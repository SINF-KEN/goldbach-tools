#!/usr/bin/env bash
# tty_bench.sh — TTY-friendly production benchmark loop for goldbach-l0.
#
# Why: the headless/TTY fast-vs-slow mode test runs from a bare console
# (no python conveniences, no second chance if a run hangs — the first
# sweep.py attempt hung 300 s there). This script is bash + coreutils only:
#   - canary 1: `--info` with a 10 s ceiling  -> does L0 init headless?
#   - then N production runs, each with a 120 s ceiling, so a hang is
#     reported as HUNG instead of stalling the console.
# Every run is a fresh process, appended to the journal as ONE line:
#   <iso-ts> run=<n> <pass|FAIL|HUNG> <full --csv key=value line>
#
# Run as your NORMAL user (not sudo), from anywhere:
#   bash tools/tty_bench.sh [runs=12] [journal=sweep-tty.csv]
set -u
cd "$(dirname "$0")/.." || exit 1

RUNS=${1:-12}
OUT=${2:-sweep-tty.csv}
BIN=./build/goldbach_l0
SPV=kernels/goldbach_bmg.spv
START=4000000000000000000
LIMIT=4000000002000000000
SEG=250000000
SPLIT=0
MODE_CUT=0.085   # large_s below this (8-launch sum) = FAST run

die() { echo "FATAL: $*" >&2; exit 1; }
[ -x "$BIN" ]   || die "$BIN not found (wrong tree? run bash build.sh first)"
[ -f "$SPV" ]   || die "$SPV not found"
command -v timeout >/dev/null || die "coreutils 'timeout' missing"
command -v awk     >/dev/null || die "awk missing"
[ "$(id -u)" -eq 0 ] && echo "WARNING: running as root; journal will be root-owned" >&2

touch "$OUT" || die "cannot write $OUT"
SKIP=$(( $(wc -l < "$OUT") + 1 ))   # summary covers only THIS invocation
echo "tty_bench: $RUNS runs, window [$START, $LIMIT] seg $SEG split $SPLIT"
echo "journal:   $(readlink -f "$OUT")"

echo "== canary: driver init (--info, 10 s ceiling) =="
if ! timeout 10 "$BIN" --info >/dev/null 2>&1; then
    echo "  --info FAILED or HUNG."
    echo "  => Level Zero does not initialize in this console. Wait 30 s and"
    echo "     re-run this script; if it fails again, restore the desktop"
    echo "     (sudo systemctl isolate graphical.target) and report it."
    echo "     That result by itself is a finding: the TTY test cannot run."
    exit 2
fi
echo "  OK"

n=0
while [ "$n" -lt "$RUNS" ]; do
    n=$((n + 1))
    ts=$(date -u +%Y-%m-%dT%H:%M:%S%z)
    out=$(timeout 120 "$BIN" --start "$START" --limit "$LIMIT" \
            --segment "$SEG" --split "$SPLIT" --spv "$SPV" \
            --prime-cache base_primes.cache --csv 2>&1)
    rc=$?
    if [ "$rc" -eq 124 ]; then
        status=HUNG
    elif printf '%s\n' "$out" | grep -q 'result=PASS'; then
        status=pass
    else
        status=FAIL
    fi
    printf '%s run=%s %s %s\n' "$ts" "$n" "$status" "$out" >> "$OUT"
    gpu=$(printf   '%s\n' "$out" | grep -o 'gpu_s=[0-9.]*'        | head -1)
    large=$(printf '%s\n' "$out" | grep -o 'large_s=[0-9.]*'      | head -1)
    ev=$(printf    '%s\n' "$out" | grep -o 'evens_per_sec=[0-9.e+]*' | head -1)
    echo "run $n/$RUNS $status  ${gpu:--}  ${large:--}  ${ev:--}"
    if [ "$n" -eq 1 ] && [ "$status" != "pass" ]; then
        echo "First production run did not PASS (rc=$rc). Raw output tail:"
        printf '%s\n' "$out" | tail -5
        echo "Journal kept: $PWD/$OUT — restore the desktop and report."
        exit 3
    fi
done

echo
echo "== summary (this invocation) =="
tail -n "+$SKIP" "$OUT" | awk -v cut="$MODE_CUT" '
{
    mode="??"; gpu="-"; large="-";
    for (i = 1; i <= NF; i++) {
        if (index($i, "run=")      == 1) rn    = substr($i, 5);
        if (index($i, "gpu_s=")    == 1) gpu   = substr($i, 7);
        if (index($i, "large_s=")  == 1) large = substr($i, 9);
    }
    if      ($3 == "pass") mode = (large + 0 < cut) ? "FAST" : "SLOW";
    else                   mode = $3;
    printf "  run %-3s %-6s gpu %-9s large %-9s %s\n", rn, $3, gpu, large, mode;
    if (mode == "FAST") f++; else if (mode == "SLOW") s++;
    else other++;
}
END { printf "  => FAST %d   SLOW %d   other %d\n", f + 0, s + 0, other + 0 }'
echo
echo "Done. Now restore the desktop:  sudo systemctl isolate graphical.target"
