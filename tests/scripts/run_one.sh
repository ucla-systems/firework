#!/usr/bin/env bash
# Firework experiment orchestrator.
#
# Launches one Firework application as N cooperating processes (ranks),
# waits for the benchmark rank (rank N-1) to finish, then tears down the
# idle ranks automatically. Replaces the manual tmux-window-per-rank flow.
#
# Usage:
#   scripts/run_one.sh <app-dir> <nodes> [options]
#
#   <app-dir>  benchmark directory under the tests repo (e.g. locktest, feed_gen)
#   <nodes>    number of Firework processes (FW_SERVER_SIZE)
#
# Options:
#   --no-setup        skip scripts/setup.sh (dax/debugfs reconfiguration)
#   --no-build        skip make clean && make all
#   --timeout SEC     kill everything after SEC seconds (default 7200)
#   --stagger0 SEC    delay after launching rank 0 (default 30; rank 0 sets up
#                     shared metadata and loads data, and starting rank 1 too
#                     early occasionally corrupts initialization)
#   --stagger SEC     delay between launching the remaining ranks (default 2)
#   --out DIR         results directory (default <tests>/results)
#   --env KEY=VAL     extra environment for the app (repeatable),
#                     e.g. --env CONSUMER_THREADS=1
#
# Exit code: the benchmark rank's exit code (124 on timeout).

set -u

TESTS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GLIBC_SO="${FW_GLIBC_SO:-$TESTS_ROOT/../glibc/build/libc.so.6}"

APP_DIR=""
NODES=""
DO_SETUP=1
DO_BUILD=1
TIMEOUT=7200
STAGGER0=30
STAGGER=2
OUT_ROOT="$TESTS_ROOT/results"
EXTRA_ENV=()
BIN_NAME=""

while [ $# -gt 0 ]; do
    case "$1" in
        --no-setup)  DO_SETUP=0 ;;
        --no-build)  DO_BUILD=0 ;;
        --timeout)   TIMEOUT="$2"; shift ;;
        --stagger0)  STAGGER0="$2"; shift ;;
        --stagger)   STAGGER="$2"; shift ;;
        --out)       OUT_ROOT="$2"; shift ;;
        --env)       EXTRA_ENV+=("$2"); shift ;;
        --bin)       BIN_NAME="$2"; shift ;;   # launch this binary instead of <dir-name>
        -*)          echo "unknown option: $1" >&2; exit 2 ;;
        *)  if   [ -z "$APP_DIR" ]; then APP_DIR="$1"
            elif [ -z "$NODES"   ]; then NODES="$1"
            else echo "unexpected argument: $1" >&2; exit 2; fi ;;
    esac
    shift
done

if [ -z "$APP_DIR" ] || [ -z "$NODES" ]; then
    sed -n '2,22p' "$0"; exit 2
fi

APP_PATH="$TESTS_ROOT/${APP_DIR%/}"
APP_NAME="$(basename "$APP_PATH")"
# The launched binary defaults to <dir-name> but can be overridden with
# --bin (e.g. sci_kernel ships a separate single-process binary sci_kernel-single).
[ -n "$BIN_NAME" ] || BIN_NAME="$APP_NAME"
BIN="$APP_PATH/$BIN_NAME"

[ -d "$APP_PATH" ]  || { echo "no such app dir: $APP_PATH" >&2; exit 2; }
[ -f "$GLIBC_SO" ]  || { echo "custom glibc not found: $GLIBC_SO (set FW_GLIBC_SO)" >&2; exit 2; }
[ "$NODES" -ge 1 ]  || { echo "nodes must be >= 1" >&2; exit 2; }

RUN_ID="$(date +%Y%m%d-%H%M%S)-${APP_NAME}-n${NODES}"
OUT_DIR="$OUT_ROOT/$RUN_ID"
mkdir -p "$OUT_DIR"

log() { echo "[run_one] $*" | tee -a "$OUT_DIR/orchestrator.log"; }

# --- 1. one-time setup + build (was: rank 0's job in run.sh) ---------------
if [ "$DO_SETUP" -eq 1 ]; then
    log "running scripts/setup.sh"
    "$TESTS_ROOT/scripts/setup.sh" >"$OUT_DIR/setup.log" 2>&1 \
        || { log "setup.sh FAILED (see $OUT_DIR/setup.log)"; exit 1; }
fi
if [ "$DO_BUILD" -eq 1 ]; then
    log "building $APP_NAME"
    make -C "$APP_PATH" clean all >"$OUT_DIR/build.log" 2>&1 \
        || { log "build FAILED (see $OUT_DIR/build.log)"; exit 1; }
