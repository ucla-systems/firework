#!/usr/bin/env bash
# Figure 6 evaluation driver (eval.sh).
#
# For each application, builds and runs the four series of Figure 6 across
# the 8 x-axis points (4..32 cores) and writes one value per line to CSVs
# laid out like plot/data/main-new/<app>/<series>.csv:
#
#   firework_512m.csv       Firework, 512 MB coherent region  (N=1..8 procs)
#   firework_unlimited.csv  Firework, unlimited region        (N=1..8 procs)
#   shared.csv              shared-heap baseline              (N=1..8 procs)
#   proc.csv                single-process                    (1 proc, T=4..32 threads)
#
# Firework/shared scale by processes (4 threads each: 4,8,..,32 cores).
# single-process scales by threads in one process (4,8,..,32 threads),
# which requires a rebuild per thread count (the thread count is a compile
# -time constant), so this series is the slow one.
#
# Usage:
#   scripts/eval.sh [--apps "df cf pr fg sk"] [--series "firework_512m ..."]
#                   [--min-n A] [--max-n B] [--reps N] [--no-plot] [--dry-run]
#                   [--out DIR]
#
# Results go to results/fig6/<app>/<series>.csv: always 8 rows (cores
# 4..32), median of --reps runs per point (default 3; --reps 1 for a quick
# look). Points outside --min-n/--max-n keep their previous values (nan if
# never run), so a sweep can be completed in pieces. Every repetition's raw
# value is also appended to results/fig6/<app>/raw/reps.log. Re-running appends a fresh timestamped copy under
# results/fig6/<app>/raw/ so nothing is silently overwritten.

set -u
TESTS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_ROOT="$TESTS_ROOT/results/fig6"

# --- x-axis: 8 points, 4..32 cores -----------------------------------------
CORES=(4 8 12 16 20 24 28 32)     # = 4 * (N procs) = threads for single-proc
NPROCS=(1 2 3 4 5 6 7 8)          # firework/shared: procs (4 threads each)

# --- per-app config --------------------------------------------------------
# threads_macro: the -D name to override the per-process thread count.
declare -A APP_DIR=( [df]=dataframe [cf]=collab_filter [pr]=pagerank [fg]=feed_gen [sk]=sci_kernel )
declare -A APP_TMACRO=( [df]=THREADS_PER_PROC [cf]=THREADS_NUM [pr]=THREADS_PER_PROCESS [fg]=THREADS_PER_PROCESS [sk]=THREADS_PER_PROCESS )
# Single-process series specifics:
#  APP_SP_EXTRA - extra -D flags for the 1-proc build
#      cf: SINGLE_PROCESS also selects the per-thread input file.
#  APP_SP_BIN   - Makefile APP= target / binary name for the 1-proc build
#      sk: sci_kernel ships a dedicated single-process source (sci_kernel-single.cpp)
#          with a different work decomposition; the paper's SK single-process
#          baseline is that binary, not sci_kernel.cpp at N=1.
declare -A APP_SP_EXTRA=( [df]="" [cf]="-DSINGLE_PROCESS" [pr]="" [fg]="" [sk]="" )
declare -A APP_SP_BIN=(   [df]="" [cf]="" [pr]="" [fg]="" [sk]="sci_kernel-single" )

# Per-app runtime tuning applied to /sys/kernel/debug/firework before each
# app's runs (overrides the default set by setup.sh). migration_batch_size:
# FG uses 1 (its random KV access has poor spatial locality, so prefetching
# contiguous pages just wastes migrations); the others use 8.
declare -A APP_MIG_BATCH=( [df]=8 [cf]=8 [pr]=8 [fg]=1 [sk]=8 )
# unshare_to_accessor: move unshared pages to their single recent accessor
# (paper §4.4) instead of back to the allocating process. Enabled for CF,
# whose phases re-partition the shared factor matrices across nodes.
declare -A APP_UNSHARE_TO_ACCESSOR=( [df]=0 [cf]=1 [pr]=0 [fg]=0 [sk]=0 )
# epoch_unshare: periodic accessed-bit scan that unshares pages with a single
# recent accessor (paper §4.4) even when the region is not full. Enabled for
# CF, whose compute phase works on node-local slices of matrices shared in
# the previous phase. FW_EPOCH_UNSHARE=0|1 in the environment overrides the
# table for every app (experiments only).
declare -A APP_EPOCH_UNSHARE=( [df]=0 [cf]=1 [pr]=0 [fg]=0 [sk]=0 )

