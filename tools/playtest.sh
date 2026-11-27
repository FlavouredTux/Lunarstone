#!/usr/bin/env bash
# Builds a Rojo project into a place and plays its tests without a window
# (lunarstone --playtest, see app/Playtest.hpp): every playtests/*.luau in the
# project, or the ones named, side by side. Each test's log and pictures go to
# a folder of its own under ~/.cache/lunarstone/playtest/<project>/; the
# summary says which passed and where to look at the ones that didn't.
#
#   tools/playtest.sh ~/graveyard-shift                all of its tests
#   tools/playtest.sh ~/graveyard-shift walk task      just these
#   tools/playtest.sh ~/graveyard-shift -- --seed 7    lunarstone flags after --
set -euo pipefail
cd "$(dirname "$0")/.."

project="$(realpath "${1:?which Rojo project?}")"
shift
names=()
while [[ $# -gt 0 && "$1" != "--" ]]; do
    names+=("$1")
    shift
done
[[ "${1:-}" == "--" ]] && shift
flags=("$@")

lunarstone="$PWD/build/release/app/lunarstone"
out="${XDG_CACHE_HOME:-$HOME/.cache}/lunarstone/playtest/$(basename "$project")"
mkdir -p "$out"

tests=()
if [[ ${#names[@]} -eq 0 ]]; then
    shopt -s nullglob
    tests=("$project"/playtests/*.luau)
else
    for name in "${names[@]}"; do
        tests+=("$project/playtests/${name%.luau}.luau")
    done
fi
if [[ ${#tests[@]} -eq 0 ]]; then
    echo "no tests: put them in $project/playtests/*.luau" >&2
    exit 2
fi

echo "== building lunarstone"
cmake --build build/release > "$out/build.log" 2>&1 || { tail -20 "$out/build.log" >&2; exit 2; }
echo "== rojo build $(basename "$project")"
(cd "$project" && rojo build -o "$out/place.rbxlx") > "$out/rojo.log" 2>&1 || { cat "$out/rojo.log" >&2; exit 2; }

echo "== ${#tests[@]} test(s)"
pids=()
for test in "${tests[@]}"; do
    name="$(basename "$test" .luau)"
    rm -rf "${out:?}/$name"
    "$lunarstone" --playtest "$out/place.rbxlx" "$test" --out "$out/$name" "${flags[@]}" \
        > "$out/$name.stdout" 2>&1 &
    pids+=($!)
done

failed=0
for i in "${!tests[@]}"; do
    name="$(basename "${tests[$i]}" .luau)"
    if wait "${pids[$i]}"; then
        echo "PASS $name"
    else
        failed=$((failed + 1))
        # The verdict and the seed, or how it couldn't run.
        verdict="$(grep -E '\[test\] (FAIL|again)' "$out/$name/log.txt" 2>/dev/null | sed 's/^ *[0-9.]* \[test\] //')"
        echo "FAIL $name"
        echo "${verdict:-$(tail -5 "$out/$name.stdout")}" | sed 's/^/     /'
        echo "     log: $out/$name/log.txt"
    fi
done
echo "== $((${#tests[@]} - failed)) passed, $failed failed; pictures in $out/<test>/"
[[ $failed -eq 0 ]]
