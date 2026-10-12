#!/usr/bin/env bash
# Frame-time benchmark: start the game, drive it to a scene through the input FIFO, measure
# for a while and print the frame statistics summary (RECOMP_PERF, docs/perf.md).
#
# Usage: tools/bench.sh SCENARIO [--cold] [--seconds N] [--fullscreen] [reach flags...]
#   SCENARIO      menu      the main menu (title screen > MAIN MENU), idle
#                 gameplay  Custom Game lobby > START GAME with the profile's last game type
#                           and map (set SWAT on Sword Base once by hand), turning in place
#   --cold        fresh shader storage (a temporary cache_root that shares the game's
#                 cache0/cache1) and Mesa's shader cache off: every pipeline is compiled
#                 during the run. Default: warm, the caches as they are.
#   --seconds N   measured time once the scene is reached (default 30)
#   --fullscreen  fullscreen like a normal game (default: a window, so the desktop stays usable)
#   reach flags   passed on, e.g. --vulkan_allow_present_mode_immediate=true
#
# Environment: BENCH_DIR (default: a new directory under ${TMPDIR:-/tmp}) receives reach.log,
# frames.csv (every guest frame and host present), machine.txt and a screenshot of each step.
# BENCH_LOCK=<file> (with BENCH_OWNER=<name>): a shared machine's lock for clean
# measurements ("owner expiry-epoch purpose"); the run refuses to start while someone else
# holds it. While it holds the lock, it waits for other games to finish before the launch and
# again before measuring (BENCH_WAIT seconds at most, default 1500), and for the load
# average to settle below 2 before the launch.
# Prints the guest and present summary lines (covering only the measured time), the game's
# CPU use and the GPU's load (AMD) over that time, and the paths. Needs 10 GB of available
# memory.
#
# Other games skew the numbers. The machine's state (other reach/forza processes, load
# average, available memory, GPU load) is recorded before the launch and at the start and
# end of the measurement, and other games are checked every second while it runs. If any
# other game ran, the 1-minute load average before the launch was above 2 or the GPU was
# more than 20% busy, the summary lines end in CONTENDED: not a baseline (docs/perf.md).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$ROOT/tools"

SCENARIO="${1:-}"
case "$SCENARIO" in
    menu|gameplay) shift ;;
    *) sed -n '2,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 2 ;;
esac
COLD=0; SECS=30; FULLSCREEN=0; FLAGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --cold) COLD=1 ;;
        --seconds) SECS="$2"; shift ;;
        --fullscreen) FULLSCREEN=1 ;;
        *) FLAGS+=("$1") ;;
    esac
    shift
done

avail=$(free -g | awk '/^Mem:/ {print $7}')
if [ "${avail:-0}" -lt 10 ]; then
    echo "only ${avail} GB of memory available; a game needs 10" >&2; exit 1
fi
if [ -n "${BENCH_LOCK:-}" ] && [ -f "$BENCH_LOCK" ]; then
    read -r lock_owner lock_expiry lock_purpose < "$BENCH_LOCK"
    if [ "$lock_owner" != "${BENCH_OWNER:-}" ] && [ "${lock_expiry:-0}" -gt "$(date +%s)" ]; then
        echo "the machine is reserved by $lock_owner until $(date -d "@$lock_expiry" +%T) ($lock_purpose)" >&2
        exit 1
    fi
fi

PID=""
others() {  # other games running now, as "name(pid)"
    ps -C reach,forza -o pid=,comm= 2>/dev/null |
        awk -v me="${PID:-0}" '$1 != me { printf "%s%s(%s)", sep, $2, $1; sep = " " }'
}
gpu_busy() {
    local f
    f=$(ls /sys/class/drm/card*/device/gpu_busy_percent 2>/dev/null | head -1)
    if [ -n "$f" ]; then cat "$f"; else echo "?"; fi
}
snapshot() {  # snapshot LABEL: one line of machine state, also kept in DIR/machine.txt
    local o
    o=$(others)
    echo "$1: load $(cut -d' ' -f1-3 /proc/loadavg), MemAvailable $(awk '/^MemAvailable/ {printf "%.1f GB", $2 / 1048576}' /proc/meminfo), GPU $(gpu_busy)% busy, other games: ${o:-none}" |
        tee -a "$DIR/machine.txt"
}
CONTENDED=()
holds_lock() { [ -n "${BENCH_LOCK:-}" ] && [ -f "$BENCH_LOCK" ] && [ "$(cut -d' ' -f1 "$BENCH_LOCK")" = "${BENCH_OWNER:-}" ]; }
wait_alone() {  # wait_alone SECONDS: with the lock held, wait until no other game runs
    holds_lock || return 0
    local end=$((SECONDS + $1)) o said=""
    while o=$(others); [ -n "$o" ]; do
        [ $SECONDS -lt $end ] || { echo "other games still running after waiting: $o" >&2; return 1; }
        [ -n "$said" ] || { echo "waiting for other games to finish: $o" >&2; said=1; }
        sleep 5
    done
}

