#!/usr/bin/env python3
"""waterfall.py — continuous GPU timing waterfall from a benchmark CSV.

Reads the sweep-results.csv / bench-results.csv schema (tools/bench.py writes
it). Default: one section per calendar date (daily average), exactly as
before. Additions for bench.py's multi-window/multi-arm CSVs:
  --window NAME   keep only rows of one window (bench, steady, end, ...)
  --label NAME    keep only rows of one arm
  --group-by      date (default) | label | window — one section per value
Rows of different windows are never averaged together silently: grouping by
date/label with several windows present requires --window.

Run with the tools venv:  tools/.venv/bin/python tools/waterfall.py CSV
"""

import argparse
import re
import datetime as dt
from pathlib import Path

import pandas as pd
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle


# ============================================================
# Configuration
# ============================================================

# Local time zone of this machine: every date/time shown is local.
LOCAL_TZ = dt.datetime.now().astimezone().tzinfo

# What the bench.py preset windows are FOR (the numbers are read from the
# CSV's start/limit/segment columns, not from here).
WINDOW_PURPOSE = {
    "bench":  "standard benchmark: short window at the start of [4e18, 5e18]; "
              "startup costs weigh more",
    "steady": "steady state at the start of [4e18, 5e18]: long enough that "
              "startup is negligible - closest to a production run",
    "end":    "steady state at the END of the range (just below 5e18): the "
              "hardest part, ~11% more base primes",
    "long":   "precision window: 1e11 at the start of [4e18, 5e18] - 5x steady, "
              "used to resolve sub-1% differences",
    "long-end": "precision window: 1e11 just below 5e18",
}

PHASES = [
    "sieve_s",
    "large_s",
    "check_s",
    "offsets_s",
    "sync_s",
]


# ============================================================
# Load CSV
# ============================================================

def load_csv(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)

    required = [
        "timestamp",
        "segment",
        "split",
        "run",
        "status",
        "gpu_s",
        "sieve_s",
        "large_s",
        "check_s",
        "offsets_s",
        "sync_s",
        "evens_per_sec",
        "wall_s",
    ]

    missing = [col for col in required if col not in df.columns]

    if missing:
        raise ValueError(
            "CSV is missing required columns:\n"
            + "\n".join(f"  - {x}" for x in missing)
        )

    # Parse timestamp.
    df["timestamp"] = pd.to_datetime(
        df["timestamp"],
        errors="coerce",
        utc=True,
    )

    if df["timestamp"].isna().any():
        bad = df["timestamp"].isna().sum()
        raise ValueError(f"{bad} timestamp values could not be parsed.")

    # Convert numeric columns.
    numeric_columns = [
        "segment",
        "split",
        "run",
        "gpu_s",
        "sieve_s",
        "large_s",
        "check_s",
        "offsets_s",
        "sync_s",
        "evens_per_sec",
        "wall_s",
    ]

    for column in numeric_columns:
        df[column] = pd.to_numeric(
            df[column],
            errors="coerce",
        )

    # Remove incomplete benchmark rows.
    df = df.dropna(
        subset=[
            "timestamp",
            "gpu_s",
            "sieve_s",
            "large_s",
            "check_s",
            "offsets_s",
            "sync_s",
            "evens_per_sec",
            "wall_s",
        ]
    )

    # Keep PASS results if a status column exists.
    if "status" in df.columns:
        df = df[
            df["status"]
            .astype(str)
            .str.lower()
            .eq("pass")
        ]

    if df.empty:
        raise ValueError("No valid PASS benchmark rows found.")

    # Calendar date used for daily averaging (local time).
    df["timestamp"] = df["timestamp"].dt.tz_convert(LOCAL_TZ)
    df["date"] = df["timestamp"].dt.date.astype(str)

    return df


# ============================================================
# Calculate daily averages
# ============================================================

