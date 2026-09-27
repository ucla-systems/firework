#!/usr/bin/env python3
"""Regenerate the Figure 6 panels from the sweep CSVs.

Reads results/fig6/<app>/<series>.csv (one elapsed-seconds value per line,
8 rows = cores 4..32) and plots normalized throughput vs cores, one panel
per application, matching the paper's Figure 6.

Normalization follows the original per-application plotting scripts
(tests/<app>/fig/figure.py), with t_ref = proc.csv row 0 (single process,
4 cores) so that point is 1.0 for every app:

  cf, pr, fg, sk :  y = t_ref / t
      Fixed total work split across threads; the app prints the wall-clock
      time of one run, so throughput is just its inverse.
      (the original Scientific Kernel (sci_kernel) script had this step commented out only because its
      CSV had been filled with already-normalized values by hand; the sweep
      records seconds, so the same formula applies.)

  df             :  y = nodes^2 * t_ref / t      (nodes = point index + 1)
      DataFrame (dataframe) scales its input with the thread count (weak scaling) and reports
      the SUM of the per-thread times, not wall-clock. Work grows ~nodes and
      wall-clock ~ sum/nodes, so work/wall ~ nodes^2/sum. This reproduces the
      paper's ~5.9x DF gain from 4 to 32 cores.

Outputs (under results/figures/ by default, or the -o directory):
  fig6_<app>.{pdf,png}   one standalone figure per application
  fig6.{pdf,png}         the combined 5-panel figure as in the paper

Usage: python3 scripts/plot_fig6.py [results_dir] [-o out_dir]
"""
import os
import sys
import csv

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# Paper style (tests/plot/draw.ipynb, main_perf cell)
FONT_SIZE = 24
BASELINE_COLOR = "#29503B"
plt.rc("font", size=FONT_SIZE, family="sans-serif")
plt.rcParams["font.sans-serif"] = ["Arial", "Liberation Sans", "DejaVu Sans"]
plt.rc("pdf", fonttype=42)
plt.rcParams["axes.facecolor"] = "white"
plt.rcParams["axes.edgecolor"] = "black"

CORES = [4, 8, 12, 16, 20, 24, 28, 32]
APPS = [("df", "DataFrame"), ("cf", "Collaborative Filtering"),
        ("pr", "PageRank"), ("fg", "Feed Generation"),
        ("sk", "Scientific Kernel")]
# Per-app normalization (see module docstring). nodes = point index + 1.
def _norm_strong(i, t, ref):
    return ref / t

def _norm_weak_sum(i, t, ref):
    return (i + 1) ** 2 * ref / t

NORMALIZE = {"df": _norm_weak_sum}     # everything else: _norm_strong

# (csv, label, color, marker) in the paper's legend order
SERIES = [
    ("shared",             "Shared Heap",          "C2", "o"),
    ("firework_512m",      "Firework (512MB)",     "C0", "^"),
    ("firework_unlimited", "Firework (Unlimited)", "C1", "s"),
    ("proc",               "Single Process",       "C3", "*"),
]


def load(path):
    if not os.path.exists(path):
        return None
    vals = []
    with open(path) as f:
        for row in f:
            row = row.strip()
            if not row:
                continue
            try:
                vals.append(float(row))
            except ValueError:
                vals.append(float("nan"))
    return vals or None


def plot_app(ax, appdir, title, ylabel=True, key=None):
    """Draw one application's Figure 6 panel onto `ax`.

    Returns True if any series was plotted (so callers can skip apps with
    no data yet).
    """
    proc = load(os.path.join(appdir, "proc.csv"))
    ref = proc[0] if proc and proc[0] == proc[0] else None  # nan-safe
    plotted = False
    for sname, label, color, marker in SERIES:
        vals = load(os.path.join(appdir, sname + ".csv"))
        if not vals:
            continue
        n = min(len(vals), len(CORES))
        xs, ys = [], []
        for i in range(n):
            t = vals[i]
            if t != t or t <= 0:      # nan/invalid
                continue
            # normalized throughput vs single-process @ 4 cores
            norm = NORMALIZE.get(key, _norm_strong)
            y = norm(i, t, ref if ref else vals[0])
            xs.append(CORES[i])
            ys.append(y)
        if xs:
            ax.plot(xs, ys, label=label, color=color, marker=marker,
                    linewidth=3, linestyle="-", markersize=10)
            plotted = True
    # the single-process @ 4 cores reference (y = 1)
    ax.plot(CORES, [1.0] * len(CORES), linestyle="--", linewidth=3, color=BASELINE_COLOR)
    if title:
        ax.set_title(title, fontsize=FONT_SIZE, pad=12)
    ax.set_xlabel("Number of Cores")
    ax.set_xticks(CORES)
    if ylabel:
        ax.set_ylabel("Normalized Throughput")
    return plotted


def top_legend(fig, ax, ncol, y):
    """One legend above the figure, as in the paper."""
    handles, labels = ax.get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, y),
               ncol=ncol, frameon=False, prop={"size": FONT_SIZE})


def save(fig, path):
    base = os.path.splitext(path)[0]
    fig.savefig(base + ".pdf", format="pdf", bbox_inches="tight")
    fig.savefig(base + ".png", dpi=120, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {base}.pdf / .png")


def main():
    # Resolve paths relative to the repo (this script's parent's parent), so
    # the plot works regardless of the current working directory.
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    results = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") \
        else os.path.join(repo, "results", "fig6")
    # Default output directory: results/figures/
    outdir = os.path.join(repo, "results", "figures")
    if "-o" in sys.argv:
        outdir = sys.argv[sys.argv.index("-o") + 1]
    os.makedirs(outdir, exist_ok=True)

    # 1. One standalone figure per application: fig6_<app>.{pdf,png}
    #    (each panel is its own file so they can be placed independently).
    for key, title in APPS:
        appdir = os.path.join(results, key)
        fig, ax = plt.subplots(figsize=(8, 4.5))
        if plot_app(ax, appdir, None, key=key):
            top_legend(fig, ax, ncol=2, y=1.25)
            fig.tight_layout()
            save(fig, os.path.join(outdir, f"fig6_{key}"))
        else:
            plt.close(fig)
            print(f"skip {key}: no data under {appdir}")

    # 2. The combined 5-panel figure as in the paper: fig6.{pdf,png}
    fig, axes = plt.subplots(1, len(APPS), figsize=(24, 4.8))
    for i, (ax, (key, title)) in enumerate(zip(axes, APPS)):
        plot_app(ax, os.path.join(results, key), f"({'abcde'[i]}) {title}",
                 ylabel=(i == 0), key=key)
    top_legend(fig, axes[0], ncol=4, y=1.12)
    fig.align_labels()
    fig.tight_layout()
    save(fig, os.path.join(outdir, "fig6"))


if __name__ == "__main__":
    main()