DIR="${BENCH_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/reach_bench_${SCENARIO}_XXXX")}"
mkdir -p "$DIR"
DIR="$(cd "$DIR" && pwd)"
[ "$FULLSCREEN" = 1 ] || FLAGS+=(--fullscreen=false)
MODE=warm
if [ "$COLD" = 1 ]; then
    MODE=cold
    # The game's own caches (cache0/cache1: preferences, map caches) stay shared; only the
    # SDK's shader storage (cache_root/shaders) starts empty.
    cache="${XDG_DATA_HOME:-$HOME/.local/share}/reach/cache"
    rm -rf "$DIR/cache"; mkdir -p "$DIR/cache/shaders"
    for part in cache0 cache1; do
        mkdir -p "$cache/$part"
        ln -s "$cache/$part" "$DIR/cache/$part"
    done
    FLAGS+=(--cache_root="$DIR/cache")
    export MESA_SHADER_CACHE_DISABLE=true
fi

rm -f "$DIR/frames.csv" "$DIR/perf.reset" "$DIR"/step_*.png "$DIR/machine.txt"
wait_alone "${BENCH_WAIT:-1500}"
# The 1-minute load average lags: let it settle after other games (up to 4 minutes).
if holds_lock; then
    for _ in $(seq 1 240); do awk '{ exit !($1 < 2) }' /proc/loadavg && break; sleep 1; done
fi
snapshot "before launch" > /dev/null
load_before=$(cut -d' ' -f1 /proc/loadavg)
gpu_before=$(gpu_busy)
awk -v l="$load_before" 'BEGIN { exit !(l > 2) }' && CONTENDED+=("load average $load_before before launch")
[ "$gpu_before" != "?" ] && [ "$gpu_before" -gt 20 ] && CONTENDED+=("GPU ${gpu_before}% busy before launch")
o=$(others); [ -n "$o" ] && CONTENDED+=("other games before launch: $o")
RECOMP_PERF=1 RECOMP_PERF_SCENE="$SCENARIO-$MODE" RECOMP_PERF_CSV="$DIR/frames.csv" \
RECOMP_PERF_RESET="$DIR/perf.reset" \
    "$TOOLS/live_session.sh" "$DIR" 900 "${FLAGS[@]}" > /dev/null
WRAPPER=$(cat "$DIR/pid")
for _ in $(seq 1 50); do
    PID=$(ps -o pid= --ppid "$WRAPPER" 2>/dev/null | tr -d ' ')
    [ -n "$PID" ] && break
    sleep 0.2
done
[ -n "$PID" ] || { echo "the game did not start; see $DIR/run.out" >&2; exit 1; }

stop() {
    kill -INT "$PID" 2>/dev/null
    for _ in $(seq 1 30); do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
    kill -KILL "$PID" 2>/dev/null
    kill "$WRAPPER" 2>/dev/null
    # Guest memory left behind by a killed game (see tools/live_session.sh).
    for shm in /dev/shm/xenia_memory_*; do
        [ -O "$shm" ] || continue
        grep -qs "$(basename "$shm")" /proc/[0-9]*/maps || rm -f -- "$shm"
    done
}
fail() { echo "$1 (last screen: $DIR/last.png)" >&2; stop; exit 1; }

n=0
step() {  # step WAIT [INPUT...]: press inputs, wait, screenshot to DIR/last.png and step_N.png
    local ok=0
    "$TOOLS/live_step.sh" "$DIR" "$@" > /dev/null 2>&1 || ok=1
    rm -f "${DIR:?}"/reach_frame_*.bin "${DIR:?}"/reach_frame_*.json  # 3 MB each
    [ $ok = 0 ] || return 1
    n=$((n + 1)); cp "$DIR/last.png" "$DIR/step_$(printf %02d $n).png"
}
state() { python3 "$TOOLS/menu_state.py" --strict "$1" "$DIR/last.png"; }
wait_for() {  # wait_for MENU SECONDS: poll until MENU shows a highlighted row; prints it
    local end=$((SECONDS + $2)) idx
    while [ $SECONDS -lt $end ]; do
        step 2 && idx=$(state "$1") && [ "$idx" != -1 ] && { echo "$idx"; return 0; }
        kill -0 "$PID" 2>/dev/null || return 1
    done
    return 1
}
select_row() {  # select_row MENU INDEX: move the highlight to INDEX (the menus wrap)
    local idx
    for _ in $(seq 1 8); do
        idx=$(state "$1")
        if [ "$idx" = "$2" ]; then return 0
        elif [ "$idx" = -1 ]; then step 1  # mid-transition: look again
        elif [ "$idx" -lt "$2" ]; then step 1 DOWN
        else step 1 UP
        fi
    done
    return 1
}

