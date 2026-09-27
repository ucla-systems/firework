#!/usr/bin/env bash
# Figure 8 drilldown driver (fig8.sh).
#
#   size     8a  scaling factor at 32 cores vs coherent-region size (256/512/768 MB), CF FG SK
#   unshare  8b  CF scaling with and without page unsharing
#   batch    8c  page sharing / unsharing throughput vs batch size (1..128), batching microbenchmark
#   policy   8d  FG scaling with Firework's eviction policy vs a FIFO baseline
#
# Usage:
#   scripts/fig8.sh [size|unshare|batch|policy|all] [--reps N] [--no-plot] [--out DIR]
#
# Results (median of --reps runs, default 3) go to results/fig8/<panel>/*.csv:
#   size/<app>.csv          3 rows: seconds at N=8 procs (32 cores) for 256, 512, 768 MB
#   unshare/{with,without}.csv   8 rows: CF seconds for N=1..8 procs
#   batch/{sharing,unsharing}.csv 8 rows: MB/s for batch size 1,2,4,...,128
#   policy/{firework,fifo}.csv    8 rows: FG seconds for N=1..8 procs
# The scaling factors of 8a use results/fig6/<app>/proc.csv (row 0) as the
# single-process reference, so run scripts/eval.sh first (or at least its
# proc series for CF, FG, SK).
set -u
TESTS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_ROOT="$TESTS_ROOT/results/fig8"
K=/sys/kernel/debug/firework

PANELS="size unshare batch policy"
REPS=3
PLOT=1
while [ $# -gt 0 ]; do
    case "$1" in
        size|unshare|batch|policy) PANELS="$1" ;;
        all)       PANELS="size unshare batch policy" ;;
        --reps)    REPS="$2"; shift ;;
        --no-plot) PLOT=0 ;;
        --out)     OUT_ROOT="$2"; shift ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done

declare -A APP_DIR=( [cf]=collab_filter [fg]=feed_gen [sk]=sci_kernel )
# Per-app runtime tuning, identical to scripts/eval.sh.
declare -A APP_MIG_BATCH=( [cf]=8 [fg]=1 [sk]=8 )
declare -A APP_UNSHARE_TO_ACCESSOR=( [cf]=1 [fg]=0 [sk]=0 )
declare -A APP_EPOCH_UNSHARE=( [cf]=1 [fg]=0 [sk]=0 )

MB=$((1024 * 1024))
HWC_UNLIMITED=$((3 * 1024 * MB))

log() { echo "[fig8] $*"; }
knob() { echo "$2" | sudo tee "$K/$1" >/dev/null 2>&1 || true; }
apply_app_knobs() {   # <app>
    knob migration_batch_size "${APP_MIG_BATCH[$1]}"
    knob unshare_to_accessor "${APP_UNSHARE_TO_ACCESSOR[$1]}"
    knob epoch_unshare "${APP_EPOCH_UNSHARE[$1]}"
    knob eviction_fifo 0
}
build() {   # <dir>
    make -C "$TESTS_ROOT/$1" clean >/dev/null 2>&1
    make -C "$TESTS_ROOT/$1" >/dev/null 2>&1 || { log "BUILD FAILED: $1"; return 1; }
}
median() {   # numbers on stdin -> median (nan-aware)
    sort -g | awk '{v[NR]=$1} END{ if(NR==0){print "nan"; exit} if(NR%2) print v[(NR+1)/2]; else print (v[NR/2]+v[NR/2+1])/2 }'
}
# Elapsed seconds of one run (same patterns/priority as eval.sh), or "nan".
elapsed_of() {   # <run dir>
    awk '
        match($0,/Total elapsed time: *([0-9]+\.?[0-9]*)/,a){ if(a[1]+0>t1)t1=a[1]+0 }
        match($0,/Bench completed in *([0-9]+\.?[0-9]*)/,a) { if(a[1]+0>t2)t2=a[1]+0 }
        match($0,/Elapsed time: *([0-9]+\.?[0-9]*)/,a)      { if(a[1]+0>t3)t3=a[1]+0 }
        match($0,/Elapsed: *([0-9]+\.?[0-9]*)/,a)           { if(a[1]+0>t4)t4=a[1]+0 }
        END{ if(t1>0)print t1; else if(t2>0)print t2; else if(t3>0)print t3; else if(t4>0)print t4; else print "nan" }
    ' "$1"/rank*.log
}
# Throughput reported by the batching microbenchmark ("<Label>: ... MB/s").
mbps_of() {   # <Sharing|Unsharing> <run dir>
    grep -hoE "^$1: .* [0-9.]+ MB/s" "$2"/rank*.log | grep -oE '[0-9.]+ MB/s' | cut -d' ' -f1 | head -1
}
# Run <dir> with <nprocs> REPS times, echo the median elapsed seconds.
run_point() {   # <dir> <nprocs> [extra run_one args...]
    local dir="$1" n="$2"; shift 2
    for r in $(seq 1 "$REPS"); do
        "$TESTS_ROOT/scripts/run_one.sh" "$dir" "$n" --no-setup --no-build --timeout 1200 "$@" >/dev/null 2>&1
        elapsed_of "$TESTS_ROOT/results/latest"
    done | median
}

