#!/usr/bin/env bash
# Plays a game in the Lunarstone window and
# records the session for looking at afterwards: the capture (with its
# teleport hops) and everything the window logged, in a folder of its own
# under ~/.local/share/lunarstone/sessions. Recordings hold the session's
# keys, so the folder is private to you and stays out of the repository.
#
#   tools/play.sh <place-id> --mute  any place, any extra lunarstone flags
#   tools/play.sh '<link>'           a private server, by its share link (quote it)
#   tools/play.sh --replay <folder>  watch a recorded session again
set -euo pipefail
cd "$(dirname "$0")/.."

lunarstone=build/release/app/lunarstone
sessions="${XDG_DATA_HOME:-$HOME/.local/share}/lunarstone/sessions"
case "${1:-}" in
    ""|-h|--help)
        printf 'Usage: %s {<place-id>|<private-server-link>|--replay <folder>} [options]\n' "$0"
        exit 0
        ;;
esac

echo "== building"
cmake --build build/release > /dev/null

if [[ "${1:-}" == "--replay" ]]; then
    folder="${2:?which session folder? (ls $sessions)}"
    shift 2
    exec "$lunarstone" --net-play --replay "$folder/session" --graphics max "$@"
fi

if [[ "$1" =~ ^[0-9]+$ ]]; then
    join=(--net-play "$1")
    label="$1"
    shift
elif [[ "$1" =~ ^https?:// ]]; then
    join=(--net-play --private "$1")
    label=private
    shift
else
    printf 'Expected a place ID or private server link. See %s --help.\n' "$0" >&2
    exit 2
fi

folder="$sessions/$(date +%Y-%m-%d_%H-%M-%S)_$label"
mkdir -p "$folder"
chmod 700 "$sessions" "$folder"

echo "== playing $label, recording to $folder"
echo "   (waits here while another live join holds the lock)"
status=0
flock /tmp/lunarstone-live-join.lock \
    "$lunarstone" "${join[@]}" --graphics max --out "$folder/session" "$@" 2>&1 \
    | tee "$folder/log.txt" || status=$?

echo
echo "== recorded to $folder"
echo "   watch it again: tools/play.sh --replay $folder"
exit "$status"
