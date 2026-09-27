#!/usr/bin/env python3
"""Regenerate the Figure 8 panels from the fig8.sh CSVs, in the paper's style.

  8a  fig8_size:    scaling factor at 32 cores per app (CF, FG, SK) for a
                    256 / 512 / 768 MB coherent region
                    (t_ref = results/fig6/<app>/proc.csv row 0, i.e. single
                    process at 4 cores; factor = t_ref / t)
  8b  fig8_unshare: CF scaling with / without unsharing (t[N=1] / t[N])
  8c  fig8_batch:   sharing / unsharing throughput (MB/s) vs batch size
  8d  fig8_policy:  FG scaling, Firework policy vs FIFO (t[N=1] / t[N])

Usage: python3 scripts/plot_fig8.py [results/fig8] [-o out_dir]
Outputs fig8_<panel>.{pdf,png} (one panel each, as in the paper) and the
combined fig8.{pdf,png} under results/figures/ (or -o).
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CORES = [4, 8, 12, 16, 20, 24, 28, 32]
SIZES = [256, 512, 768]
BATCH = [1, 2, 4, 8, 16, 32, 64, 128]
APPS = [("cf", "CF"), ("fg", "FG"), ("sk", "SK")]
# one bar group per app, one bar per region size: (color, hatch) as in the paper
SIZE_STYLE = [("C0", None), ("C1", "//"), ("C2", "..")]

FONT_SIZE = 24
FIG_SIZE = (8, 4.5)
BASELINE_COLOR = "#29503B"

plt.rc("font", size=FONT_SIZE, family="sans-serif")
plt.rcParams["font.sans-serif"] = ["Arial", "Liberation Sans", "DejaVu Sans"]
plt.rc("pdf", fonttype=42)
plt.rcParams["axes.facecolor"] = "white"
plt.rcParams["axes.edgecolor"] = "black"
plt.rcParams["grid.color"] = "lightgrey"


def load(path):
    if not os.path.exists(path):
        return None
    vals = []
    for row in open(path):
        row = row.strip()
        if row:
            try:
                vals.append(float(row))
            except ValueError:
                vals.append(float("nan"))
    return vals or None


def legend(ax, **kw):
    args = dict(frameon=True, loc="upper left", ncol=1, prop={"size": FONT_SIZE})
    args.update(kw)
    ax.legend(**args)


def panel_size(ax, res, fig6):
    """Bars: scaling factor at 32 cores per app, one bar per region size."""
    n = len(SIZES)
    width = 0.75
    factors = {}
    for key, _ in APPS:
        t = load(os.path.join(res, "size", key + ".csv"))
        proc = load(os.path.join(fig6, key, "proc.csv"))
        if t and proc:
            factors[key] = [proc[0] / v if v and v == v else float("nan") for v in t]
    if not factors:
        return False
    for j, (mb, (color, hatch)) in enumerate(zip(SIZES, SIZE_STYLE)):
        xs = [x + (j - (n - 1) / 2) * width / n for x in range(len(APPS))]
        ys = [factors[key][j] if key in factors and j < len(factors[key]) else float("nan")
              for key, _ in APPS]
        ax.bar(xs, ys, width / n, label=f"{mb}MB", color=color, hatch=hatch,
               edgecolor="black", linewidth=0.5)
    ax.set_xticks(range(len(APPS)))
    ax.set_xticklabels([label for _, label in APPS])
    ax.set_ylabel("Scaling Factor")
    ax.set_ylim(0, 7)
    ax.grid(axis="y")
    legend(ax)
    return True


def panel_lines(ax, res, sub, series):
    """Scaling-factor lines (t[N=1] / t[N]) with the y=1 baseline."""
    plotted = False
    for fname, label, color, marker in series:
        t = load(os.path.join(res, sub, fname + ".csv"))
        if not t:
            continue
        base = t[0]
        xs = [CORES[i] for i, v in enumerate(t[:8]) if v == v and v > 0]
        ys = [base / v for v in t[:8] if v == v and v > 0]
        ax.plot(xs, ys, label=label, color=color, marker=marker,
                linewidth=4, linestyle="-", markersize=13)
        plotted = True
    ax.plot(CORES, [1.0] * len(CORES), linestyle="--", linewidth=4, color=BASELINE_COLOR)
    ax.set_xticks(CORES)
    ax.set_xlabel("Number of Cores")
    ax.set_ylabel("Scaling Factor")
    ax.grid(axis="y")
    if plotted:
        legend(ax)
    return plotted


def panel_batch(ax, res):
    plotted = False
    for fname, label, color, marker in [("sharing", "Sharing", "C0", "s"),
                                        ("unsharing", "Unsharing", "C1", "o")]:
        t = load(os.path.join(res, "batch", fname + ".csv"))
        if not t:
            continue
        xs = [BATCH[i] for i, v in enumerate(t[:8]) if v == v]
        ys = [v for v in t[:8] if v == v]
        ax.plot(xs, ys, label=label, color=color, marker=marker,
                linewidth=4, linestyle="-", markersize=10)
        plotted = True
    ax.set_xscale("log")
    ax.set_xticks(BATCH)
    ax.set_xticklabels([str(b) for b in BATCH])
    ax.minorticks_off()
    ax.set_xlabel("Batch Size")
    ax.set_ylabel("Throughput (MB/s)")
    ax.grid(axis="y")
    if plotted:
        legend(ax)
    return plotted


def save(fig, base):
    fig.savefig(base + ".pdf", format="pdf", bbox_inches="tight")
    fig.savefig(base + ".png", dpi=120, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {base}.pdf / .png")


def main():
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    args = sys.argv[1:]
    res = args[0] if args and not args[0].startswith("-") else os.path.join(repo, "results", "fig8")
    fig6 = os.path.join(repo, "results", "fig6")
    outdir = args[args.index("-o") + 1] if "-o" in args else os.path.join(repo, "results", "figures")
    os.makedirs(outdir, exist_ok=True)

    panels = [
        ("size", "(a) Coherent region size", lambda ax: panel_size(ax, res, fig6)),
        ("unshare", "(b) Page unsharing (CF)", lambda ax: panel_lines(ax, res, "unshare",
            [("with", "with unsharing", "C0", "^"), ("without", "w/o unsharing", "C5", "h")])),
        ("batch", "(c) Batch size", lambda ax: panel_batch(ax, res)),
        ("policy", "(d) Eviction policy (FG)", lambda ax: panel_lines(ax, res, "policy",
            [("firework", "Firework", "C0", "^"), ("fifo", "FIFO", "C5", "h")])),
    ]
    # one figure per panel, laid out exactly as the paper's PDFs
    for name, _, draw in panels:
        fig, ax = plt.subplots(figsize=FIG_SIZE)
        if draw(ax):
            fig.align_labels()
            fig.tight_layout()
            save(fig, os.path.join(outdir, f"fig8_{name}"))
        else:
            plt.close(fig)
            print(f"skip {name}: no data under {res}/{name}")
    # all four side by side, with the paper's sub-captions
    fig, axes = plt.subplots(1, len(panels), figsize=(FIG_SIZE[0] * len(panels), FIG_SIZE[1] + 0.8))
    for ax, (_, caption, draw) in zip(axes, panels):
        draw(ax)
        ax.set_title(caption, fontsize=FONT_SIZE, pad=12)
    fig.align_labels()
    fig.tight_layout()
    save(fig, os.path.join(outdir, "fig8"))


if __name__ == "__main__":
    main()