mkdir -p "$OUT_ROOT"
"$TESTS_ROOT/scripts/setup.sh" >/dev/null 2>&1 || { log "setup.sh failed"; exit 1; }

for panel in $PANELS; do
    out="$OUT_ROOT/$panel"; mkdir -p "$out"
    log "=== panel $panel ==="
    case "$panel" in
    size)
        for app in cf fg sk; do
            dir="${APP_DIR[$app]}"
            build "$dir" || continue
            apply_app_knobs "$app"
            : > "$out/$app.csv"
            for mb in 256 512 768; do
                knob hwc_size $((mb * MB))
                v=$(run_point "$dir" 8)
                echo "$v" >> "$out/$app.csv"
                log "  $app hwc=${mb}MB N=8 -> ${v}s"
            done
        done
        ;;
    unshare)
        dir="${APP_DIR[cf]}"; build "$dir" || continue
        apply_app_knobs cf
        for series in with without; do
            if [ "$series" = with ]; then knob epoch_unshare 1; knob hwc_size $((512 * MB))
            else                          knob epoch_unshare 0; knob hwc_size "$HWC_UNLIMITED"; fi
            : > "$out/$series.csv"
            for n in 1 2 3 4 5 6 7 8; do
                v=$(run_point "$dir" "$n")
                echo "$v" >> "$out/$series.csv"
                log "  cf $series unsharing N=$n -> ${v}s"
            done
        done
        ;;
    batch)
        build batching || continue
        knob unshare_to_accessor 0; knob epoch_unshare 0; knob eviction_fifo 0
        knob hwc_size "$HWC_UNLIMITED"
        : > "$out/sharing.csv"; : > "$out/unsharing.csv"
        for b in 1 2 4 8 16 32 64 128; do
            knob migration_batch_size "$b"; knob tmp_unshare_batch_size "$b"
            sh_v=""; un_v=""
            for r in $(seq 1 "$REPS"); do
                "$TESTS_ROOT/scripts/run_one.sh" batching 2 --no-setup --no-build --timeout 600 \
                    --env FW_BATCH_UNSHARE=1 >/dev/null 2>&1
                logs="$TESTS_ROOT/results/latest"
                sh_v+="$(mbps_of Sharing "$logs") "
                un_v+="$(mbps_of Unsharing "$logs") "
            done
            s=$(printf '%s\n' $sh_v | median); u=$(printf '%s\n' $un_v | median)
            echo "$s" >> "$out/sharing.csv"; echo "$u" >> "$out/unsharing.csv"
            log "  batch=$b -> sharing ${s} MB/s, unsharing ${u} MB/s"
        done
        knob migration_batch_size 8; knob tmp_unshare_batch_size 128
        ;;
    policy)
        dir="${APP_DIR[fg]}"; build "$dir" || continue
        apply_app_knobs fg
        knob hwc_size $((512 * MB))
        for series in firework fifo; do
            if [ "$series" = fifo ]; then knob eviction_fifo 1; else knob eviction_fifo 0; fi
            : > "$out/$series.csv"
            for n in 1 2 3 4 5 6 7 8; do
                v=$(run_point "$dir" "$n")
                echo "$v" >> "$out/$series.csv"
                log "  fg $series policy N=$n -> ${v}s"
            done
        done
        knob eviction_fifo 0
        ;;
    esac
done

# restore the setup.sh defaults for whatever runs next
"$TESTS_ROOT/scripts/setup.sh" >/dev/null 2>&1
log "done. CSVs under $OUT_ROOT"

if [ "$PLOT" -eq 1 ]; then
    PY="${PYTHON:-python3}"
    if "$PY" -c "import matplotlib" 2>/dev/null; then
        "$PY" "$TESTS_ROOT/scripts/plot_fig8.py" "$OUT_ROOT"
    else
        log "matplotlib not found for $PY; plot later with: python3 scripts/plot_fig8.py"
    fi
fi
