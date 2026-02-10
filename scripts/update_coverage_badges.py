#!/usr/bin/env python3
import argparse
import json
import os
import re
import sys
import urllib.parse
import xml.etree.ElementTree as ET
from typing import Any, Dict, Iterable, List, Tuple


JSDOC_KINDS = {
    "class",
    "constant",
    "enum",
    "event",
    "external",
    "function",
    "interface",
    "member",
    "mixin",
    "module",
    "namespace",
    "typedef",
}

TYPEDOC_KINDS = {
    "Accessor",
    "Class",
    "Constructor",
    "Enumeration",
    "Enumeration member",
    "Enum",
    "Enum member",
    "Function",
    "Interface",
    "Method",
    "Module",
    "Namespace",
    "Object literal",
    "Property",
    "Type alias",
    "Variable",
}

OPENAPI_METHODS = {
    "get",
    "put",
    "post",
    "delete",
    "options",
    "head",
    "patch",
    "trace",
}

SUPPORTED_GENERATORS = ("doxygen", "jsdoc", "typedoc", "openapi", "coverage-json")


def _has_text(elem: ET.Element) -> bool:
    if elem is None:
        return False
    if elem.text and elem.text.strip():
        return True
    for child in elem:
        if _has_text(child):
            return True
    return False


def _member_documented(elem: ET.Element) -> bool:
    return _has_text(elem.find("briefdescription")) or _has_text(
        elem.find("detaileddescription")
    )


def _has_text_value(value: Any) -> bool:
    if value is None:
        return False
    if isinstance(value, str):
        return bool(value.strip())
    if isinstance(value, list):
        return any(_has_text_value(item) for item in value)
    if isinstance(value, dict):
        return any(_has_text_value(item) for item in value.values())
    return False


def compute_doxygen_counts(xml_dir: str) -> Tuple[int, int]:
    total = 0
    documented = 0

    for name in os.listdir(xml_dir):
        if not name.endswith(".xml"):
            continue
        if name in {"index.xml", "Doxyfile.xml"}:
            continue
        path = os.path.join(xml_dir, name)
        try:
            tree = ET.parse(path)
        except ET.ParseError as exc:
            raise RuntimeError(f"Failed to parse {path}: {exc}") from exc
        root = tree.getroot()
        for compound in root.findall("compounddef"):
            for member in compound.findall(".//memberdef"):
                prot = member.get("prot")
                if prot is not None and prot != "public":
                    continue
                total += 1
                if _member_documented(member):
                    documented += 1
                if member.get("kind") == "enum":
                    for enum_val in member.findall("enumvalue"):
                        total += 1
                        if _member_documented(enum_val):
                            documented += 1

    return documented, total


def compute_jsdoc_counts(report_path: str) -> Tuple[int, int]:
    with open(report_path, "r", encoding="utf-8") as handle:
        doclets = json.load(handle)
    if not isinstance(doclets, list):
        raise RuntimeError(f"JSDoc report must be a list: {report_path}")

    total = 0
    documented = 0

    for doclet in doclets:
        if not isinstance(doclet, dict):
            continue
        if doclet.get("ignore"):
            continue
        kind = doclet.get("kind")
        if kind not in JSDOC_KINDS:
            continue
        access = doclet.get("access")
        if access in {"private", "protected"}:
            continue
        total += 1
        if doclet.get("undocumented"):
            continue
        if _has_text_value(doclet.get("description")) or _has_text_value(
            doclet.get("classdesc")
        ):
            documented += 1
            continue
        if _has_text_value(doclet.get("summary")):
            documented += 1
            continue
        for param in doclet.get("params") or []:
            if _has_text_value(param.get("description")):
                documented += 1
                break
        else:
            for ret in doclet.get("returns") or doclet.get("return") or []:
                if _has_text_value(ret.get("description")):
                    documented += 1
                    break

    return documented, total