def calculate_daily_averages(df: pd.DataFrame, group_by: str = "date") -> pd.DataFrame:

    columns = [
        "gpu_s",
        "sieve_s",
        "large_s",
        "check_s",
        "offsets_s",
        "sync_s",
        "wall_s",
        "evens_per_sec",
    ]

    grouped = df.groupby(group_by)
    daily = grouped[columns].mean()

    # When the runs of each section happened, and how many there were.
    daily["t_first"] = grouped["timestamp"].min()
    daily["t_last"] = grouped["timestamp"].max()
    daily["n_runs"] = grouped.size()

    # Window extent (bench.py CSVs; one window per section is guaranteed
    # by the caller unless grouping by label with a --window filter).
    for col in ("start", "limit", "segment", "window", "label", "commit"):
        if col in df.columns:
            daily[col] = grouped[col].agg(
                lambda v: v.iloc[0] if v.nunique() == 1 else "mixed")

    # Preset windows in their logical order (bench, steady, end), then any
    # other key alphabetically.
    order = {w: i for i, w in enumerate(WINDOW_PURPOSE)}
    if group_by == "label":
        # Version labels (v1.13.0, v1.9.0, ...) newest first, numerically
        # (plain string order put v1.10 before v1.6); other labels after them.
        def vkey(k):
            m = re.fullmatch(r"v?(\d+(?:\.\d+)*)", str(k))
            return (0, tuple(-int(x) for x in m.group(1).split("."))) if m else (1, (str(k),))
        return daily.sort_index(key=lambda idx: [vkey(k) for k in idx])
    return daily.sort_index(key=lambda idx: [(order.get(str(k), len(order)), str(k)) for k in idx]
                            if group_by == "window" else idx)


def fmt_span(t0, t1):
    """'2026-09-24 16:12-16:25 CEST' (or across days: full stamps)."""
    tz = t0.strftime("%Z")
    if t0.date() == t1.date():
        return f"{t0:%Y-%m-%d %H:%M:%S}\u2013{t1:%H:%M:%S} {tz}"
    return f"{t0:%Y-%m-%d %H:%M:%S} \u2013 {t1:%Y-%m-%d %H:%M:%S} {tz}"


def sci(x):
    """4000000000000000000 -> '4e18', 4000000020000000000 -> '4.00000002e18'."""
    m, e = f"{float(x):.12e}".split("e")
    return f"{m.rstrip('0').rstrip('.')}e{int(e)}"


