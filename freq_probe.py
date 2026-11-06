#!/usr/bin/env python3
"""freq_probe.py — GPU render-clock sampling around goldbach_l0 runs.

Answers one question (benchmark-toolsmith, minimal tool): is the large-kernel
fast/slow performance mode correlated with the GT0 render clock? Samples
/sys/.../gt0/freq0/cur_freq in a background thread while fresh goldbach_l0
processes run, classifies each run's mode from its large_s column, and
appends one journal row per run to freq-samples.csv (append-only, new metric
set => new file per gpu-benchmark conventions).

Usage (from project root):
  python3 tools/bench/freq_probe.py --runs 6 \
      --spv kernels/goldbach_bmg.spv --spv /tmp/goldbach_bmg.base.spv
  # --spv may repeat; runs alternate across the list (interleaved A/B).
  # Fixed workload args: production 250M/0 window near 4e18.

Mode classification (from session data): large_s <= 0.085 -> fast,
> 0.085 -> slow (observed fast 0.069-0.078, slow 0.092-0.118).
"""
import argparse
import csv
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

FREQ = "/sys/bus/pci/devices/0000:03:00.0/tile0/gt0/freq0/cur_freq"
ACT = "/sys/bus/pci/devices/0000:03:00.0/tile0/gt0/freq0/act_freq"
ENERGY = "/sys/class/hwmon/hwmon1/energy1_input"   # 'card', microjoules
OUT = Path("perf-state-samples.csv")
HEADER = ["timestamp", "spv", "run", "mode", "status", "gpu_s", "large_s",
          "cur_min", "cur_mean", "act_min", "act_mean",
          "power_w", "detail"]


def read_int(path):
    try:
        with open(path) as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def sample_state(stop, samples):
    while not stop.is_set():
        samples.append((time.monotonic(),
                        read_int(FREQ), read_int(ACT), read_int(ENERGY)))
        time.sleep(0.005)


def parse_csv_line(text):
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("version="):
            return dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build/goldbach_l0")
    ap.add_argument("--runs", type=int, default=6)
    ap.add_argument("--spv", action="append", required=True)
    ap.add_argument("--out", default=str(OUT))
    args = ap.parse_args()

    fixed = ["--start", "3999999998000000000", "--limit", "4000000000000000000",
             "--segment", "250000000", "--split", "0",
             "--prime-cache", "base_primes.cache", "--csv"]

    new_file = not Path(args.out).exists()
    out = open(args.out, "a", newline="")
    wr = csv.writer(out)
    if new_file:
        wr.writerow(HEADER)
        out.flush()

    rows = []
    for i in range(args.runs):
        spv = args.spv[i % len(args.spv)]
        samples = []
        stop = threading.Event()
        th = threading.Thread(target=sample_state, args=(stop, samples), daemon=True)
        th.start()
        t0 = time.monotonic()
        try:
            proc = subprocess.run([args.bin, "--spv", spv] + fixed,
                                  capture_output=True, text=True, timeout=300)
            wall = time.monotonic() - t0
            stop.set(); th.join()
            kv = parse_csv_line(proc.stdout)
            status = "pass" if (kv and kv.get("result") == "PASS") else \
                     f"exit {proc.returncode}"
        except subprocess.TimeoutExpired:
            wall = time.monotonic() - t0
            stop.set(); th.join()
            kv, status = None, "timeout"

        win = [s for s in samples if s[0] >= t0]
        cur = [s[1] for s in win if s[1] is not None]
        act = [s[2] for s in win if s[2] is not None]
        en = [s[3] for s in win if s[3] is not None]
        cmin = min(cur) if cur else 0
        cmean = sum(cur) / len(cur) if cur else 0
        amin = min(act) if act else 0
        amean = sum(act) / len(act) if act else 0
        # energy counters wrap at 2^32 uJ on some hwmon impls; guard deltas
        power_w = 0.0
        if len(en) >= 2 and wall > 0:
            de = en[-1] - en[0]
            if de < 0:
                de += 1 << 32
            power_w = de / 1e6 / wall
        gpu_s = float(kv["gpu_s"]) if kv and "gpu_s" in kv else 0.0
        large_s = float(kv["large_s"]) if kv and "large_s" in kv else 0.0
        mode = "n/a"
        if kv and status == "pass":
            mode = "fast" if large_s <= 0.085 else "slow"
        detail = f"wall={wall:.3f} n_samples={len(win)}"
        ts = datetime.now(timezone.utc).isoformat()
        wr.writerow([ts, Path(spv).name, i + 1, mode, status,
                     f"{gpu_s:.4f}", f"{large_s:.4f}",
                     cmin, f"{cmean:.0f}", amin, f"{amean:.0f}",
                     f"{power_w:.1f}", detail])
        out.flush()
        rows.append((Path(spv).name, mode, status, gpu_s, large_s,
                     cmean, amean, power_w))
        print(f"run {i+1:2d} {Path(spv).name:24s} mode={mode:4s} {status:6s} "
              f"gpu={gpu_s:.4f} large={large_s:.4f} "
              f"cur={cmin}-{cmean:.0f} act={amin}-{amean:.0f} "
              f"pwr={power_w:.1f}W")
    out.close()

    print("\nper-spv, per-mode medians:")
    by = {}
    for name, mode, status, gpu_s, *_ in rows:
        if status == "pass":
            by.setdefault((name, mode), []).append(gpu_s)
    for (name, mode), v in sorted(by.items()):
        v.sort()
        med = v[len(v) // 2] if len(v) % 2 else (v[len(v)//2 - 1] + v[len(v)//2]) / 2
        print(f"  {name:24s} {mode:4s} n={len(v)} gpu_median={med:.4f}")


if __name__ == "__main__":
    sys.exit(main())
