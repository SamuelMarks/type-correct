#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-$(pwd)}"
PROFRAW_DIR="${BUILD_DIR}/coverage"
PROFDATA_FILE="${BUILD_DIR}/coverage.profdata"
REPORT_FILE="${BUILD_DIR}/coverage.txt"

mkdir -p "${PROFRAW_DIR}"
rm -f "${PROFRAW_DIR}"/*.profraw "${PROFDATA_FILE}" "${REPORT_FILE}"

export LLVM_PROFILE_FILE="${PROFRAW_DIR}/%p.profraw"

ctest --output-on-failure

find_llvm_tool() {
  local tool="$1"
  if command -v "${tool}" >/dev/null 2>&1; then
    command -v "${tool}"
    return 0
  fi
  if command -v llvm-config >/dev/null 2>&1; then
    local bindir
    bindir="$(llvm-config --bindir)"
    if [[ -x "${bindir}/${tool}" ]]; then
      echo "${bindir}/${tool}"
      return 0
    fi
  fi
  if [[ -x "/opt/homebrew/opt/llvm/bin/${tool}" ]]; then
    echo "/opt/homebrew/opt/llvm/bin/${tool}"
    return 0
  fi
  if [[ -x "/usr/local/opt/llvm/bin/${tool}" ]]; then
    echo "/usr/local/opt/llvm/bin/${tool}"
    return 0
  fi
  return 1
}

LLVM_PROFDATA="$(find_llvm_tool llvm-profdata || true)"
LLVM_COV="$(find_llvm_tool llvm-cov || true)"
if [[ -z "${LLVM_PROFDATA}" ]]; then
  echo "llvm-profdata not found (install LLVM or add it to PATH)" >&2
  exit 1
fi
if [[ -z "${LLVM_COV}" ]]; then
  echo "llvm-cov not found (install LLVM or add it to PATH)" >&2
  exit 1
fi

"${LLVM_PROFDATA}" merge -sparse "${PROFRAW_DIR}"/*.profraw -o "${PROFDATA_FILE}"

OBJECTS=()
if [[ -f "${BUILD_DIR}/bin/test_type_correct" ]]; then
  OBJECTS+=("${BUILD_DIR}/bin/test_type_correct")
fi
if [[ -f "${BUILD_DIR}/bin/type_correct_cli" ]]; then
  OBJECTS+=("${BUILD_DIR}/bin/type_correct_cli")
fi
if [[ -f "${BUILD_DIR}/lib/libtype_correct.dylib" ]]; then
  OBJECTS+=("${BUILD_DIR}/lib/libtype_correct.dylib")
elif [[ -f "${BUILD_DIR}/lib/libtype_correct.so" ]]; then
  OBJECTS+=("${BUILD_DIR}/lib/libtype_correct.so")
fi

if [[ ${#OBJECTS[@]} -eq 0 ]]; then
  echo "No coverage objects found under ${BUILD_DIR}/bin or ${BUILD_DIR}/lib" >&2
  exit 1
fi

IGNORE_REGEX=".*/(build|_deps|tests)/.*"

"${LLVM_COV}" report "${OBJECTS[@]}" \
  -instr-profile="${PROFDATA_FILE}" \
  -ignore-filename-regex="${IGNORE_REGEX}" | tee "${REPORT_FILE}"

LINE_COVERAGE=$(awk '$1 == "TOTAL" {print $(NF-3)}' "${REPORT_FILE}" | tr -d '%')
BRANCH_COVERAGE=$(awk '$1 == "TOTAL" {print $NF}' "${REPORT_FILE}" | tr -d '%')
if [[ -z "${LINE_COVERAGE}" ]]; then
  echo "Failed to parse TOTAL line coverage from ${REPORT_FILE}" >&2
  exit 1
fi

if [[ "${LINE_COVERAGE}" != "100.00" && "${LINE_COVERAGE}" != "100" ]]; then
  echo "Line coverage is ${LINE_COVERAGE}%, expected 100%" >&2
  exit 1
fi

if [[ -z "${BRANCH_COVERAGE}" || "${BRANCH_COVERAGE}" == "-" ]]; then
  echo "Failed to parse TOTAL branch coverage from ${REPORT_FILE}" >&2
  exit 1
fi

if [[ "${BRANCH_COVERAGE}" != "100.00" && "${BRANCH_COVERAGE}" != "100" ]]; then
  echo "Branch coverage is ${BRANCH_COVERAGE}%, expected 100%" >&2
  exit 1
fi

echo "Line coverage is 100%"
echo "Branch coverage is 100%"