def _typedoc_comment_has_text(comment: Dict[str, Any]) -> bool:
    if not comment or not isinstance(comment, dict):
        return False
    if _has_text_value(comment.get("shortText")) or _has_text_value(
        comment.get("text")
    ):
        return True
    if _has_text_value(comment.get("returns")):
        return True
    summary = comment.get("summary")
    if isinstance(summary, list):
        for part in summary:
            if _has_text_value(part.get("text") if isinstance(part, dict) else part):
                return True
    block_tags = comment.get("blockTags")
    if isinstance(block_tags, list):
        for tag in block_tags:
            if not isinstance(tag, dict):
                continue
            content = tag.get("content")
            if isinstance(content, list):
                for part in content:
                    if _has_text_value(
                        part.get("text") if isinstance(part, dict) else part
                    ):
                        return True
            elif _has_text_value(content):
                return True
    return False


def _typedoc_signature_has_text(reflection: Dict[str, Any]) -> bool:
    for signature in reflection.get("signatures") or []:
        if isinstance(signature, dict) and _typedoc_comment_has_text(
            signature.get("comment")
        ):
            return True
    for key in ("getSignature", "setSignature"):
        signature = reflection.get(key)
        if isinstance(signature, dict) and _typedoc_comment_has_text(
            signature.get("comment")
        ):
            return True
    return False


def _typedoc_is_public(reflection: Dict[str, Any]) -> bool:
    flags = reflection.get("flags") or {}
    if flags.get("isPrivate") or flags.get("isProtected") or flags.get("isExternal"):
        return False
    return True


def _iter_typedoc_reflections(node: Any) -> Iterable[Dict[str, Any]]:
    if isinstance(node, dict):
        yield node
        for child in node.get("children") or []:
            yield from _iter_typedoc_reflections(child)
    elif isinstance(node, list):
        for child in node:
            yield from _iter_typedoc_reflections(child)


def compute_typedoc_counts(report_path: str) -> Tuple[int, int]:
    with open(report_path, "r", encoding="utf-8") as handle:
        data = json.load(handle)

    total = 0
    documented = 0

    for reflection in _iter_typedoc_reflections(data):
        kind = reflection.get("kindString")
        if kind not in TYPEDOC_KINDS:
            continue
        if not _typedoc_is_public(reflection):
            continue
        total += 1
        if _typedoc_comment_has_text(reflection.get("comment")) or _typedoc_signature_has_text(
            reflection
        ):
            documented += 1

    return documented, total


def _load_openapi_spec(report_path: str) -> Dict[str, Any]:
    ext = os.path.splitext(report_path)[1].lower()
    with open(report_path, "r", encoding="utf-8") as handle:
        if ext in {".yaml", ".yml"}:
            try:
                import yaml  # type: ignore
            except ImportError as exc:
                raise RuntimeError(
                    "PyYAML is required to parse YAML OpenAPI specs."
                ) from exc
            return yaml.safe_load(handle)
        return json.load(handle)


def compute_openapi_counts(report_path: str) -> Tuple[int, int]:
    spec = _load_openapi_spec(report_path)
    if not isinstance(spec, dict):
        raise RuntimeError(f"OpenAPI spec must be a JSON/YAML object: {report_path}")

    paths = spec.get("paths") or {}
    total = 0
    documented = 0

    for path_item in paths.values():
        if not isinstance(path_item, dict):
            continue
        for method, operation in path_item.items():
            if method.lower() not in OPENAPI_METHODS:
                continue
            if not isinstance(operation, dict):
                continue
            total += 1
            if _has_text_value(operation.get("summary")) or _has_text_value(
                operation.get("description")
            ):
                documented += 1

    return documented, total


