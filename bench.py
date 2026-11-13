#!/usr/bin/env python3
"""bench.py — benchmark harness for goldbach-l0 (stdlib only; works from a TTY).

Every measurement is a FRESH goldbach_l0 process. Rounds rotate through every
(window, arm) pair in alternating order, so slow drift, thermal state and
desktop activity hit every arm equally; the first --warmup runs of each pair
are discarded. While each run executes, a sampler records the B580's actual
clock, package temperature and throttle state, and the card energy counter
gives average power and energy per billion evens.

Rows are APPENDED to a CSV (default: <goldbach-l0>/bench/bench-results.csv) using the
sweep-results.csv schema first —
  timestamp,segment,split,run,status,gpu_s,sieve_s,large_s,check_s,
  offsets_s,sync_s,evens_per_sec,wall_s,detail
— followed by extra columns (label, window, start, limit, init_s, total_s,
leftovers, version, commit, freq_avg_mhz, freq_min_mhz, temp_max_c,
power_avg_w, energy_j, j_per_gevens, throttled, throttle_reasons,
display_on_gpu, other_gpu_pct). tools/waterfall.py plots this file.

Precision: per-process noise is ~0.2-0.35% CV, so run count matters more
than window length beyond ~1e11. --target-ci PCT keeps adding interleaved
rounds until every arm mean and paired ratio has a 95% CI within +-PCT%.
Paired ratios are geometric means of per-round ratios with t-based CIs.
other_gpu_pct is the share of B580 engine time used by OTHER processes
(xe fdinfo drm-cycles) during the run; --exclude-contended PCT drops runs
above that.

Examples (run from anywhere):
  tools/bench.py                                   # final build, 3 windows
  tools/bench.py --windows bench --runs 15
  tools/bench.py --arm final --arm nobucket:GB_BUCKET=0 --windows steady
  tools/bench.py --arm new --arm old:@/path/goldbach_l0@/path/old.spv
Arm syntax: NAME[:ENV=V[,ENV=V...]][:@BINARY[@SPV]]   (SEGMENT=N / SPLIT=N in the ENV list set
that arm's --segment / --split; default: each binary's own default span)
Windows: bench (2e9 @4e18), steady (2e10 @4e18), end (2e10 ending at 5e18),
or NAME=START:LIMIT (plain integers).
"""
import argparse
import math
import csv
import datetime as dt
import os
import random
import re
import statistics as st
import subprocess
import sys
import threading
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
L0_ROOT = (TOOLS / ".." / "goldbach-l0").resolve()
if not (L0_ROOT / "build" / "goldbach_l0").exists():   # invoked via the goldbach-l0/tools symlink
    L0_ROOT = Path.cwd()

PRESET_WINDOWS = {
    "long":   (4000000000000000000, 4000000100000000000),   # 1e11 (235 segments at 425M)
    "long-end": (4999999900000000000, 5000000000000000000),
    "bench":  (4000000000000000000, 4000000002000000000),
    "steady": (4000000000000000000, 4000000020000000000),
    "end":    (4999999980000000000, 5000000000000000000),
}
SCHEMA = ["timestamp", "segment", "split", "run", "status", "gpu_s", "sieve_s",
          "large_s", "check_s", "offsets_s", "sync_s", "evens_per_sec", "wall_s",
          "detail"]
EXTRA = ["label", "window", "start", "limit", "init_s", "total_s", "leftovers",
         "version", "commit", "freq_avg_mhz", "freq_min_mhz", "temp_max_c",
         "power_avg_w", "energy_j", "j_per_gevens", "throttled",
         "throttle_reasons", "display_on_gpu", "other_gpu_pct", "session"]


# ---------------------------------------------------------------- sensors --
def find_b580():
    """Return (drm card dir, hwmon dir, freq dir, render node) of the Intel GPU."""
    for card in sorted(Path("/sys/class/drm").glob("card[0-9]")):
        try:
            if (card / "device" / "vendor").read_text().strip() != "0x8086":
                continue
        except OSError:
            continue
        hw = next(iter(sorted((card / "device" / "hwmon").glob("hwmon*"))), None)
        freq = card / "device" / "tile0" / "gt0" / "freq0"
        render = None
        for r in Path("/sys/class/drm").glob("renderD*"):
            if (r / "device").resolve() == (card / "device").resolve():
                render = r.name
        return card, hw, freq if freq.exists() else None, render
    return None, None, None, None


