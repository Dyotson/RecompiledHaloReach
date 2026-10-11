#!/usr/bin/env bash
# Press inputs in a game started by tools/live_session.sh, wait, and dump a frame.
#
# Usage: tools/live_step.sh DIR WAIT [INPUT...]
#   Sends each INPUT 0.8 s apart, waits WAIT seconds, dumps a frame and writes
#   DIR/last_full.png and DIR/last.png (768x480). Prints the dump's file name.
#   With no new dump within 10 s (the game stopped presenting) it prints "no frame".
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -ge 2 ] || { echo "usage: $0 DIR WAIT [INPUT...]  (quote paths: the repo path has spaces)" >&2; exit 2; }
DIR="$1"; WAIT="$2"; shift 2
# Fail fast when the game is gone: a write to a FIFO with no reader blocks forever.
pid="$(cat "$DIR/pid" 2>/dev/null || true)"
if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
    echo "error: the game in $DIR is not running (pid ${pid:-unknown}); start it with tools/live_session.sh" >&2
    exit 3
fi
for input in "$@"; do
    # Bounded write: the game may exit between the check and the write.
    if ! timeout 5 bash -c 'printf "%s\n" "$1" > "$2"' _ "$input" "$DIR/in.fifo"; then
        echo "error: could not write '$input' to $DIR/in.fifo within 5 s (the game stopped reading)" >&2
        exit 3
    fi
    sleep 0.8
done
sleep "$WAIT"
before=$(ls "$DIR"/reach_frame_*.bin 2>/dev/null | wc -l)
touch "$DIR/dump.trigger"
for _ in $(seq 1 40); do
    [ "$(ls "$DIR"/reach_frame_*.bin 2>/dev/null | wc -l)" -gt "$before" ] && break
    sleep 0.25
done
if [ "$(ls "$DIR"/reach_frame_*.bin 2>/dev/null | wc -l)" -le "$before" ]; then
    echo "no frame"; exit 1
fi
f=$(ls -t "$DIR"/reach_frame_*.bin | head -1); sleep 0.3
python3 "$ROOT/tools/frame_to_png.py" "$f" "$DIR/last_full.png" > /dev/null
python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).convert('RGB').resize((768,480)).save(sys.argv[2])" "$DIR/last_full.png" "$DIR/last.png"
basename "$f"
