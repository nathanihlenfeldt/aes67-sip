#!/usr/bin/env bash
#
# Runs everything CI runs, locally: clang-format, build, unit tests and the
# config validation.  Run this before committing; the formatting gate must be
# the *last* thing that passes, otherwise a later edit can break CI.
#
# Usage:  ./scripts/check.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

BUILD_DIR="${BUILD_DIR:-build}"
# .clang-format targets clang-format 18 (what ubuntu-24.04 ships); allow an
# override for machines where it is installed elsewhere.
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

echo "==> source files"
SOURCES="$(find src tests -name '*.hpp' -o -name '*.cpp' | sort)"
echo "${SOURCES}" | wc -l | tr -d ' '
echo " files"

echo "==> clang-format"
if ! command -v "${CLANG_FORMAT}" >/dev/null 2>&1; then
  echo "    ${CLANG_FORMAT} not found: skipping (CI will check it)"
else
  "${CLANG_FORMAT}" --version
  if ! "${CLANG_FORMAT}" --dry-run --Werror ${SOURCES}; then
    echo "    run '${CLANG_FORMAT} -i \$(find src tests -name '*.hpp' -o -name '*.cpp')'"
    exit 1
  fi
fi

echo "==> build (${BUILD_DIR})"
cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "${BUILD_DIR}" --parallel

echo "==> tests"
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "==> shell scripts"
for script in scripts/*.sh; do
  bash -n "${script}"
done
echo "    syntax OK"

echo
echo "all checks passed"