def describe_window(row):
    """One-line description of a window from its start/limit/segment."""
    if "start" not in row or "mixed" in (str(row.get("start")), str(row.get("limit"))):
        return ""
    try:
        start, limit = int(row["start"]), int(row["limit"])
        seg = int(row["segment"])
    except (TypeError, ValueError):
        return ""
    span = limit - start
    segments = -(-span // seg) if seg else 0
    return (f"evens in [{sci(start)}, {sci(limit)}]: {sci(span)} integers, "
            f"{(span // 2 + 1) / 1e9:.1f} B evens, {segments} segments of {sci(seg)}")


# ============================================================
# Draw one waterfall section
# ============================================================

def draw_waterfall_section(
    ax,
    date,
    row,
    xmax,
    what="DAILY AVERAGE",
    phases=PHASES,
):
    """
    Draw one daily-average Chrome-DevTools-style
    timing waterfall.

    Timeline:

        0
        |
        +---- init ----+
                      |
                      +---- sieve ----+
                                     |
                                     +---- large ----+
                                                    |
                                                    +-- check --+
                                                               |
                                                               + offsets +
                                                                         |
                                                                         + sync

    All times are milliseconds.
    """

    # --------------------------------------------------------
    # Overall timings
    # --------------------------------------------------------

    wall_ms = row["wall_s"] * 1000.0
    gpu_ms = row["gpu_s"] * 1000.0

    # Everything before GPU execution.
    #
    # This follows the visualization model used previously:
    #
    #     wall = init + gpu
    #
    init_ms = max(
        0.0,
        wall_ms - gpu_ms,
    )

    gpu_start = init_ms

    # --------------------------------------------------------
    # Rows
    # --------------------------------------------------------

    labels = ["wall_s", "init", "gpu_s", *phases]

    y = {
        label: i
        for i, label in enumerate(labels)
    }

    ax.set_yticks(
        list(y.values())
    )

    ax.set_yticklabels(
        list(y.keys())
    )

    # Chrome DevTools-like ordering:
    # wall_s at bottom, sync_s at top.
    ax.invert_yaxis()

    # --------------------------------------------------------
    # wall_s
    # --------------------------------------------------------

    ax.add_patch(
        Rectangle(
            (
                0,
                y["wall_s"] - 0.28,
            ),
            wall_ms,
            0.56,
            fill=False,
            linewidth=2.0,
        )
    )

    # --------------------------------------------------------
    # Initialization
    # --------------------------------------------------------

    ax.add_patch(
        Rectangle(
            (
                0,
                y["init"] - 0.22,
            ),
            init_ms,
            0.44,
            alpha=0.55,
        )
    )

    # --------------------------------------------------------
    # GPU envelope
    # --------------------------------------------------------

    ax.add_patch(
        Rectangle(
            (
                gpu_start,
                y["gpu_s"] - 0.22,
            ),
            gpu_ms,
            0.44,
            fill=False,
            linewidth=1.8,
        )
    )

    # --------------------------------------------------------
    # Continuous GPU phases
    # --------------------------------------------------------

    left = gpu_start

    for phase in phases:

        duration_ms = row[phase] * 1000.0

        ax.add_patch(
            Rectangle(
                (
                    left,
                    y[phase] - 0.22,
                ),
                duration_ms,
                0.44,
                alpha=0.82,
            )
        )

        # Put the timing directly on the phase.
        # Inside the bar when it fits, otherwise just right of it.
        if duration_ms > 0:
            inside = duration_ms > 0.07 * xmax
            ax.text(
                left + duration_ms / 2.0 if inside else left + duration_ms + 0.006 * xmax,
                y[phase],
                f"{duration_ms:.1f} ms",
                ha="center" if inside else "left",
                va="center",
                fontsize=8 if inside else 8.5,
                color="white" if inside else "black",
            )

        # CRITICAL:
        # The next phase starts where the previous one ends.
        left += duration_ms

    # --------------------------------------------------------
    # Numeric labels for overall timings
    # --------------------------------------------------------

    def label_x(x):
        return min(
            x + 8,
            xmax - 5,
        )

    ax.text(
        label_x(wall_ms),
        y["wall_s"],
        f"{wall_ms:.2f} ms",
        va="center",
        fontsize=9,
    )

    ax.text(
        label_x(init_ms),
        y["init"],
        f"{init_ms:.2f} ms",
        va="center",
        fontsize=9,
    )

    ax.text(
        label_x(gpu_start + gpu_ms),
        y["gpu_s"],
        f"{gpu_ms:.2f} ms",
        va="center",
        fontsize=9,
    )

    # --------------------------------------------------------
    # Section title
    # --------------------------------------------------------

    # GPU rate = the bars from sieve to sync; end-to-end = the wall_s bar
    # (includes startup), so the headline matches what the bars show.
    throughput = row["evens_per_sec"] / 1e9
    e2e = row.get("e2e_evens_per_sec", float("nan")) / 1e9
    ax.set_title(
        f"{date}  —  {what}  |  GPU {throughput:.3f} B evens/s"
        + (f"  |  end-to-end {e2e:.3f}" if e2e == e2e else ""),
        loc="left",
        fontsize=12,
    )

    # Right: when these runs happened and how many are averaged; below it
    # (inside the header line) the window this section measured.
    right = f"{fmt_span(row['t_first'], row['t_last'])}  \u00b7  {int(row['n_runs'])} runs"
    extra = []
    if "window" in row and str(row["window"]) not in ("mixed", "nan"):
        extra.append(f"window {row['window']}")
    if "label" in row and str(row["label"]) not in ("mixed", "nan"):
        extra.append(f"build {row['label']}")
    if "commit" in row and str(row["commit"]) not in ("mixed", "nan"):
        extra.append(str(row["commit"]))
    if extra:
        right = "  \u00b7  ".join(extra) + "\n" + right
    ax.set_title(right, loc="right", fontsize=9, color="0.25")

    # --------------------------------------------------------
    # Axis
    # --------------------------------------------------------

    ax.set_xlim(
        0,
        xmax,
    )

    ax.set_ylim(
        -0.65,
        len(labels) - 0.35,
    )

    ax.grid(
        axis="x",
        alpha=0.22,
    )

    ax.set_axisbelow(True)


# ============================================================
# Create graph
# ============================================================

def create_waterfall(
    df: pd.DataFrame,
    output: Path,
    group_by: str = "date",
    subtitle: str = "",
    per: float = 0.0,
    xmax: float = 0.0,
    print_xmax: bool = False,
):
    daily = calculate_daily_averages(df, group_by)

    # --------------------------------------------------------
    # One common scale for EVERY date.
    # --------------------------------------------------------

    # End-to-end rate from the mean wall time of the same work the bars show.
    if per:
        daily["e2e_evens_per_sec"] = (per / 2) / daily["wall_s"]
    elif {"start", "limit"} <= set(daily.columns):
        span = pd.to_numeric(daily["limit"], errors="coerce") - pd.to_numeric(daily["start"], errors="coerce")
        daily["e2e_evens_per_sec"] = (span / 2) / daily["wall_s"]

    largest_wall_ms = (
        daily["wall_s"].max() * 1000.0
    )

    # --xmax shares one axis across separate figures (docs/make-plots.sh).
    xmax = xmax or largest_wall_ms * 1.10
    if print_xmax:
        print(f"XMAX {largest_wall_ms * 1.10:.3f}")
        return

    # --------------------------------------------------------
    # Figure
    # --------------------------------------------------------

    number_of_days = len(daily)

    # Header band (inches) for the main title, added ON TOP of the
    # 2.9 in per section so the sections keep their full height.
    header_in = 1.0 + (0.35 if subtitle else 0.0)

    # Footer: what each window in the plot is, and when the plot was made.
    footer = []
    if "window" in df.columns:
        order = {w: i for i, w in enumerate(WINDOW_PURPOSE)}
        for wname, wrows in sorted(df.groupby("window"),
                                   key=lambda kv: (order.get(str(kv[0]), len(order)), str(kv[0]))):
            first = calculate_daily_averages(wrows, "window").iloc[0]
            desc = describe_window(first)
            purpose = WINDOW_PURPOSE.get(str(wname), "custom window")
            footer.append(f"{wname}: {desc}  \u2014  {purpose}")
    t0, t1 = df["timestamp"].min(), df["timestamp"].max()
    footer.append(f"runs {fmt_span(t0, t1)}  \u00b7  {len(df)} PASS runs  \u00b7  "
                  f"generated {dt.datetime.now().astimezone():%Y-%m-%d %H:%M %Z}  \u00b7  "
                  "bars = mean per section; init = wall - gpu (process start, "
                  "driver, prime tables, upload)"
)
    footer_in = 0.75 + 0.24 * len(footer)

    fig, axes = plt.subplots(
        number_of_days,
        1,
        figsize=(
            14,
            2.9 * number_of_days + header_in + footer_in,
        ),
        sharex=True,
        gridspec_kw={
            "hspace": 0.55,
        },
    )

    # Matplotlib returns a single Axes instead of an array
    # when there is only one subplot.
    if number_of_days == 1:
        axes = [axes]

    # --------------------------------------------------------
    # Draw every day
    # --------------------------------------------------------

    what = "DAILY AVERAGE" if group_by == "date" else "AVERAGE"
    # Skip phases that are zero in every section (offsets_s in current builds).
    phases = [p for p in PHASES if daily[p].abs().sum() > 0]

    for ax, (date, row) in zip(
        axes,
        daily.iterrows(),
    ):

        draw_waterfall_section(
            ax=ax,
            date=date,
            row=row,
            xmax=xmax,
            what=what,
            phases=phases,
        )

    # --------------------------------------------------------
    # Shared x-axis
    # --------------------------------------------------------

    axes[-1].set_xlabel(
        (f"Time per {sci(per)} integers (ms; startup amortized over the window) "
         if per else "Elapsed time from request start (ms) ")
        + f"— SAME SCALE FOR EVERY {group_by.upper()}"
    )

    # --------------------------------------------------------
    # Main title
    # --------------------------------------------------------

    title = ("GPU Benchmark — Continuous Waterfall by Day" if group_by == "date"
             else f"GPU Benchmark — Continuous Waterfall by {group_by.capitalize()}")
    if subtitle:
        title += f"\n{subtitle}"

    # Reserve a fixed header band (in inches) for the main title so it never
    # overlaps the first section's title — with a single section the old
    # fractional y=0.998 placed it on top of the plot.
    fig_h = fig.get_size_inches()[1]
    fig.subplots_adjust(top=1.0 - header_in / fig_h, bottom=footer_in / fig_h)
    for i, line in enumerate(footer):
        fig.text(0.01, (footer_in - 0.62 - 0.24 * i) / fig_h, line,
                 fontsize=9, color="0.25", ha="left", va="top")

    fig.suptitle(
        title,
        fontsize=17,
        y=1.0 - 0.15 / fig_h,
        va="top",
    )

    # --------------------------------------------------------
    # Save
    # --------------------------------------------------------

    output.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    plt.savefig(
        output,
        dpi=180,
        bbox_inches="tight",
    )

    plt.close(fig)

    # --------------------------------------------------------
    # Print summary
    # --------------------------------------------------------

    print()
    print("=" * 70)
    print("GPU BENCHMARK WATERFALL")
    print("=" * 70)

    print(f"Input rows:  {len(df)}")
    print(f"Sections:    {len(daily)} (by {group_by})")
    print(f"Output:      {output}")
    print()

    for date, row in daily.iterrows():

        print(
            f"{date}: "
            f"{row['evens_per_sec'] / 1e9:.3f} B evens/s | "
            f"GPU {row['gpu_s'] * 1000:.2f} ms | "
            f"WALL {row['wall_s'] * 1000:.2f} ms"
        )

    print()
    print("=" * 70)


# ============================================================
# Main
# ============================================================

def main():

    parser = argparse.ArgumentParser(
        description=(
            "Create a continuous GPU timing waterfall "
            "from a benchmark CSV."
        )
    )

    parser.add_argument(
        "csv",
        type=Path,
        help="Input benchmark CSV",
    )

    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("gpu_daily_waterfall.png"),
        help="Output PNG path",
    )

    parser.add_argument("--window", help="only rows of this window (bench.py CSVs; comma list ok)")
    parser.add_argument("--label", help="only rows of these arm labels, e.g. v1.8.0,v1.7.0")
    parser.add_argument(
        "--per", default="auto",
        help="normalize times to this many integers (e.g. 1e10) so windows of different "
             "size share one meaningful scale; 'none' = raw ms; 'auto' (default) = 1e10 "
             "when the plotted windows differ in size")
    parser.add_argument("--latest", action="store_true",
                        help="per label and window, plot only the most recent bench.py session")
    parser.add_argument("--xmax", type=float, default=0.0,
                        help="x-axis limit in ms (to put several figures on one scale)")
    parser.add_argument("--print-xmax", action="store_true",
                        help="print the x-axis limit this figure would use and exit")
    parser.add_argument(
        "--group-by",
        choices=["date", "label", "window"],
        default="date",
        help="one section per date (default), arm label, or window",
    )

    args = parser.parse_args()

    # Load CSV.
    df = load_csv(
        args.csv
    )

    subtitle = []
    for col, val in (("window", args.window), ("label", args.label)):
        if val is None:
            continue
        if col not in df.columns:
            raise SystemExit(f"--{col} given but the CSV has no '{col}' column")
        df = df[df[col].astype(str).isin(val.split(","))]   # comma list allowed
        subtitle.append(f"{col}={val}")
    if df.empty:
        raise SystemExit("no PASS rows match the filters")
    # --latest: per (label, window), only the most recent bench.py session,
    # so a section never averages builds or conditions from different days.
    if args.latest:
        if "session" not in df.columns:
            raise SystemExit("--latest needs a 'session' column (bench.py journals)")
        keys = [c for c in ("label", "window") if c in df.columns]
        sess = df["session"].fillna("").astype(str)
        df = df[sess == sess.groupby([df[k] for k in keys]).transform("max")]
        subtitle.append("latest session")
    if args.group_by in ("label", "window") and args.group_by not in df.columns:
        raise SystemExit(f"--group-by {args.group_by}: the CSV has no '{args.group_by}' column")
    # Averaging different windows together would mix 2e9- and 2e10-integer
    # runs into one meaningless bar: require a window filter in that case.
    if "window" in df.columns and args.group_by != "window" and df["window"].nunique() > 1:
        raise SystemExit("rows from several windows (" + ", ".join(sorted(df["window"].astype(str).unique()))
                         + ") — pick one with --window, or use --group-by window")

    # Normalize every run to the same amount of work, so a 2e9 window and a
    # 1e11 window can share one time axis (otherwise the short one is a sliver).
    have_span = {"start", "limit"} <= set(df.columns)
    if args.per == "none":
        per = 0.0
    elif args.per == "auto":
        per = 1e10 if have_span and (df["limit"].astype(float) - df["start"].astype(float)).nunique() > 1 else 0.0
    else:
        per = float(args.per)
    if per:
        if not have_span:
            raise SystemExit("--per needs start/limit columns (bench.py CSVs)")
        df = df.copy()
        scale = per / (df["limit"].astype(float) - df["start"].astype(float))
        for col in ("gpu_s", "sieve_s", "large_s", "check_s", "offsets_s", "sync_s", "wall_s"):
            df[col] = df[col] * scale
        subtitle.append(f"per {sci(per)} integers")

    # Create image.
    create_waterfall(
        df=df,
        output=args.output,
        group_by=args.group_by,
        subtitle="  ".join(subtitle),
        per=per,
        xmax=args.xmax,
        print_xmax=args.print_xmax,
    )


if __name__ == "__main__":
    main()