APPS="df cf pr fg sk"
SERIES="firework_512m firework_unlimited shared proc"
REPS=3
DRY=0
MINN=1
MAXN=8
PLOT=1

while [ $# -gt 0 ]; do
    case "$1" in
        --apps)    APPS="$2"; shift ;;
        --series)  SERIES="$2"; shift ;;
        --reps)    REPS="$2"; shift ;;
        --min-n)   MINN="$2"; shift ;;   # first point to run (1..8)
        --max-n)   MAXN="$2"; shift ;;   # last point to run (1..8)
        --no-plot) PLOT=0 ;;              # skip regenerating the figures at the end
        --out)     OUT_ROOT="$2"; shift ;; # write CSVs under this directory instead of results/fig6
        --dry-run) DRY=1 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done

# Restrict the sweep to points MINN..MAXN (e.g. --min-n 5 --max-n 8 -> 20..32 cores).
if [ "$MINN" -lt 1 ] || [ "$MAXN" -gt 8 ] || [ "$MINN" -gt "$MAXN" ]; then
    echo "bad --min-n/--max-n: $MINN..$MAXN (need 1 <= min <= max <= 8)" >&2; exit 2
fi
CORES=("${CORES[@]:$((MINN-1)):$((MAXN-MINN+1))}")
NPROCS=("${NPROCS[@]:$((MINN-1)):$((MAXN-MINN+1))}")

HWC_512M=536870912
# "Unlimited" coherent region = 3GB: no application ever shares more than
# that, and the kernel's per-pfn tables are sized for at most 4GB
# (MAX_LIMITED_MEMORY_SIZE); larger values are rejected at init.
HWC_UNLIMITED=$((3 * 1024 * 1024 * 1024))

log() { echo "[eval] $*"; }

# Build one app dir with the given FIG6_FLAGS. Optional 3rd arg overrides the
# Makefile APP= target (used by sci_kernel to build sci_kernel-single).
build() {
    local dir="$1" flags="$2" appvar="${3:-}"
    local margs=(FIG6_FLAGS="$flags")
    [ -n "$appvar" ] && margs+=(APP="$appvar")
    log "build $dir  FIG6_FLAGS='$flags' ${appvar:+APP=$appvar}"
    [ "$DRY" -eq 1 ] && return 0
    make -C "$TESTS_ROOT/$dir" clean >/dev/null 2>&1
    make -C "$TESTS_ROOT/$dir" "${margs[@]}" >/dev/null 2>&1 \
        || { log "BUILD FAILED: $dir ($flags $appvar)"; return 1; }
}

# Set the coherent-region size for a run.
set_hwc() {
    [ "$DRY" -eq 1 ] && return 0
    echo "$1" | sudo tee /sys/kernel/debug/firework/hwc_size >/dev/null
}

# Set the page-migration batch size (per-app tuning).
set_mig_batch() {
    [ "$DRY" -eq 1 ] && return 0
    echo "$1" | sudo tee /sys/kernel/debug/firework/migration_batch_size >/dev/null
}

# Select the unsharing destination policy (per-app tuning).
set_unshare_to_accessor() {
    [ "$DRY" -eq 1 ] && return 0
    echo "$1" | sudo tee /sys/kernel/debug/firework/unshare_to_accessor >/dev/null
}

# Enable/disable epoch-based automatic unsharing (per-app tuning).
set_epoch_unshare() {
    [ "$DRY" -eq 1 ] && return 0
    echo "$1" | sudo tee /sys/kernel/debug/firework/epoch_unshare >/dev/null 2>&1 || true
}