CARD, HWMON, FREQ, RENDER = find_b580()


def _read(p, default=None):
    try:
        return p.read_text().strip()
    except (OSError, AttributeError):
        return default


def hwmon_value(label_wanted, kind):
    """Read <kind>N_input whose <kind>N_label == label_wanted."""
    if not HWMON:
        return None
    for lab in HWMON.glob(f"{kind}*_label"):
        if _read(lab) == label_wanted:
            v = _read(lab.with_name(lab.name.replace("_label", "_input")))
            return int(v) if v and v.lstrip("-").isdigit() else None
    return None


def display_on_gpu():
    if not CARD:
        return False
    return any(_read(c / "status") == "connected"
               for c in Path("/sys/class/drm").glob(CARD.name + "-*"))


class Sampler:
    """Polls clock / temperature / throttle while a run executes."""

    def __init__(self, period=0.02):
        self.period, self.freqs, self.temps = period, [], []
        self.throttled, self.reasons = False, set()
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._loop, daemon=True)

    def _loop(self):
        while not self._stop.is_set():
            if FREQ:
                f = _read(FREQ / "act_freq")
                if f and f.isdigit() and int(f) > 0:
                    self.freqs.append(int(f))
                if _read(FREQ / "throttle" / "status") == "1":
                    self.throttled = True
                    for r in (FREQ / "throttle").glob("reason_*"):
                        if _read(r) == "1":
                            self.reasons.add(r.name[len("reason_"):])
            t = hwmon_value("pkg", "temp")
            if t is not None:
                self.temps.append(t / 1000.0)
            time.sleep(self.period)

    def __enter__(self):
        self.e0 = hwmon_value("card", "energy")
        self.g0 = other_gpu_snapshot()
        self._t.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        self._t.join()
        self.e1 = hwmon_value("card", "energy")
        self.other_pct = other_gpu_pct(self.g0, other_gpu_snapshot())

    def energy_j(self):
        if self.e0 is None or self.e1 is None or self.e1 < self.e0:
            return None
        return (self.e1 - self.e0) / 1e6


def other_gpu_snapshot():
    """{(pid, fd): {engine: (cycles, total)}} for B580 DRM clients other than
    goldbach_l0 (our own child)."""
    out = {}
    if not CARD:
        return out
    pci = (CARD / "device").resolve().name
    for pd in Path("/proc").glob("[0-9]*"):
        try:
            if _read(pd / "comm", "") == "goldbach_l0":
                continue
            for fi in (pd / "fdinfo").iterdir():
                try:
                    txt = fi.read_text()
                except OSError:
                    continue
                if f"drm-pdev:\t{pci}" not in txt:
                    continue
                cyc = dict(re.findall(r"^drm-cycles-(\w+):\s+(\d+)", txt, re.M))
                tot = dict(re.findall(r"^drm-total-cycles-(\w+):\s+(\d+)", txt, re.M))
                out[(pd.name, fi.name)] = {e: (int(cyc[e]), int(tot.get(e, 0))) for e in cyc}
        except OSError:
            continue
    return out


def other_gpu_pct(a, b):
    """Max over engines of the share of GPU time used by other clients
    between two snapshots (percent)."""
    per_engine = {}
    for key, eng in b.items():
        if key not in a:
            continue
        for e, (c1, t1) in eng.items():
            c0, t0 = a[key].get(e, (c1, t1))
            dt = t1 - t0
            if dt > 0 and c1 >= c0:
                used, span = per_engine.get(e, (0, dt))
                per_engine[e] = (used + (c1 - c0), max(span, dt))
    return max((100.0 * u / t for u, t in per_engine.values() if t > 0), default=0.0)