# The intro is skipped by live_session.sh's START press; the title screen follows.
wait_for title 240 > /dev/null || fail "no title screen"
select_row title 1 || fail "MAIN MENU not selectable on the title screen"
step 4 A
wait_for main 60 > /dev/null || fail "no main menu"

if [ "$SCENARIO" = gameplay ]; then
    select_row main 3 || fail "CUSTOM GAME not selectable"
    step 4 A
    wait_for lobby 60 > /dev/null || fail "no Custom Game lobby"
    select_row lobby 3 || fail "START GAME not selectable"
    cp "$DIR/last.png" "$DIR/lobby.png"
    step 5 A
    # In play once the lobby is gone and the screen shows the world (not a black load). The
    # main menu's marker test can fire on the HUD (a bright column where the marker would be),
    # so after 40 s without the lobby a bright screen counts even then.
    end=$((SECONDS + 300)); seen=0; gone=0
    while [ $SECONDS -lt $end ] && [ $seen -lt 2 ]; do
        sleep 3; step 0 || continue
        bright=$(python3 -c "from PIL import Image, ImageStat; print(int(ImageStat.Stat(Image.open('$DIR/last.png').convert('L')).mean[0]))")
        if [ "$(state lobby)" = -1 ]; then [ $gone = 0 ] && gone=$SECONDS; else gone=0; fi
        if [ "$bright" -gt 15 ] && [ $gone != 0 ] && { [ "$(state main)" = -1 ] || [ $((SECONDS - gone)) -ge 40 ]; }; then
            seen=$((seen + 1))
        else
            seen=0
        fi
    done
    [ $seen -ge 2 ] || fail "the game did not start"
    sleep 10  # spawn
    echo "RSRIGHT:$SECS" > "$DIR/in.fifo"  # turn in place for the whole measurement
else
    sleep 5
fi

ticks() { awk '{print $14 + $15}' "/proc/$PID/stat"; }
hz=$(getconf CLK_TCK)
# AMD GPUs report how busy they are (all processes, so other games on the machine count).
gpu_file=$(ls /sys/class/drm/card*/device/gpu_busy_percent 2>/dev/null | head -1)
gpu_sum=0; gpu_n=0
wait_alone 300
snapshot "measurement start" > /dev/null
seen_others=""
cpu0=$(ticks); t0=$(date +%s.%N)
touch "$DIR/perf.reset"
for _ in $(seq 1 "$SECS"); do
    sleep 1
    if [ -n "$gpu_file" ]; then gpu_sum=$((gpu_sum + $(cat "$gpu_file"))); gpu_n=$((gpu_n + 1)); fi
    o=$(others); [ -n "$o" ] && seen_others="$o"
done
cpu1=$(ticks); t1=$(date +%s.%N)
snapshot "measurement end" > /dev/null
[ -n "$seen_others" ] && CONTENDED+=("other games during the measurement: $seen_others")
step 0 && cp "$DIR/last.png" "$DIR/measured.png"
stop

tag=""; [ ${#CONTENDED[@]} -gt 0 ] && tag=" CONTENDED"
grep -h "PERF: .*final=1" "$DIR/reach.log" | sed 's/.*PERF: /PERF: /; s/$/'"$tag"'/'
awk -v a="$cpu0" -v b="$cpu1" -v t0="$t0" -v t1="$t1" -v hz="$hz" \
    'BEGIN { printf "cpu: %.0f%% of one core over %.0f s\n", 100 * (b - a) / hz / (t1 - t0), t1 - t0 }'
[ "$gpu_n" -gt 0 ] && echo "gpu: $((gpu_sum / gpu_n))% busy (the whole GPU, other programs included)"
grep -h "Vulkan swapchain" "$DIR/reach.log" | tail -1 | sed 's/.*Vulkan swapchain/swapchain:/'
cat "$DIR/machine.txt"
if [ ${#CONTENDED[@]} -gt 0 ]; then
    printf 'CONTENDED (not a baseline): %s\n' "$(IFS=';'; echo "${CONTENDED[*]}")"
else
    echo "machine: clean"
fi
echo "log: $DIR/reach.log  frames: $DIR/frames.csv  screens: $DIR/step_*.png"
