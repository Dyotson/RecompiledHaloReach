#!/usr/bin/env bash
# Start the game for interactive, scripted stepping (see tools/live_step.sh).
#
# Usage: tools/live_session.sh DIR [seconds] [extra reach flags...]
#   DIR receives the input FIFO (in.fifo), frame dumps, run.out and the log (reach.log).
#   Inputs written to DIR/in.fifo, one per line ("A", "UP", "START:0.5"), are pressed as
#   they arrive; touching DIR/dump.trigger dumps the next frame (guest memory, what
#   tools/live_step.sh uses); tools/live_shot.sh DIR saves the host's output image.
#
# Environment:
#   REACH_INSTANCE=N  (N >= 2) another instance on the same machine: its own profile and
#                     saves (XDG_DATA_HOME=DIR/data), XUID and gamertag (override with
#                     REACH_XUID / REACH_GAMERTAG). Use REACH_NET=1 on both for System Link,
#                     or REACH_SERVER=host[:port] for a Reach Live server.
#   REXSDK            SDK prefix (default: the patched overlay, as tools/run_reach.sh).
# The game runs in the background; stop it with kill -INT on its PID (never pkill -f).
set -u
[ $# -ge 1 ] || { echo "usage: $0 DIR [seconds] [extra reach flags...]  (quote paths: the repo path has spaces)" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$(mkdir -p "$1" && cd "$1" && pwd)"
SECS="${2:-1800}"
shift 2 2>/dev/null || shift $#
rm -f "$DIR/in.fifo" "$DIR/dump.trigger" "$DIR"/reach_frame_*.bin "$DIR"/reach_frame_*.json
# Guest memory files (5 GB each) of instances that were killed or crashed: left behind,
# they fill /dev/shm and later instances die with SIGBUS.
for shm in /dev/shm/xenia_memory_*; do
    [ -O "$shm" ] || continue
    grep -qs "$(basename "$shm")" /proc/[0-9]*/maps || rm -f -- "$shm"
done
mkfifo "$DIR/in.fifo"

N="${REACH_INSTANCE:-1}"
if [ "$N" != 1 ]; then
    export XDG_DATA_HOME="$DIR/data"
    if [ -z "${REACH_SERVER:-}" ]; then  # a Reach Live server gives each its own identity
        export REACH_XUID="${REACH_XUID:-E00000000000B00$N}"
        export REACH_GAMERTAG="${REACH_GAMERTAG:-Spartan$N}"
    fi
fi
DEFAULT_SDK="$HOME/rexglue-sdk-patched/0.10.0.24/linux-amd64"
[ -d "$DEFAULT_SDK/lib" ] || DEFAULT_SDK="$HOME/rexglue-sdk-nightly/0.10.0.24/linux-amd64"
cd "$ROOT/reach-recomp/out/build/${REACH_BUILD:-linux-nightly}"
SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 \
REACH_AUTOPRESS="${REACH_AUTOPRESS-9:START}" REACH_AUTOPRESS_FIFO="$DIR/in.fifo" \
REACH_FRAMEDUMP_TRIGGER="$DIR/dump.trigger" REACH_FRAMEDUMP_DIR="$DIR" \
REACH_SCREENSHOT_TRIGGER="$DIR/shot.trigger" \
LD_LIBRARY_PATH="${REXSDK:-$DEFAULT_SDK}/lib:$PWD" nohup timeout --signal=INT "$SECS" \
    ./reach --game_data_root="$ROOT/extracted/xbox360" --log_file="$DIR/reach.log" \
    --log_level="${REACH_LOG_LEVEL:-info}" --vulkan_readback_resolve=true "$@" > "$DIR/run.out" 2>&1 &
echo $! > "$DIR/pid"  # the `timeout` wrapper; the game is its child
echo "started pid $! in $DIR"
