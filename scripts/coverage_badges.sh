#!/usr/bin/env bash
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
BUILD_DIR="${1:-${BUILD_DIR:-${ROOT}/build}}"

if [[ "${BUILD_DIR}" != /* ]]; then
  BUILD_DIR="${ROOT}/${BUILD_DIR}"
fi

cmake -S "${ROOT}" -B "${BUILD_DIR}" \
  -DTYPE_CORRECT_ENABLE_COVERAGE=ON \
  -DTYPE_CORRECT_ENABLE_DOC_COVERAGE=ON

cmake --build "${BUILD_DIR}" --target coverage
cmake --build "${BUILD_DIR}" --target doc-coverage

python3 "${ROOT}/scripts/update_coverage_badges.py" \
  --build-dir "${BUILD_DIR}" \
  --readme "${ROOT}/README.md" \
  --fail-under-doc 100 \
  --fail-under-test 100

if [[ -f "${BUILD_DIR}/coverage.txt" ]]; then
  echo "Coverage summary:"
  grep -E "^TOTAL" "${BUILD_DIR}/coverage.txt" || true
fi