def busy_b580_users(seconds=1.0):
    """Processes (other than us) whose B580 engine-cycle counters advance."""
    if not RENDER:
        return []
    pci = CARD and (CARD / "device").resolve().name

    def snap():
        out = {}
        for pd in Path("/proc").glob("[0-9]*"):
            try:
                for fi in (pd / "fdinfo").iterdir():
                    txt = fi.read_text()
                    if pci and f"drm-pdev:\t{pci}" not in txt:
                        continue
                    cyc = sum(int(m) for m in re.findall(r"^drm-cycles-\w+:\s+(\d+)", txt, re.M))
                    key = (pd.name, fi.name)
                    out[key] = cyc
            except OSError:
                continue
        return out
    a = snap()
    time.sleep(seconds)
    b = snap()
    busy = {}
    for key, cyc in b.items():
        if key in a and cyc > a[key]:
            busy[key[0]] = busy.get(key[0], 0) + cyc - a[key]
    res = []
    for pid, d in sorted(busy.items(), key=lambda kv: -kv[1]):
        res.append(f"{_read(Path('/proc') / pid / 'comm', '?')}({pid})")
    return res


# ------------------------------------------------------------------- runs --
def parse_arm(spec, default_bin, default_spv):
    parts = spec.split(":")
    name, env, binary, spv = parts[0], {}, default_bin, default_spv
    for p in parts[1:]:
        if p.startswith("@"):
            bits = p[1:].split("@")
            binary = bits[0]
            if len(bits) > 1 and bits[1]:
                spv = bits[1]
        elif p:
            for kv in p.split(","):
                k, _, v = kv.partition("=")
                env[k] = v
    # SEGMENT=N is not an env var: it sets this arm's --segment (arms at
    # their own optimum, e.g. v1.10.1 at 250M vs v1.11.0 at 350M).
    seg = int(env.pop("SEGMENT")) if "SEGMENT" in env else None
    # SPLIT=N likewise sets this arm's --split (split re-tunes as arms).
    spl = int(env.pop("SPLIT")) if "SPLIT" in env else None
    return {"name": name, "env": env, "bin": binary, "spv": spv, "segment": seg, "split": spl}


def parse_window(spec):
    if spec in PRESET_WINDOWS:
        return spec, PRESET_WINDOWS[spec]
    name, _, rng = spec.partition("=")
    a, _, b = rng.partition(":")
    if not (a.isdigit() and b.isdigit()):
        raise SystemExit(f"bad window '{spec}': use a preset or NAME=START:LIMIT")
    return name, (int(a), int(b))


