#!/usr/bin/env python3
"""gpu_watch.py — run a command while passively sampling the B580 via gputop.

The B580 uses the Xe kernel driver: intel_gpu_top does NOT support it
("Detected Xe device ... use 'gputop' instead"). gputop prints ASCII frames
(no JSON), so this wrapper polls `gputop -n 1` and parses each frame's
header (GT frequencies) and per-PID render-engine (rcs) busy percentages.

Purpose: diagnosing the B580's per-launch bimodality (whole runs at ~7.5 vs
~6.4 G ev/s) — is the slow mode a frequency clamp, or a busy% effect? The
sampler only reads counters; it does not touch the GPU, so it cannot distort
what it measures (sanity-check once: watched vs unwatched throughput should
match within run-to-run noise).

usage: gpu_watch.py [--interval SEC] [--out CSV] -- <command...>
example (from a verifier tree root):
  python3 ../goldbach-tools/gpu_watch.py --out watch.csv -- \
      ./build/goldbach_l0 --start 4e18 --limit 4000000200000000000 --csv
"""
import argparse
import csv
import datetime
import re
import statistics
import subprocess
import sys
import time

# Header example: "DRM minor 128   Frequency(MHz) GT0-1200/1550 GT1-1200/1200"
FREQ_RE = re.compile(r"Frequency\(MHz\)\s+GT0-(\d+)/(\d+)")
PID_LINE_RE = re.compile(r"^\s*(\d+)\s+\S+\s+\S+\s+\|\s*([\d.]+)%")


def parse_frame(text):
    """Return {'gt0_cur': int, 'gt0_max': int, 'rcs_busy': float} or None."""
    m = FREQ_RE.search(text)
    if not m:
        return None
    rcs = 0.0
    for line in text.splitlines():
        pm = PID_LINE_RE.match(line)
        if pm:
            rcs += float(pm.group(2))
    return {"gt0_cur": int(m.group(1)), "gt0_max": int(m.group(2)),
            "rcs_busy": rcs}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--interval", type=float, default=0.5,
                    help="sample interval in seconds (default 0.5)")
    ap.add_argument("--out", default=None,
                    help="append per-sample rows to this CSV journal")
    ap.add_argument("command", nargs=argparse.REMAINDER,
                    help="command after '--'")
    args = ap.parse_args()
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    if not args.command:
        ap.error("no command given (put it after '--')")

    samples = []

    def sample_once():
        try:
            out = subprocess.run(["gputop", "-n", "1", "-d",
                                  str(args.interval)],
                                 capture_output=True, text=True,
                                 timeout=args.interval * 4).stdout
            f = parse_frame(out)
            if f:
                f["ts"] = datetime.datetime.now(
                    datetime.timezone.utc).isoformat()
                samples.append(f)
        except (subprocess.TimeoutExpired, OSError):
            pass

    sample_once()  # priming read (first frame may be partial)
    proc = subprocess.Popen(args.command)
    while proc.poll() is None:
        sample_once()
    proc.wait()

    if args.out:
        with open(args.out, "a", newline="") as f:
            w = csv.DictWriter(f, fieldnames=["ts", "gt0_cur", "gt0_max",
                                              "rcs_busy"])
            if f.tell() == 0:
                w.writeheader()
            for s in samples:
                w.writerow(s)
        print(f"{len(samples)} samples appended to {args.out}",
              file=sys.stderr)

    def series(name):
        return [float(s[name]) for s in samples if name in s]

    for name, unit in (("gt0_cur", "MHz"), ("rcs_busy", "% (sum over PIDs)")):
        xs = series(name)
        if xs:
            print(f"{name:>9}: min {min(xs):7.1f}  median "
                  f"{statistics.median(xs):7.1f}  max {max(xs):7.1f} {unit}")
    sys.exit(proc.returncode)


if __name__ == "__main__":
    main()
