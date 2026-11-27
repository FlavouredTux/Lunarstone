#!/usr/bin/env bash
# Runs clang-tidy. Needs a configured build, build/debug unless
# LUNAR_BUILD_DIR says otherwise.
#
#   tools/lint.sh [--all] [run-clang-tidy arguments, e.g. -fix]
#
# By default only what changed against HEAD (or LUNAR_LINT_BASE, e.g. main
# on a branch): changed and new sources, plus the sources that include a
# changed header, since clang-tidy sees headers only through them. --all
# lints the whole project, for occasional sweeps.
set -euo pipefail
cd "$(dirname "$0")/.."

all=false
if [[ ${1:-} == --all ]]; then
    all=true
    shift
fi

src=${LUNAR_BUILD_DIR:-build/debug}/compile_commands.json
out=build/tidy
[[ -f $src ]] || { echo "configure first: cmake --preset debug" >&2; exit 1; }
mkdir -p "$out"

# Clang doesn't know GCC's reflection flag or CMake's module-scanning flags,
# and compiler warnings are GCC's job.
sed -E 's/ -freflection| -fmodules-ts| -fmodule-mapper=[^ "]*| -fdeps-format=[^ "]*| -Werror//g' "$src" > "$out/compile_commands.json"

dirs='engine|terrain|physics|platform|render|script|assets|avatar|ui|net|app|tests'
if $all; then
    files="$PWD/($dirs)/"
else
    changed=$( (git diff --name-only "${LUNAR_LINT_BASE:-HEAD}" --; git ls-files --others --exclude-standard) \
        | grep -E "^($dirs)/" | sort -u || true)
    sources=$(grep -E '\.cpp$' <<< "$changed" || true)
    while IFS= read -r header; do
        [[ -n $header && -f $header ]] || continue
        # Public headers are included as <lunar/...>, private ones by name
        # from the sources next to them.
        if [[ $header == */include/* ]]; then
            users=$(git grep -l -F "${header#*/include/}" -- '*.cpp' || true)
        else
            users=$(git grep -l -F "\"$(basename "$header")\"" -- "$(dirname "$header")/*.cpp" || true)
        fi
        sources+=$'\n'$users
    done <<< "$(grep -E '\.hpp$' <<< "$changed" || true)"
    sources=$(grep -E "^($dirs)/.*\.cpp$" <<< "$sources" | sort -u || true)
    [[ -n $sources ]] || { echo "nothing changed to lint"; exit 0; }
    echo "linting $(wc -l <<< "$sources") changed source(s)"
    # run-clang-tidy takes regular expressions: escape the paths (only dots
    # and pluses turn up in them) and join them.
    files="^($(sed -e 's/[.+]/\\&/g' -e "s|^|$PWD/|" <<< "$sources" | paste -sd '|'))\$"
fi

# Not every core: several builds often share this machine.
run-clang-tidy -quiet -j "${LUNAR_JOBS:-8}" -p "$out" -extra-arg=-Wno-unknown-warning-option "$@" "$files"
