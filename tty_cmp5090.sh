#!/usr/bin/env bash
# tty_cmp5090.sh — same-range benchmark suite vs isaac-6/goldbach-gpu (RTX 5090).
#
# Verifies the SAME ranges Isaac's DEVELOPMENT-HISTORY documents, so the
# comparison is apples-to-apples: evens in [4, 10^k], full process wall
# clock, production config (segment 250M, split auto). His reference times:
#   1e9-1e12 : flagship `goldbach.cu` table rows  (PRE-v3.0.0 vintage)
#   1e14     : 933 s                             (v3.0.0 current)
# Run it from a bare TTY (ctrl-alt-f3, desktop stopped) for the headless
# fast band; the script auto-labels every journal row headless-tty/desktop,
# so it can also run on the desktop without mislabeling.
#
# Bash + coreutils only (no python in the console):
#   - canary: `--info` with a 10 s ceiling before anything else
#   - refuses to start while another goldbach_l0 owns the GPU
#   - fresh process per run, per-run timeout (hang -> TIMEOUT, not a stall)
#   - appends one row per run to the journal; medians + CV + ratio summary
#
# Usage (from the goldbach-l0 root, as your normal user):
#   bash tools/tty_cmp5090.sh                 # full suite: 1e9..1e12, 1e14
#   bash tools/tty_cmp5090.sh 12 14           # subset by exponent
#   bash tools/tty_cmp5090.sh 9 10 11 12      # his table rows only (fast)
#   GB_REPO=/path/to/tree bash ...            # non-default verifier tree
#   GB_JOURNAL=file.csv bash ...              # non-default journal path
# Journal: compare-5090-same-range.csv (append-only; created if missing)
set -u

REPO=${GB_REPO:-$(dirname "$(readlink -f "$0")")/../goldbach-l0}
[ -x "$REPO/build/goldbach_l0" ] || REPO=$PWD
[ -x "$REPO/build/goldbach_l0" ] || { echo "FATAL: build/goldbach_l0 not found (run from the goldbach-l0 root, or set GB_REPO)" >&2; exit 1; }
cd "$REPO" || exit 1

BIN=./build/goldbach_l0
SPV=kernels/goldbach_bmg.spv
JOURNAL=${GB_JOURNAL:-compare-5090-same-range.csv}

die() { echo "FATAL: $*" >&2; exit 1; }
[ -f "$SPV" ] || die "$SPV not found"
command -v timeout >/dev/null || die "coreutils 'timeout' missing"
command -v awk     >/dev/null || die "awk missing"
[ "$(id -u)" -eq 0 ] && echo "WARNING: running as root; journal will be root-owned" >&2

if [ "${MOCK:-0}" != "1" ] && pgrep -x goldbach_l0 >/dev/null 2>&1; then
    die "another goldbach_l0 is running ($(pgrep -x goldbach_l0 | tr '\n' ' '))- sharing the GPU would poison both runs. Wait for it or stop it first."
fi

# Desktop detection must be Wayland-aware: this machine's KDE runs
# kwin_wayland (plasmashell with NO Xorg process). DISPLAY is deliberately
# NOT trusted — it leaks through sudo/runuser into headless runs and
# mislabels them (seen 2026-09-21: truly-headless rows labeled desktop).
ENV_LABEL=headless-tty
if pgrep -x Xorg >/dev/null 2>&1 || pgrep -x kwin_wayland >/dev/null 2>&1 \
   || pgrep -x plasmashell >/dev/null 2>&1 || pgrep -x gnome-shell >/dev/null 2>&1; then
    ENV_LABEL=desktop
    echo "WARNING: desktop session active -> expect the DESKTOP band (~x0.8 of headless)."
    echo "         For the fast band use tools/tty_auto.sh (or a bare TTY)."
fi

# exp -> limit runs timeout isaac_wall_s
cfg() {
    case "$1" in
        9)  LIM=1000000000       RUNS=5 TO=120   ISAAC=0.141018 ;;
        10) LIM=10000000000      RUNS=5 TO=120   ISAAC=0.395769 ;;
        11) LIM=100000000000     RUNS=5 TO=300   ISAAC=3.3115   ;;
        12) LIM=1000000000000    RUNS=3 TO=600   ISAAC=37.440   ;;
        14) LIM=100000000000000  RUNS=1 TO=21600 ISAAC=933.0    ;;
        *)  die "no config for exponent $1 (supported: 9 10 11 12 14)" ;;
    esac
}

EXPS=${*:-"9 10 11 12 14"}
for e in $EXPS; do case "$e" in 9|10|11|12|14) ;; *) die "bad exponent '$e' (supported: 9 10 11 12 14)";; esac; done

[ -f "$JOURNAL" ] || echo 'timestamp,limit,run,status,wall_s,gpu_s,total_s,sieve_s,large_s,check_s,sync_s,evens_per_sec,leftovers,isaac_wall_s,env,detail' > "$JOURNAL"
touch "$JOURNAL" || die "cannot write $JOURNAL"

echo "tty_cmp5090: exponents [$EXPS]  env=$ENV_LABEL"
echo "tree:        $PWD   ($(git log -1 --oneline 2>/dev/null || echo 'not a git tree'))"
echo "journal:     $(readlink -f "$JOURNAL")"

