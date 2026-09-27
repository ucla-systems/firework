# Firework — Artifact

Artifact repository for *Firework: Efficient and Transparent DSM with
Hardware-Coherent CXL Shared Memory* (ASPLOS 2027). It contains the Firework
user-level runtime, the five applications evaluated in the paper, and the
scripts that run the experiments and regenerate the main result (Figure 6)
and the performance drilldown (Figure 8).

```
runtime/         Firework runtime (libfirework.so)
dataframe/       DataFrame (DF)          collab_filter/   Collaborative Filtering (CF)
pagerank/        PageRank (PR)           feed_gen/        Feed Generation (FG)
sci_kernel/      Scientific Kernel (SK)
locktest/ unshare/   microbenchmarks used by the basic test
batching/            page sharing/unsharing throughput microbenchmark (Figure 8c)
scripts/         setup, orchestration, evaluation, plotting
```

## After every reboot

```bash
scripts/setup.sh
```

Initializes the dax namespaces and the Firework debugfs settings (they do not
survive a reboot). The scripts below run it automatically unless `--no-setup`
is given.

## Basic test (~5 min)

```bash
scripts/kick_the_tires.sh
```

Checks the environment (Firework kernel, debugfs, dax, glibc, runtime) and
runs two 2-process benchmarks: `locktest` (remote threads, CXL-resident
lock) and `unshare` (on-demand page sharing; non-zero page-fault and
migration counters, data verified). Ends with `ALL CHECKS PASSED`.

## Evaluation (Figure 6)

```bash
scripts/eval.sh                 # all apps, 4..32 cores, 3 runs per point (median)
scripts/eval.sh --max-n 4       # 4..16 cores only (quicker first look)
scripts/eval.sh --apps "cf pr"  # a subset of applications
```

For each application it runs the four series of Figure 6 — Firework with a
512 MB coherent region, Firework with an unlimited region, the shared-heap
baseline, and the single-process baseline — and writes:

- `results/fig6/<app>/<series>.csv` — elapsed time per point
- `results/figures/fig6_<app>.pdf` (`.png`) — one figure per application
- `results/figures/fig6.pdf` (`.png`) — the combined figure

Figures are regenerated at the end of the run (`--no-plot` to skip;
`python3 scripts/plot_fig6.py` regenerates them by hand). A single
configuration can be run with `scripts/run_one.sh <app> <processes>`.
The full sweep takes about 14 hours.

Datasets live under each application's `data/` directory on the evaluation
machine (`dataframe/data`, `collab_filter/data`, `pagerank/data`, `feed_gen/data`).

## Drilldown (Figure 8)

```bash
scripts/fig8.sh                 # all four panels, 3 runs per point (median)
scripts/fig8.sh size --reps 1   # one panel: size | unshare | batch | policy
```

- `size` (8a): scaling factor at 32 cores for a 256 / 512 / 768 MB coherent
  region (CF, FG, SK; uses the single-process reference from `results/fig6`)
- `unshare` (8b): CF with and without page unsharing
- `batch` (8c): page sharing / unsharing throughput vs batch size, measured by
  the `batching` microbenchmark
- `policy` (8d): FG under Firework's eviction policy vs a FIFO baseline

Results go to `results/fig8/<panel>/*.csv` and the figures to
`results/figures/fig8_<panel>.pdf` (`.png`) and `fig8.pdf`
(`python3 scripts/plot_fig8.py` regenerates them). Run `scripts/eval.sh`
(at least its single-process series) before the `size` panel.
With the default 3 runs per point the four panels take about 4 hours
(roughly 1 h 15 min per run: size 16 min, unshare 30 min, policy 25 min,
batch 5 min).