def git_commit(repo=L0_ROOT):
    try:
        head = subprocess.run(["git", "-C", str(repo), "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True, timeout=5).stdout.strip()
        if not head:
            return ""
        return head + (
            "+dirty" if subprocess.run(["git", "-C", str(repo), "status", "--porcelain",
                                        "--untracked-files=no", "--", "src", "include",
                                        "kernels", "CMakeLists.txt"],
                                       capture_output=True, text=True, timeout=5).stdout.strip()
            else "")
    except (OSError, subprocess.SubprocessError):
        return ""


def run_once(arm, wname, window, segment, split, timeout):
    start, limit = window
    if arm.get("segment"):
        segment = arm["segment"]
    if arm.get("split") is not None:
        split = arm["split"]
    cmd = [arm["bin"], "--start", str(start), "--limit", str(limit),
           "--split", str(split), "--csv"]
    if segment:                              # 0: the binary's own default span
        cmd += ["--segment", str(segment)]
    if arm["spv"]:
        cmd += ["--spv", arm["spv"]]
    env = dict(os.environ, **arm["env"])
    ts = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
    with Sampler() as smp:
        t0 = time.perf_counter()
        try:
            p = subprocess.run(cmd, cwd=L0_ROOT, env=env, capture_output=True, text=True,
                               timeout=timeout)
            rc, out, err = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired:
            rc, out, err = -9, "", "TIMEOUT"
        wall = time.perf_counter() - t0
    line = next((l for l in out.splitlines() if "result=" in l), "")
    r = dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
    ok = rc == 0 and r.get("result") == "PASS"
    e = smp.energy_j()
    evens = (limit - start) // 2 + 1
    row = {
        "timestamp": ts, "segment": r.get("segment", segment), "split": r.get("split", split),
        "status": "pass" if ok else "fail",
        "gpu_s": r.get("gpu_s", ""), "sieve_s": r.get("sieve_s", ""),
        "large_s": r.get("large_s", ""), "check_s": r.get("check_s", ""),
        "offsets_s": r.get("offsets_s", ""), "sync_s": r.get("sync_s", ""),
        "evens_per_sec": r.get("evens_per_sec", ""), "wall_s": f"{wall:.4f}",
        "detail": f"bench label={arm['name']} window={wname}"
                  + ("" if ok else f" rc={rc} err={(err or out).strip()[-160:]!r}"),
        "label": arm["name"], "window": wname, "start": start, "limit": limit,
        "init_s": r.get("init_s", ""), "total_s": r.get("total_s", ""),
        "leftovers": r.get("leftovers", ""), "version": r.get("version", ""),
        "freq_avg_mhz": f"{st.mean(smp.freqs):.0f}" if smp.freqs else "",
        "freq_min_mhz": min(smp.freqs) if smp.freqs else "",
        "temp_max_c": f"{max(smp.temps):.1f}" if smp.temps else "",
        "power_avg_w": f"{e / wall:.1f}" if e else "",
        "energy_j": f"{e:.2f}" if e else "",
        "j_per_gevens": f"{e / (evens / 1e9):.3f}" if e else "",
        "throttled": int(smp.throttled),
        "other_gpu_pct": f"{smp.other_pct:.2f}",
        "throttle_reasons": "+".join(sorted(smp.reasons)),
    }
    return row


# ------------------------------------------------------------------ stats --
def boot_ci(xs, stat=st.median, n=2000, seed=1):
    if len(xs) < 3:
        return (float("nan"), float("nan"))
    rnd = random.Random(seed)
    bs = sorted(stat([rnd.choice(xs) for _ in xs]) for _ in range(n))
    return bs[int(0.025 * n)], bs[int(0.975 * n) - 1]


_T975 = [12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
         2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
         2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042]


def t975(df):
    return _T975[df - 1] if df <= 30 else 1.960 + 2.4 / df


def mean_ci_pct(xs):
    """(mean, 95% t-CI half-width as % of the mean); nan with < 3 samples."""
    if len(xs) < 3:
        return (st.mean(xs) if xs else float("nan"), float("nan"))
    m = st.mean(xs)
    return m, 100 * t975(len(xs) - 1) * st.stdev(xs) / math.sqrt(len(xs)) / m


def paired_log_ratio(xs, ys):
    """Geometric-mean ratio y/x over paired rounds with a 95% t-CI.
    Returns (ratio, lo, hi, half-width %)."""
    d = [math.log(y / x) for x, y in zip(xs, ys)]
    if len(d) < 3:
        r = math.exp(st.mean(d)) if d else float("nan")
        return r, float("nan"), float("nan"), float("nan")
    m, h = st.mean(d), t975(len(d) - 1) * st.stdev(d) / math.sqrt(len(d))
    return math.exp(m), math.exp(m - h), math.exp(m + h), 100 * (math.exp(h) - 1)


def usable(rows, contended_pct):
    return [r for r in rows if r["status"] == "pass"
            and (contended_pct is None or fnum(r, "other_gpu_pct") is None
                 or fnum(r, "other_gpu_pct") <= contended_pct)]


def fnum(row, k):
    try:
        return float(row[k])
    except (KeyError, ValueError, TypeError):
        return None


def worst_ci(rows, arms, windows, ref, contended_pct):
    """Largest 95% CI half-width (%) over all arm means and paired ratios."""
    worst = 0.0
    for wname, _ in windows:
        ev = {}
        for arm in arms:
            ok = usable([r for r in rows if r["label"] == arm["name"] and r["window"] == wname],
                        contended_pct)
            ev[arm["name"]] = {r["run"]: fnum(r, "evens_per_sec") for r in ok}
            worst = max(worst, mean_ci_pct(list(ev[arm["name"]].values()))[1])
        for arm in arms:
            if arm["name"] != ref:
                common = sorted(set(ev[ref]) & set(ev[arm["name"]]))
                worst = max(worst, paired_log_ratio([ev[ref][k] for k in common],
                                                    [ev[arm["name"]][k] for k in common])[3])
    return worst


def summarize(rows, arms, windows, ref, contended_pct=None):
    print()
    for wname, _ in windows:
        print(f"=== window {wname} ===")
        hdr = (f"{'arm':14s} {'n':>3s} {'fail':>4s} {'Gev/s med':>9s} {'95% CI':>15s} {'CV%':>5s}"
               f" {'mean':>7s} {'±CI%':>5s} {'gpu_ms':>7s} {'init_ms':>7s} {'sieve':>6s} {'large':>6s} {'check':>6s}"
               f" {'sync':>5s} {'W':>5s} {'J/Gev':>6s} {'MHz':>5s} {'°C':>4s} {'thr':>3s}")
        print(hdr)
        by_arm = {}
        for arm in arms:
            rs = [r for r in rows if r["label"] == arm["name"] and r["window"] == wname]
            ok = usable(rs, contended_pct)
            by_arm[arm["name"]] = ok
            if not ok:
                print(f"{arm['name']:14s} {len(rs):3d} {len(rs):4d}  all failed: {rs[0]['detail'] if rs else ''}")
                continue
            ev = [fnum(r, "evens_per_sec") / 1e9 for r in ok]
            lo, hi = boot_ci(ev)
            cv = 100 * st.pstdev(ev) / st.mean(ev)
            mean, ci = mean_ci_pct(ev)
            med = lambda k, s=1000.0: st.median([fnum(r, k) * s for r in ok if fnum(r, k) is not None] or [float("nan")])
            print(f"{arm['name']:14s} {len(ok):3d} {len(rs) - len(ok):4d} {st.median(ev):9.3f}"
                  f" {lo:7.3f}-{hi:7.3f} {cv:5.2f} {mean:7.3f} {ci:5.2f} {med('gpu_s'):7.2f} {med('init_s'):7.1f}"
                  f" {med('sieve_s'):6.2f} {med('large_s'):6.2f} {med('check_s'):6.2f}"
                  f" {med('sync_s'):5.2f} {med('power_avg_w', 1):5.0f} {med('j_per_gevens', 1):6.2f}"
                  f" {med('freq_avg_mhz', 1):5.0f} {max(fnum(r, 'temp_max_c') or 0 for r in ok):4.0f}"
                  f" {sum(int(r['throttled']) for r in ok):3d}")
        # paired comparison vs the reference arm (same round => same conditions)
        if ref and len(arms) > 1 and by_arm.get(ref):
            base = {r["run"]: fnum(r, "evens_per_sec") for r in by_arm[ref]}
            for arm in arms:
                if arm["name"] == ref:
                    continue
                pairs = [(base[r["run"]], fnum(r, "evens_per_sec"))
                         for r in by_arm.get(arm["name"], []) if r["run"] in base]
                if pairs:
                    g, lo, hi, h = paired_log_ratio(*zip(*pairs))
                    verdict = ("no resolvable difference" if lo <= 1 <= hi
                               else "faster" if lo > 1 else "slower")
                    print(f"  {arm['name']} vs {ref}: x{g:.4f} ({100 * (g - 1):+.2f}%,"
                          f" 95% CI {lo:.4f}-{hi:.4f} = ±{h:.2f}%, {len(pairs)} paired rounds)"
                          f" -> {verdict}")
        if contended_pct is not None:
            n_bad = sum(1 for r in rows if r["window"] == wname and r["status"] == "pass"
                        and (fnum(r, "other_gpu_pct") or 0) > contended_pct)
            if n_bad:
                print(f"  ({n_bad} run(s) excluded: other processes used > {contended_pct}% of the B580)")
        print()


def upgrade_journal(path):
    """Append columns added since the journal was created (old rows get empty
    values; nothing else changes). Keeps a .bak copy."""
    with path.open(newline="") as f:
        old = list(csv.reader(f))
    hdr = old[0]
    missing = [c for c in SCHEMA + EXTRA if c not in hdr]
    if not missing:
        return
    extra = [c for c in hdr if c not in SCHEMA + EXTRA]
    if extra:
        sys.exit(f"{path}: unknown columns {extra}; refusing to append")
    path.with_suffix(path.suffix + ".bak").write_bytes(path.read_bytes())
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(hdr + missing)
        for r in old[1:]:
            w.writerow(r + [""] * len(missing))
    print(f"journal upgraded: added column(s) {', '.join(missing)} (backup: {path.name}.bak)")


# ------------------------------------------------------------------- main --
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arm", action="append", default=[],
                    help="NAME[:ENV=V,...][:@BIN[@SPV]] (repeatable; default: final)")
    ap.add_argument("--windows", default="bench,steady,end",
                    help="comma list of presets (bench,steady,end) or NAME=START:LIMIT")
    ap.add_argument("--runs", type=int, default=9, help="measured runs per (window, arm)")
    ap.add_argument("--warmup", type=int, default=1, help="discarded runs per (window, arm)")
    ap.add_argument("--segment", type=int, default=0,
                    help="segment span for every arm (default 0: each binary's own "
                         "default; an arm's SEGMENT=N overrides)")
    ap.add_argument("--split", type=int, default=0)
    ap.add_argument("--bin", default=str(L0_ROOT / "build" / "goldbach_l0"))
    ap.add_argument("--spv", default="", help="default --spv for all arms")
    ap.add_argument("--cooldown", type=float, default=0.0, help="seconds to sleep between runs")
    ap.add_argument("--timeout", type=float, default=900.0)
    ap.add_argument("--ref", default=None, help="reference arm for paired ratios (default: first)")
    ap.add_argument("--out", default=str(L0_ROOT / "bench" / "bench-results.csv"))
    ap.add_argument("--force", action="store_true", help="run even if the pre-flight check fails")
    ap.add_argument("--preflight-wait", type=float, default=180.0, metavar="S",
                    help="wait up to S seconds for a running goldbach_l0 to exit before failing pre-flight")
    ap.add_argument("--target-ci", type=float, default=None, metavar="PCT",
                    help="after --runs rounds, keep adding rounds until every arm mean and every "
                         "paired ratio has a 95%% CI half-width <= PCT %% (bounded by --max-runs/--max-minutes)")
    ap.add_argument("--max-runs", type=int, default=60, help="round cap for --target-ci")
    ap.add_argument("--max-minutes", type=float, default=120.0, help="time cap for --target-ci")
    ap.add_argument("--exclude-contended", type=float, default=None, metavar="PCT",
                    help="drop runs during which other processes used > PCT %% of any B580 engine")
    a = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)

    arms = [parse_arm(s, a.bin, a.spv) for s in (a.arm or ["final"])]
    windows = [parse_window(w) for w in a.windows.split(",") if w]
    ref = a.ref or arms[0]["name"]

    # ---- pre-flight --------------------------------------------------------
    problems = []
    # a goldbach_l0 that exits within --preflight-wait is a short CPU-side run
    # (e.g. cli_tests of a CI job on the CPU runner), not a GPU job: wait for
    # it instead of failing the whole A/B. Production slices last minutes.
    deadline = time.monotonic() + a.preflight_wait
    while True:
        running = subprocess.run(["pgrep", "-x", "goldbach_l0"], capture_output=True, text=True).stdout.split()
        if not running or time.monotonic() >= deadline:
            break
        time.sleep(2)
    if running:
        problems.append(f"goldbach_l0 already running (pid {' '.join(running)})")
    disp = display_on_gpu()
    print(f"GPU: {CARD.name if CARD else '?'} render={RENDER}  clock max "
          f"{_read(FREQ / 'max_freq') if FREQ else '?'} MHz  pkg {(hwmon_value('pkg', 'temp') or 0) / 1000:.0f} °C")
    if disp:
        print("WARNING: a monitor is connected to the B580 — the desktop shares the GPU "
              "(~20% slower). Plug the display into the motherboard for clean numbers.")
    users = busy_b580_users()
    if users:
        print("WARNING: other processes are using the B580 right now: " + ", ".join(users))
    if problems:
        print("PRE-FLIGHT FAILED: " + "; ".join(problems))
        if not a.force:
            sys.exit(2)
    commit = git_commit()
    session = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
    arm_commit = {x["name"]: (git_commit(Path(x["bin"]).resolve().parent.parent)
                              or f"ext:{Path(x['bin']).name}") for x in arms}
    print(f"build {commit}  arms: {', '.join(x['name'] for x in arms)}  windows: "
          f"{', '.join(w for w, _ in windows)}  runs {a.runs} (+{a.warmup} warm-up)  -> {a.out}")

    # ---- measure -----------------------------------------------------------
    out = Path(a.out)
    new = not out.exists() or out.stat().st_size == 0
    if not new:
        upgrade_journal(out)
    rows = []
    total_rounds = a.warmup + a.runs
    cap_rounds = a.warmup + max(a.runs, a.max_runs if a.target_ci else a.runs)
    t_start = time.monotonic()
    with out.open("a", newline="") as f:
        fields = SCHEMA + EXTRA
        if not new:
            with out.open(newline="") as g:
                fields = next(csv.reader(g))    # keep the journal's own column order
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        if new:
            w.writeheader()
        rnd = -1
        while True:
            rnd += 1
            if rnd >= total_rounds:
                if not a.target_ci or rnd >= cap_rounds:
                    break
                worst = worst_ci(rows, arms, windows, ref, a.exclude_contended)
                mins = (time.monotonic() - t_start) / 60
                if worst <= a.target_ci:
                    print(f"  target reached: worst CI ±{worst:.3f}% <= ±{a.target_ci}%", file=sys.stderr)
                    break
                if mins >= a.max_minutes:
                    print(f"  time cap: worst CI ±{worst:.3f}% after {mins:.0f} min", file=sys.stderr)
                    break
                print(f"  worst CI ±{worst:.3f}% > ±{a.target_ci}% — adding a round", file=sys.stderr)
            pairs = [(wn, win, arm) for wn, win in windows for arm in arms]
            if rnd % 2:
                pairs.reverse()
            for wn, win, arm in pairs:
                row = run_once(arm, wn, win, a.segment, a.split, a.timeout)
                # The goldbach-l0 HEAD only describes the default binary; another
                # binary gets the commit of the checkout it was built in
                # (<checkout>/build/goldbach_l0), else is recorded as external.
                row.update(commit=commit if arm["bin"] == a.bin else arm_commit[arm["name"]],
                           display_on_gpu=int(disp), session=session)
                if rnd < a.warmup:
                    continue
                row["run"] = rnd - a.warmup + 1
                rows.append(row)
                w.writerow(row)
                f.flush()
                if a.cooldown:
                    time.sleep(a.cooldown)
            print(f"  round {rnd + 1}/{max(total_rounds, rnd + 1)}"
                  f"{' (warm-up)' if rnd < a.warmup else ''}"
                  f"  other-GPU {max((fnum(r, 'other_gpu_pct') or 0) for r in rows[-len(pairs):]) if rnd >= a.warmup else 0:.1f}%",
                  file=sys.stderr, flush=True)

    summarize(rows, arms, windows, ref, a.exclude_contended)
    fails = sum(r["status"] != "pass" for r in rows)
    bad_left = [r for r in rows if r["status"] == "pass" and r["leftovers"] not in ("0", "")
                and r["start"] > 10**6]
    if fails:
        print(f"{fails} FAILED run(s) — see the detail column in {a.out}")
    if bad_left:
        print(f"WARNING: {len(bad_left)} run(s) had leftovers > 0 at high range")
    print(f"plot: tools/.venv/bin/python tools/waterfall.py {a.out} -o bench-waterfall.png")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