if [ "${MOCK:-0}" != "1" ]; then
    echo "== canary: driver init (--info, 10 s ceiling) =="
    if ! timeout 10 env -u LD_LIBRARY_PATH "$BIN" --info >/dev/null 2>&1; then
        echo "  --info FAILED or HUNG. Level Zero did not initialize."
        echo "  Wait 30 s and re-run; if it fails again restore the desktop"
        echo "  (sudo systemctl isolate graphical.target) and report it."
        exit 2
    fi
    echo "  OK"
fi

kv() { printf '%s\n' "$OUT" | tr ' ' '\n' | awk -F= -v k="$1" '$1==k{print $2; exit}'; }

for e in $EXPS; do
    cfg "$e"
    echo
    echo "== 1e$e: $RUNS run(s), range [4, $LIM], timeout ${TO}s  (isaac: ${ISAAC}s) =="
    walls=/tmp/tty_cmp5090_walls.$$; rates=/tmp/tty_cmp5090_rates.$$
    : > "$walls"; : > "$rates"
    n=0; fails=0
    while [ "$n" -lt "$RUNS" ]; do
        n=$((n + 1))
        ts=$(date -u +%Y-%m-%dT%H:%M:%S%z)
        if [ "${MOCK:-0}" = "1" ]; then
            OUT="version=1.6.2 limit=$LIM start=4 segment=250000000 split=0 segments=4 leftovers=1 result=PASS init_s=0.02 init_module_s=0.02 gpu_s=0.03 total_s=0.05 sieve_s=0.01 large_s=0.01 check_s=0.01 sync_s=0.01 evens_per_sec=1.0e+10 tiles=1 large_items=1"
            wall=$(awk -v n=$n 'BEGIN{printf "%.3f", 0.100 + n*0.01}')
            rc=0
        else
            s_ns=$(date +%s%N)
            OUT=$(timeout "$TO" env -u LD_LIBRARY_PATH "$BIN" --start 4 --limit "$LIM" \
                    --segment 250000000 --split 0 --spv "$SPV" \
                    --prime-cache base_primes.cache --csv 2>&1)
            rc=$?
            e_ns=$(date +%s%N)
            wall=$(awk -v e="$e_ns" -v s="$s_ns" 'BEGIN{printf "%.3f", (e-s)/1e9}')
        fi
        if [ "$rc" -eq 124 ]; then status=timeout
        elif printf '%s\n' "$OUT" | grep -q 'result=PASS'; then status=pass
        else status="fail rc=$rc"; fi
        g=$(kv gpu_s); t=$(kv total_s); sv=$(kv sieve_s); lg=$(kv large_s)
        ck=$(kv check_s); sy=$(kv sync_s); ev=$(kv evens_per_sec); lo=$(kv leftovers)
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$ts" "$LIM" "$n" "$status" "$wall" "${g:-}" "${t:-}" "${sv:-}" "${lg:-}" \
            "${ck:-}" "${sy:-}" "${ev:-}" "${lo:-}" "$ISAAC" "$ENV_LABEL" \
            "same-range vs isaac-6/goldbach-gpu; his 1e9-1e12=pre-v3 rows; 1e14=v3.0.0" >> "$JOURNAL"
        echo "  run $n/$RUNS  $status  wall=${wall}s  gpu_s=${g:--}  large_s=${lg:--}  rate=${ev:--}"
        if [ "$status" = "pass" ]; then echo "$wall" >> "$walls"; echo "${ev:-0}" >> "$rates"; else fails=$((fails + 1)); fi
        if [ "$n" -eq 1 ] && [ "$status" != "pass" ]; then
            echo "First run of 1e$e did not PASS; aborting suite. Raw output tail:"
            printf '%s\n' "$OUT" | tail -5
            rm -f "$walls" "$rates"
            exit 3
        fi
    done
    med=$(sort -g "$walls" | awk '{a[NR]=$1} END{if(NR%2) printf "%.3f", a[(NR+1)/2]; else printf "%.3f", (a[NR/2]+a[NR/2+1])/2}')
    cv=$(awk '{s+=$1; ss+=$1*$1} END{m=s/NR; v=(NR>1)?sqrt(ss/NR-m*m)/m*100:0; printf "%.1f", v}' "$walls")
    rmed=$(sort -g "$rates" | awk '{a[NR]=$1} END{if(NR%2) printf "%.2f", a[(NR+1)/2]/1e9; else printf "%.2f", (a[NR/2]+a[NR/2+1])/2e9}')
    ratio=$(awk -v i="$ISAAC" -v m="$med" 'BEGIN{printf "%.2f", i/m}')
    who=$(awk -v r="$ratio" 'BEGIN{print (r>1)?"KEN faster":"isaac faster"}')
    echo "  -> 1e$e: wall median ${med}s  CV ${cv}%  rate median ${rmed} G/s  | isaac ${ISAAC}s  isaac/ken=${ratio}x ($who)  fails=$fails"
    rm -f "$walls" "$rates"
done

echo
echo "Done. Journal: $(readlink -f "$JOURNAL")"
if [ "$ENV_LABEL" = headless-tty ]; then
    echo "Now restore the desktop:  sudo systemctl isolate graphical.target"
fi
exit 0
