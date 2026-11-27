#!/usr/bin/env bash
# Everything that should pass before a commit: formatting, a warning-free
# sanitizer build, the tests and clang-tidy (on what changed; --all lints
# everything, see tools/lint.sh).
set -euo pipefail
cd "$(dirname "$0")/.."

step() { printf '\n== %s\n' "$1"; }

step "clang-format"
git ls-files '*.cpp' '*.hpp' | xargs clang-format --dry-run --Werror

step "build (sanitizers, -Werror)"
cmake --preset check > /dev/null
cmake --build --preset check

step "tests"
ctest --preset check

step "clang-tidy"
LUNAR_BUILD_DIR=build/check ./tools/lint.sh "$@" -warnings-as-errors='*'

printf '\nAll checks passed.\n'