# Merge the values just measured (one per line in $1, for points starting at
# $3) into the 8-row CSV $2, keeping rows outside the measured range.
merge_csv() {
    local raw="$1" csv="$2" first="$3" i k
    [ "$DRY" -eq 1 ] && return 0
    local rows=(nan nan nan nan nan nan nan nan) vals=()
    [ -f "$csv" ] && mapfile -t rows < <(head -8 "$csv"; yes nan | head -8) && rows=("${rows[@]:0:8}")
    mapfile -t vals < "$raw"
    for k in "${!vals[@]}"; do
        i=$((first - 1 + k)); [ "$i" -lt 8 ] && rows[$i]="${vals[$k]}"
    done
    printf '%s\n' "${rows[@]}" > "$csv"
}

# Run one point and echo the elapsed seconds (mean of REPS), or "nan".
# Apps disagree on wording, and several print one "Elapsed time" line PER
# THREAD, so run_point tries patterns in priority order and, for the first
# that matches, takes the MAX (all threads barrier at start; the slowest to
# finish is the wall-clock time that determines throughput).
run_point() {
    local app="$1" nprocs="$2" binname="${3:-}"
    local vals=() r out logs binarg=()
    [ -n "$binname" ] && binarg=(--bin "$binname")
    for r in $(seq 1 "$REPS"); do
        if [ "$DRY" -eq 1 ]; then vals+=("0"); continue; fi
        "$TESTS_ROOT/scripts/run_one.sh" "${APP_DIR[$app]}" "$nprocs" \
            "${binarg[@]}" --no-setup --no-build --timeout 1200 >/dev/null 2>&1
        logs="$TESTS_ROOT/results/latest"
        # Priority-ordered patterns; for the first that hits, take the max
        # value (wall-clock = slowest thread for per-thread prints).
        # Capture the number AFTER each label (not stray digits like a
        # "Thread 5:" prefix). Keep the max seen for each pattern.
        out=$(awk '
            match($0,/Total elapsed time: *([0-9]+\.?[0-9]*)/,a){ if(a[1]+0>t1)t1=a[1]+0 }
            match($0,/Bench completed in *([0-9]+\.?[0-9]*)/,a) { if(a[1]+0>t2)t2=a[1]+0 }
            match($0,/Elapsed time: *([0-9]+\.?[0-9]*)/,a)      { if(a[1]+0>t3)t3=a[1]+0 }
            match($0,/Elapsed: *([0-9]+\.?[0-9]*)/,a)           { if(a[1]+0>t4)t4=a[1]+0 }
            END{ if(t1>0)print t1; else if(t2>0)print t2; else if(t3>0)print t3; else if(t4>0)print t4 }
        ' "$logs"/rank*.log 2>/dev/null)
        vals+=("${out:-nan}")
    done
    # keep every repetition's raw value for later inspection
    [ -n "${appout:-}" ] && echo "$app n=$nprocs${binname:+ bin=$binname}: ${vals[*]}" >> "$appout/raw/reps.log"
    # median (ignoring nan); print nan if none valid. Runs fluctuate, so the
    # final figures use --reps 3 (or more) and take the median per point.
    printf '%s\n' "${vals[@]}" | awk '
        /nan/ {next} {a[n++]=$1}
        END{ if(!n){printf "nan"; exit}
             asort(a)
             if(n%2) printf "%.5f", a[(n+1)/2]; else printf "%.5f", (a[n/2]+a[n/2+1])/2 }'
}

mkdir -p "$OUT_ROOT"
[ "$DRY" -eq 0 ] && { sudo -v || exit 1; "$TESTS_ROOT/scripts/setup.sh" >/dev/null 2>&1; }

for app in $APPS; do
    dir="${APP_DIR[$app]}"; tmacro="${APP_TMACRO[$app]}"
    [ -n "$dir" ] || { log "unknown app '$app', skipping"; continue; }
    appout="$OUT_ROOT/$app"; mkdir -p "$appout/raw"
    ts=$(date +%Y%m%d-%H%M%S)

    # Apply this app's runtime tuning once, up front (overrides setup.sh).
    set_mig_batch "${APP_MIG_BATCH[$app]:-8}"
    set_unshare_to_accessor "${APP_UNSHARE_TO_ACCESSOR[$app]:-0}"
    epoch="${FW_EPOCH_UNSHARE:-${APP_EPOCH_UNSHARE[$app]:-0}}"
    set_epoch_unshare "$epoch"
    log "app=$app: migration_batch_size=${APP_MIG_BATCH[$app]:-8} unshare_to_accessor=${APP_UNSHARE_TO_ACCESSOR[$app]:-0} epoch_unshare=$epoch"

    for series in $SERIES; do
        csv="$appout/$series.csv"; raw="$appout/raw/${series}-${ts}.csv"
        : > "$raw"
        log "=== app=$app series=$series ==="

        case "$series" in
        firework_512m|firework_unlimited)
            # one build (default 4 threads/proc), sweep processes 1..8
            build "$dir" "" || { echo "nan" > "$csv"; continue; }
            [ "$series" = firework_512m ] && set_hwc "$HWC_512M" || set_hwc "$HWC_UNLIMITED"
            for n in "${NPROCS[@]}"; do
                v=$(run_point "$app" "$n")
                echo "$v" | tee -a "$raw"
                log "  N=$n cores=$((4*n)) -> ${v}s"
            done
            ;;
        shared)
            # shared-heap: rebuild with SHARE_EVERYTHING, sweep processes 1..8
            build "$dir" "-DSHARE_EVERYTHING" || { echo "nan" > "$csv"; continue; }
            set_hwc "$HWC_UNLIMITED"   # shared heap needs room for the whole heap
            for n in "${NPROCS[@]}"; do
                v=$(run_point "$app" "$n")
                echo "$v" | tee -a "$raw"
                log "  N=$n cores=$((4*n)) -> ${v}s"
            done
            ;;
        proc)
            # single-process: 1 proc, rebuild per thread count T=4..32.
            # Some apps ship a dedicated single-process binary (sk =
            # sci_kernel-single); build that target and launch it via --bin.
            set_hwc "$HWC_UNLIMITED"
            spbin="${APP_SP_BIN[$app]}"
            for t in "${CORES[@]}"; do
                build "$dir" "-D${tmacro}=${t} ${APP_SP_EXTRA[$app]}" "$spbin" \
                    || { echo "nan" | tee -a "$raw"; continue; }
                v=$(run_point "$app" 1 "$spbin")
                echo "$v" | tee -a "$raw"
                log "  T=$t threads -> ${v}s"
            done
            ;;
        *) log "unknown series '$series'"; continue ;;
        esac

        merge_csv "$raw" "$csv" "$MINN"
        log "wrote $csv (rows $MINN..$MAXN updated)"
    done
done

log "done. CSVs under $OUT_ROOT"

# Regenerate the figures from the CSVs just written. PYTHON can point at an
# interpreter that has matplotlib (e.g. a venv); falls back to a hint if none.
if [ "$PLOT" -eq 1 ] && [ "$DRY" -eq 0 ]; then
    PY="${PYTHON:-python3}"
    if "$PY" -c "import matplotlib" 2>/dev/null; then
        if [ "$OUT_ROOT" = "$TESTS_ROOT/results/fig6" ]; then
            "$PY" "$TESTS_ROOT/scripts/plot_fig6.py"
            log "figures: $TESTS_ROOT/results/figures/fig6_<app>.{pdf,png} and fig6.{pdf,png}"
        else
            "$PY" "$TESTS_ROOT/scripts/plot_fig6.py" "$OUT_ROOT" -o "$OUT_ROOT/figures"
            log "figures: $OUT_ROOT/figures/fig6_<app>.{pdf,png} and fig6.{pdf,png}"
        fi
    else
        log "matplotlib not found for $PY; plot later with: PYTHON=<python-with-matplotlib> python3 scripts/plot_fig6.py"
    fi
else
    log "plot with: python3 scripts/plot_fig6.py   (-> results/figures/)"
fi