fi
[ -x "$BIN" ] || { echo "binary not found after build: $BIN" >&2; exit 1; }

# --- 2. launch ranks -------------------------------------------------------
# Rank N-1 runs the benchmark; ranks 0..N-2 idle in sleep() and are killed
# here once the benchmark rank exits. Rank 0 starts first (it initializes
# the shared hint-zone metadata in firework_init).
SUDO_PIDS=()

cleanup() {
    for pid in ${SUDO_PIDS[@]+"${SUDO_PIDS[@]}"}; do
        if kill -0 "$pid" 2>/dev/null; then
            sudo kill -TERM "$pid" 2>/dev/null
        fi
    done
    # last resort for anything reparented or stuck
    sleep 1
    sudo pkill -KILL -x "$BIN_NAME" 2>/dev/null
    # Wait until the killed ranks have fully exited. The next run
    # re-initialises the shared-memory layout (the pool base moves with the
    # region size), and a rank still tearing down its address space at that
    # point frees pool pages of the old layout (M12 in the kernel notes).
    for _ in $(seq 1 100); do
        pgrep -x "$BIN_NAME" >/dev/null 2>&1 || break
        sleep 0.1
    done
    return 0
}
trap cleanup EXIT INT TERM

# launches in the current shell (no subshell) so $! stays wait-able
LAUNCH_PID=
launch_rank() {
    local idx="$1" logfile="$2"
    sudo env FW_SERVER_IDX="$idx" FW_SERVER_SIZE="$NODES" \
             LD_PRELOAD="$GLIBC_SO" ${EXTRA_ENV[@]+"${EXTRA_ENV[@]}"} \
             "$BIN" >"$logfile" 2>&1 &
    LAUNCH_PID=$!
}

cd "$APP_PATH"

# refresh sudo credentials up front so background launches don't prompt
sudo -v || exit 1

BENCH_RANK=$((NODES - 1))
for idx in $(seq 0 $((NODES - 2))); do
    launch_rank "$idx" "$OUT_DIR/rank$idx.log"
    SUDO_PIDS+=("$LAUNCH_PID")
    disown "$LAUNCH_PID"   # idle ranks are killed later; suppress job-control noise
    log "rank $idx launched (pid $LAUNCH_PID)"
    # Give rank 0 time to finish initialization before rank 1 starts; the
    # remaining ranks only need a short gap.
    if [ "$idx" -eq 0 ]; then sleep "$STAGGER0"; else sleep "$STAGGER"; fi
done

launch_rank "$BENCH_RANK" "$OUT_DIR/rank$BENCH_RANK.log"
BENCH_PID=$LAUNCH_PID
log "rank $BENCH_RANK (benchmark) launched (pid $BENCH_PID), waiting (timeout ${TIMEOUT}s)"

# --- 3. wait for the benchmark rank with timeout ---------------------------
# Liveness is checked through /proc rather than `kill -0`: the rank runs
# under sudo (root-owned), so `kill -0` from the unprivileged orchestrator
# fails with EPERM and would end this loop immediately, leaving the
# blocking `wait` below with no timeout at all (this is how a wedged run
# once sat for 10 hours). On timeout use SIGKILL: a rank stuck in a kernel
# lock wait ignores SIGTERM but the fault-side waits are killable.
SECONDS=0
RC=0
while [ -d "/proc/$BENCH_PID" ]; do
    if [ "$SECONDS" -ge "$TIMEOUT" ]; then
        log "TIMEOUT after ${TIMEOUT}s; killing all ranks (SIGKILL)"
        sudo pkill -KILL -x "$BIN_NAME" 2>/dev/null
        sudo kill -KILL "$BENCH_PID" 2>/dev/null
        RC=124
        break
    fi
    sleep 1
done
if [ "$RC" -eq 0 ]; then
    wait "$BENCH_PID"; RC=$?
else
    wait "$BENCH_PID" 2>/dev/null
fi

# --- 4. teardown + result summary ------------------------------------------
cleanup
trap - EXIT INT TERM

log "benchmark rank exited with code $RC"
log "logs: $OUT_DIR"
# the reporting thread may have been spawned on any rank, so scan all logs
echo "----- benchmark results (all ranks) -----"
grep -hE "Elapsed|Throughput|Latency|elapsed time|Page Faults|Migrations|Unshares" \
    "$OUT_DIR"/rank*.log || tail -20 "$OUT_DIR/rank$BENCH_RANK.log"

# machine-readable pointer to the latest run
ln -sfn "$OUT_DIR" "$OUT_ROOT/latest"
exit "$RC"
