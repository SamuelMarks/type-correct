#!/usr/bin/env bash
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
BUILD_DIR="${1:-${BUILD_DIR:-${ROOT}/build}}"

if [[ "${BUILD_DIR}" != /* ]]; then
  BUILD_DIR="${ROOT}/${BUILD_DIR}"
fi

CMAKE_EXTRA_ARGS=()
if [[ -n "${CT_Clang_INSTALL_DIR:-}" ]]; then
  CMAKE_EXTRA_ARGS+=("-DCT_Clang_INSTALL_DIR=${CT_Clang_INSTALL_DIR}")
elif [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  if command -v llvm-config >/dev/null 2>&1; then
    CMAKE_EXTRA_ARGS+=("-DCT_Clang_INSTALL_DIR=$(llvm-config --prefix)")
  elif [[ -d "/opt/homebrew/opt/llvm" ]]; then
    CMAKE_EXTRA_ARGS+=("-DCT_Clang_INSTALL_DIR=/opt/homebrew/opt/llvm")
  elif [[ -d "/usr/local/opt/llvm" ]]; then
    CMAKE_EXTRA_ARGS+=("-DCT_Clang_INSTALL_DIR=/usr/local/opt/llvm")
  fi
fi

cmake -S "${ROOT}" -B "${BUILD_DIR}" \
  -DTYPE_CORRECT_ENABLE_COVERAGE=ON \
  -DTYPE_CORRECT_ENABLE_DOC_COVERAGE=ON \
  ${CMAKE_EXTRA_ARGS[@]+"${CMAKE_EXTRA_ARGS[@]}"}

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
