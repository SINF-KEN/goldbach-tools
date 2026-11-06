#!/usr/bin/env python3
"""sweep.py — atomic parameter sweeps for goldbach-l0.

Each measurement is a FRESH goldbach_l0 process (isolated worker, like the
tuner): no state leaks between configs, one crash/timeout cannot poison the
next point. Results are appended to a CSV journal and summarized as a table.

usage: sweep.py [--bin PATH] [--start N] [--limit N] [--spv FILE]
                [--cache FILE] [--runs N] [--timeout S] [--out CSV]
                [--segment LIST] [--split LIST] [--quick]

       sweep.py --ab [--binA PATH] [--binA-spv FILE] [--envA V=1[,...]]
                     [--binB PATH] [--binB-spv FILE] [--envB V=1[,...]]
                     [--pairs N] [same window args]

examples:
  ./tools/sweep.py --segment 500000000,1000000000,2000000000 --split 4000
  ./tools/sweep.py --split 3000,4000,5000,7000 --segment 500000000 --runs 3
  ./tools/sweep.py --quick --split 4000,7000

Interleaved A/B mode (for launch-bimodal hardware — sequential AAAA/BBBB
blocks are not trustworthy):

  # validate the harness itself: A = B must show ~zero systematic difference
  ./tools/sweep.py --ab --start 4000000000000000000 \\
      --limit 4000000002000000000 --segment 250000000 --split 0 --pairs 8

  # real experiment: same binary, candidate enabled per-arm via env
  ./tools/sweep.py --ab --envB GB_TILE_EXPERIMENT=1 \\
      --start 4000000000000000000 --limit 4000000002000000000 --pairs 8

Every launch (A B A B ...) is appended to the journal with detail tags
"ab arm=... pair=..." so raw per-launch numbers stay comparable with sweep
rows; the summary prints per-arm median/p90/p95/p99 + CV for gpu_s, total_s
and wall_s, per-phase medians, and PAIRED (A_i - B_i) deltas.
"""

import argparse
import csv
import json
import os
import signal
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def parse_list(text, cast=int):
    return [cast(x) for x in text.split(",") if x.strip()]


def run_worker(args, timeout, extra_env=None):
    """Run one isolated goldbach_l0 process; return parsed dict or error."""
    t0 = time.monotonic()
    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)
    try:
        proc = subprocess.run(
            args, cwd=ROOT, capture_output=True, text=True, timeout=timeout, env=env
        )
    except subprocess.TimeoutExpired:
        return {"status": "timeout", "wall": time.monotonic() - t0}
    wall = time.monotonic() - t0
    if proc.returncode != 0:
        tail = (proc.stderr or proc.stdout or "").strip().splitlines()
        return {"status": f"exit{proc.returncode}",
                "detail": tail[-1] if tail else "", "wall": wall}
    out = proc.stdout
    if "result=PASS" not in out and '"result": "PASS"' not in out:
        return {"status": "nopass", "detail": out[-200:], "wall": wall}
    rec = {"status": "pass", "wall": wall}
    # Parse the --csv key=value line (fall back to human format keys).
    for line in out.splitlines():
        if "=" in line and "result=" in line:
            for tok in line.split():
                if "=" in tok:
                    k, _, v = tok.partition("=")
                    rec[k] = v
            break
    else:
        for key, tag in [("gpu_s", "gpu time       : "), ("evens_per_sec", "evens/second   : "),
                         ("sieve_s", "sieve        : "), ("large_s", "large sieve  : "),
                         ("check_s", "verify       : "), ("offsets_s", "offsets      : "),
                         ("sync_s", "sync/other   : ")]:
            pos = out.find(tag)
            if pos >= 0:
                rec[key] = out[pos + len(tag):].split()[0]
    return rec


def f(rec, key, default=0.0):
    try:
        return float(rec[key])
    except (KeyError, ValueError):
        return default