def compute_custom_counts(report_path: str) -> Tuple[int, int]:
    with open(report_path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise RuntimeError(
            f"Custom doc coverage report must be a JSON object: {report_path}"
        )

    if "documented" in data and "total" in data:
        return int(data["documented"]), int(data["total"])
    if "covered" in data and "total" in data:
        return int(data["covered"]), int(data["total"])
    if "undocumented" in data and "total" in data:
        total = int(data["total"])
        return total - int(data["undocumented"]), total

    raise RuntimeError(
        f"Custom doc coverage report missing keys (documented/covered + total): {report_path}"
    )


def parse_test_coverage(report_path: str) -> float:
    with open(report_path, "r", encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("TOTAL"):
                parts = line.split()
                if len(parts) < 5:
                    raise RuntimeError(
                        f"Unexpected TOTAL line format in {report_path}"
                    )
                value = parts[-4].rstrip("%")
                return float(value)
    raise RuntimeError(f"TOTAL line not found in {report_path}")


def format_percent(value: float) -> str:
    if abs(value - round(value)) < 0.005:
        return f"{int(round(value))}%"
    return f"{value:.2f}%"


def badge_color(value: float) -> str:
    if value >= 95.0:
        return "brightgreen"
    if value >= 85.0:
        return "green"
    if value >= 70.0:
        return "yellowgreen"
    if value >= 50.0:
        return "orange"
    return "red"


def badge_url(label: str, percent_text: str, color: str) -> str:
    return "https://img.shields.io/badge/{}-{}-{}".format(
        urllib.parse.quote(label), urllib.parse.quote(percent_text), color
    )


def update_readme(readme_path: str, doc_pct: float, test_pct: float) -> bool:
    start_marker = "<!-- COVERAGE_BADGES_START -->"
    end_marker = "<!-- COVERAGE_BADGES_END -->"

    doc_text = format_percent(doc_pct)
    test_text = format_percent(test_pct)
    doc_badge = f"![Doc Coverage]({badge_url('doc coverage', doc_text, badge_color(doc_pct))})"
    test_badge = (
        f"![Test Coverage]({badge_url('test coverage', test_text, badge_color(test_pct))})"
    )
    block_lines = [start_marker, doc_badge, test_badge, end_marker]
    block = "\n".join(block_lines)

    with open(readme_path, "r", encoding="utf-8") as handle:
        content = handle.read()

    if start_marker in content and end_marker in content:
        pattern = re.compile(
            re.escape(start_marker) + r".*?" + re.escape(end_marker),
            re.DOTALL,
        )
        updated = pattern.sub(block, content)
    else:
        lines = content.splitlines()
        badge_re = re.compile(r"^\s*(\[!\[.*\]\(.*\)\]\(.*\)|!\[.*\]\(.*\))\s*$")
        insert_at = None
        for idx, line in enumerate(lines):
            if badge_re.match(line):
                insert_at = idx + 1
        if insert_at is None:
            insert_at = 0
        lines.insert(insert_at, block)
        updated = "\n".join(lines) + ("\n" if content.endswith("\n") else "")

    if updated != content:
        with open(readme_path, "w", encoding="utf-8") as handle:
            handle.write(updated)
        return True
    return False


def resolve_doc_path(generator: str, path: str, build_dir: str) -> str:
    if os.path.isabs(path):
        return path
    if generator == "doxygen":
        return os.path.abspath(os.path.join(build_dir, path))
    return os.path.abspath(path)


def parse_doc_sources(
    sources: List[str], generator: str, doc_path: str, build_dir: str
) -> List[Tuple[str, str]]:
    if sources:
        parsed = []
        for source in sources:
            if "=" in source:
                name, raw_path = source.split("=", 1)
            elif ":" in source:
                name, raw_path = source.split(":", 1)
            else:
                raise RuntimeError(
                    f"Doc source must be generator=path or generator:path (got {source})"
                )
            name = name.strip().lower()
            raw_path = raw_path.strip()
            if not name or not raw_path:
                raise RuntimeError(f"Doc source missing generator or path: {source}")
            if name not in SUPPORTED_GENERATORS:
                raise RuntimeError(
                    f"Unsupported doc generator '{name}'. Supported: {', '.join(SUPPORTED_GENERATORS)}"
                )
            parsed.append((name, resolve_doc_path(name, raw_path, build_dir)))
        return parsed

    if not doc_path:
        if generator == "doxygen":
            doc_path = os.path.join(build_dir, "docs", "xml")
        else:
            raise RuntimeError(
                f"--doc-path is required for generator '{generator}'."
            )
    if generator not in SUPPORTED_GENERATORS:
        raise RuntimeError(
            f"Unsupported doc generator '{generator}'. Supported: {', '.join(SUPPORTED_GENERATORS)}"
        )
    return [(generator, resolve_doc_path(generator, doc_path, build_dir))]


def compute_doc_coverage(doc_sources: List[Tuple[str, str]]) -> float:
    documented_total = 0
    total_total = 0

    for generator, path in doc_sources:
        if generator == "doxygen":
            if not os.path.isdir(path):
                raise RuntimeError(f"Doxygen XML directory not found: {path}")
            documented, total = compute_doxygen_counts(path)
        elif generator == "jsdoc":
            if not os.path.isfile(path):
                raise RuntimeError(f"JSDoc JSON file not found: {path}")
            documented, total = compute_jsdoc_counts(path)
        elif generator == "typedoc":
            if not os.path.isfile(path):
                raise RuntimeError(f"TypeDoc JSON file not found: {path}")
            documented, total = compute_typedoc_counts(path)
        elif generator == "openapi":
            if not os.path.isfile(path):
                raise RuntimeError(f"OpenAPI spec file not found: {path}")
            documented, total = compute_openapi_counts(path)
        elif generator == "coverage-json":
            if not os.path.isfile(path):
                raise RuntimeError(f"Custom coverage JSON file not found: {path}")
            documented, total = compute_custom_counts(path)
        else:
            raise RuntimeError(f"Unsupported doc generator: {generator}")

        documented_total += documented
        total_total += total

    if total_total == 0:
        return 100.0
    return (documented_total / total_total) * 100.0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Update README coverage badges from build artifacts."
    )
    parser.add_argument(
        "--build-dir",
        default="build",
        help="Path to the CMake build directory (default: build).",
    )
    parser.add_argument(
        "--readme",
        default="README.md",
        help="Path to README markdown file.",
    )
    parser.add_argument(
        "--doc-generator",
        default="doxygen",
        choices=SUPPORTED_GENERATORS,
        help=(
            "Doc generator source to parse (doxygen, jsdoc, typedoc, openapi, coverage-json). "
            "Ignored if --doc-source is provided."
        ),
    )
    parser.add_argument(
        "--doc-path",
        default="",
        help=(
            "Path to doc generator output (Doxygen XML directory, JSON file, or OpenAPI spec). "
            "Defaults to build/docs/xml for Doxygen."
        ),
    )
    parser.add_argument(
        "--doc-source",
        action="append",
        default=[],
        help=(
            "Add a doc source as generator=path (repeatable). "
            "Example: --doc-source doxygen=build/docs/xml --doc-source typedoc=docs/typedoc.json"
        ),
    )
    parser.add_argument(
        "--fail-under-doc",
        type=float,
        default=None,
        help="Fail if documentation coverage is below this percentage.",
    )
    parser.add_argument(
        "--fail-under-test",
        type=float,
        default=None,
        help="Fail if test coverage is below this percentage.",
    )
    args = parser.parse_args()

    build_dir = os.path.abspath(args.build_dir)
    report_path = os.path.join(build_dir, "coverage.txt")
    generator = args.doc_generator.strip().lower()

    if not os.path.isfile(report_path):
        raise RuntimeError(f"Coverage report not found: {report_path}")

    doc_sources = parse_doc_sources(
        args.doc_source, generator, args.doc_path, build_dir
    )
    doc_pct = compute_doc_coverage(doc_sources)
    test_pct = parse_test_coverage(report_path)

    changed = update_readme(args.readme, doc_pct, test_pct)
    if changed:
        print(
            f"Updated coverage badges: doc={format_percent(doc_pct)}, test={format_percent(test_pct)}"
        )
    else:
        print("Coverage badges already up to date.")

    failures = []
    if args.fail_under_doc is not None and doc_pct + 1e-9 < args.fail_under_doc:
        failures.append(
            f"Doc coverage {format_percent(doc_pct)} is below {format_percent(args.fail_under_doc)}"
        )
    if args.fail_under_test is not None and test_pct + 1e-9 < args.fail_under_test:
        failures.append(
            f"Test coverage {format_percent(test_pct)} is below {format_percent(args.fail_under_test)}"
        )
    if failures:
        for msg in failures:
            print(msg, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