def pct(sorted_vals, p):
    """p-th percentile (0..100) of an already-sorted list; None if empty."""
    if not sorted_vals:
        return None
    idx = min(len(sorted_vals) - 1, max(0, round(p / 100 * (len(sorted_vals) - 1))))
    return sorted_vals[idx]


def parse_env(text):
    """'GB_X=1,GB_Y=2' -> {'GB_X': '1', 'GB_Y': '2'}"""
    env = {}
    if text:
        for item in text.split(","):
            if "=" in item:
                k, _, v = item.partition("=")
                env[k.strip()] = v.strip()
    return env


def run_ab(args):
    """Interleaved A/B A/B benchmarking of two binary+spv+env configurations.

    Every launch is a fresh process, appended to the journal with detail tags
    (established schema preserved); the summary compares per-arm medians and
    PAIRED deltas so launch-to-launch bimodality cancels.
    """
    armA = {"bin": args.binA or args.bin, "spv": args.binA_spv or args.spv,
            "env": parse_env(args.envA)}
    # B falls back to the standard --bin/--spv (NOT to arm A: comparing a
    # --binA override against the default build is the common case).
    armB = {"bin": args.binB or args.bin, "spv": args.binB_spv or args.spv,
            "env": parse_env(args.envB)}
    for name, arm in (("A", armA), ("B", armB)):
        if not os.path.exists(arm["bin"]):
            sys.exit(f"arm {name} binary not found: {arm['bin']} (build first: bash build.sh)")
        if not os.path.exists(arm["spv"]):
            sys.exit(f"arm {name} spv not found: {arm['spv']}")

    header = ["timestamp", "segment", "split", "run", "status", "gpu_s",
              "sieve_s", "large_s", "check_s", "offsets_s", "sync_s",
              "evens_per_sec", "wall_s", "detail"]
    new_file = not os.path.exists(args.out)
    journal = open(args.out, "a", newline="")
    writer = csv.writer(journal)
    if new_file:
        writer.writerow(header)

    def launch(arm, pair):
        rec = run_worker(
            [arm["bin"], "--limit", str(args.limit), "--start", str(args.start),
             "--segment", str(args.segment), "--split", str(args.split),
             "--spv", arm["spv"], "--prime-cache", args.cache, "--csv"],
            timeout=args.timeout, extra_env=arm["env"])
        detail = (f"ab arm={arm['tag']} pair={pair} total_s={rec.get('total_s', '')} "
                  f"init_s={rec.get('init_s', '')} bin={os.path.basename(arm['bin'])}")
        writer.writerow([
            datetime.now(timezone.utc).isoformat(timespec="seconds"),
            args.segment, args.split, pair, rec.get("status", "?"),
            rec.get("gpu_s", ""), rec.get("sieve_s", ""),
            rec.get("large_s", ""), rec.get("check_s", ""),
            rec.get("offsets_s", ""), rec.get("sync_s", ""),
            rec.get("evens_per_sec", ""),
            f"{rec.get('wall', 0):.3f}", detail])
        journal.flush()
        print(f"  pair {pair} {arm['tag']}: {rec.get('status', '?'):>6}  "
              f"gpu_s={rec.get('gpu_s', '-')}  total_s={rec.get('total_s', '-')}")
        return rec

    recs = {"A": [], "B": []}
    for pair in range(1, args.pairs + 1):
        # alternate which arm goes first to cancel any position bias
        order = ("A", "B") if pair % 2 == 1 else ("B", "A")
        for tag in order:
            arm = dict(armA if tag == "A" else armB)
            arm["tag"] = tag
            recs[tag].append(launch(arm, pair))
    journal.close()

    print(f"\n=== A/B summary ({args.pairs} interleaved pairs, "
          f"[{args.start}, {args.limit}], seg {int(args.segment):,} split {args.split}) ===")
    for name, arm in (("A", armA), ("B", armB)):
        env = f" env={arm['env']}" if arm["env"] else ""
        print(f"arm {name}: {arm['bin']} + {arm['spv']}{env}")
    stats = {}
    for tag in ("A", "B"):
        good = [r for r in recs[tag] if r.get("status") == "pass"]
        fails = len(recs[tag]) - len(good)
        s = {"fails": fails}
        for key in ("gpu_s", "total_s", "wall", "sieve_s", "large_s", "check_s"):
            vals = sorted(f(r, key) for r in good)
            s[key] = vals
        stats[tag] = s
        if good:
            v = s["gpu_s"]
            med = statistics.median(v)
            cv = (statistics.pstdev(v) / statistics.mean(v) * 100) if len(v) > 1 and statistics.mean(v) > 0 else float("nan")
            rate_med = statistics.median([f(r, "evens_per_sec") for r in good])
            print(f"arm {tag}: n={len(recs[tag])} pass={len(good)} fail={fails}  "
                  f"gpu_s med={med:.4f} p90={pct(v, 90):.4f} p95={pct(v, 95):.4f} "
                  f"p99={pct(v, 99):.4f} CV={cv:.1f}%  "
                  f"total_s med={statistics.median(s['total_s']):.4f}  "
                  f"wall_s med={statistics.median(s['wall']):.4f}  "
                  f"G ev/s med={rate_med / 1e9:.2f}")
            print(f"        phases med: sieve={statistics.median(s['sieve_s']):.4f} "
                  f"large={statistics.median(s['large_s']):.4f} "
                  f"check={statistics.median(s['check_s']):.4f}")
        else:
            print(f"arm {tag}: ALL RUNS FAILED")

    # paired deltas: A_i - B_i within each pair (bimodality cancels)
    a = [r for r in recs["A"]]
    b = [r for r in recs["B"]]
    for key, label in (("gpu_s", "gpu_s"), ("total_s", "total_s")):
        deltas = []
        for ra, rb in zip(a, b):
            if ra.get("status") == "pass" and rb.get("status") == "pass":
                deltas.append(f(ra, key) - f(rb, key))
        if deltas:
            med = statistics.median(deltas)
            rel = med / statistics.median([f(rb, key) for rb in b]) * 100 if b else float("nan")
            wins = sum(1 for d in deltas if d < 0)
            print(f"paired {label} deltas (A-B): median {med:+.4f} s ({rel:+.2f}% of B)  "
                  f"A faster in {wins}/{len(deltas)} pairs")
    print("raw per-launch rows appended to", args.out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "goldbach_l0"))
    ap.add_argument("--start", type=int, default=3_999_999_999_000_000_000)
    ap.add_argument("--limit", type=int, default=4_000_000_000_000_000_000)
    ap.add_argument("--spv", default=os.path.join(ROOT, "kernels", "goldbach_bmg.spv"))
    ap.add_argument("--cache", default=os.path.join(ROOT, "base_primes.cache"))
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--out", default=os.path.join(ROOT, "sweep-results.csv"))
    ap.add_argument("--segment", default="500000000",
                    help="comma-separated segment spans")
    ap.add_argument("--split", default="4000",
                    help="comma-separated split indices")
    ap.add_argument("--quick", action="store_true", help="1 run per point")
    # ---- interleaved A/B mode ----
    ap.add_argument("--ab", action="store_true",
                    help="interleaved A/B A/B benchmarking (see module docstring)")
    ap.add_argument("--binA", default=None, help="arm A binary (default: --bin)")
    ap.add_argument("--binA-spv", dest="binA_spv", default=None,
                    help="arm A spv (default: --spv)")
    ap.add_argument("--binB", default=None, help="arm B binary (default: same as A)")
    ap.add_argument("--binB-spv", dest="binB_spv", default=None,
                    help="arm B spv (default: same as A)")
    ap.add_argument("--envA", default=None,
                    help="comma-separated VAR=VAL env for arm A launches")
    ap.add_argument("--envB", default=None,
                    help="comma-separated VAR=VAL env for arm B launches")
    ap.add_argument("--pairs", type=int, default=8, help="A/B pairs (default 8)")
    args = ap.parse_args()

    if args.ab:
        if not os.path.exists(args.bin):
            sys.exit(f"worker not found: {args.bin} (build first: bash build.sh)")
        run_ab(args)
        return

    if args.quick:
        args.runs = 1
    segments = parse_list(args.segment)
    splits = parse_list(args.split)
    if not os.path.exists(args.bin):
        sys.exit(f"worker not found: {args.bin} (build first: bash build.sh)")

    header = ["timestamp", "segment", "split", "run", "status", "gpu_s",
              "sieve_s", "large_s", "check_s", "offsets_s", "sync_s",
              "evens_per_sec", "wall_s", "detail"]
    new_file = not os.path.exists(args.out)
    journal = open(args.out, "a", newline="")
    writer = csv.writer(journal)
    if new_file:
        writer.writerow(header)

    results = []
    total = len(segments) * len(splits)
    done = 0
    print(f"sweep: {len(segments)} segments x {len(splits)} splits x "
          f"{args.runs} runs -> {args.out}")
    for seg in segments:
        for split in splits:
            samples = []
            for r in range(1, args.runs + 1):
                rec = run_worker(
                    [args.bin, "--limit", str(args.limit), "--start", str(args.start),
                     "--segment", str(seg), "--split", str(split),
                     "--spv", args.spv, "--prime-cache", args.cache, "--csv"],
                    timeout=args.timeout)
                writer.writerow([
                    datetime.now(timezone.utc).isoformat(timespec="seconds"),
                    seg, split, r, rec.get("status", "?"),
                    rec.get("gpu_s", ""), rec.get("sieve_s", ""),
                    rec.get("large_s", ""), rec.get("check_s", ""),
                    rec.get("offsets_s", ""), rec.get("sync_s", ""),
                    rec.get("evens_per_sec", ""),
                    f"{rec.get('wall', 0):.3f}", rec.get("detail", "")])
                journal.flush()
                if rec.get("status") != "pass":
                    print(f"  [{done+1}/{total}] seg={seg} split={split} "
                          f"run {r}: {rec.get('status')} {rec.get('detail','')}")
                    samples = []
                    break
                samples.append(rec)
            done += 1
            if not samples:
                continue
            gpu = [f(s, "gpu_s", f(s, "gpu")) for s in samples]
            rate = [f(s, "evens_per_sec") for s in samples]
            med_gpu = statistics.median(gpu)
            med_rate = statistics.median(rate)
            cv = (statistics.pstdev(gpu) / statistics.mean(gpu) * 100
                  if len(gpu) > 1 and statistics.mean(gpu) > 0 else 0.0)
            results.append((seg, split, med_gpu, med_rate, cv))
            print(f"  [{done}/{total}] seg={seg:>11,} split={split:>7,}  "
                  f"gpu {med_gpu:7.3f}s  {med_rate/1e9:6.2f} G ev/s  "
                  f"CV {cv:4.1f}%  ({len(samples)} runs)")

    journal.close()

    if results:
        print("\n=== summary (median) ===")
        print(f"{'segment':>13} {'split':>8} {'gpu_s':>9} {'G ev/s':>8} {'CV%':>6}")
        for seg, split, gpu, rate, cv in sorted(results, key=lambda x: -x[3]):
            print(f"{seg:>13,} {split:>8,} {gpu:>9.3f} {rate/1e9:>8.2f} {cv:>6.1f}")
        best = max(results, key=lambda x: x[3])
        print(f"\nbest: segment={best[0]:,} split={best[1]:,} "
              f"-> {best[3]/1e9:.2f} G ev/s")


if __name__ == "__main__":
    main()
