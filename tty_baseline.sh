#!/usr/bin/env bash
# tty_baseline.sh — ONE command: stop the desktop -> run the bench.py baseline
# with the B580 to itself -> restore the desktop. For best-quality baseline
# numbers (no compositor, no browser, no other client on the card).
#
# Same two-phase shape as tty_auto.sh: the launcher hands phase 2 to systemd
# as a transient SYSTEM service (systemd-run, root). It does NOT re-exec with
# setsid/nohup: a setsid'd child stays in the desktop session's cgroup, and on
# 2026-09-24 the user@1000.service teardown SIGKILLed exactly such a child
# mid-run the moment the desktop stopped — no benchmark, no restore, manual
# recovery needed. A system service lives in system.slice (immune to the
# session teardown) and carries IgnoreOnIsolate=yes (immune to BOTH isolates:
# multi-user when the desktop goes down, graphical at restore). It runs the
# benchmark as your user, and ALWAYS restores graphical.target (EXIT trap + a
# hard time ceiling), so a hang cannot strand you without a desktop.
#
# Usage (from anywhere; the tidy tree is found next to this repo):
#   sudo bash tools/tty_baseline.sh                # current build, full baseline (~25 min)
#   sudo bash tools/tty_baseline.sh quick          # 5 short + 10 long runs (~8 min)
#   sudo BASELINE_ARMS="v1.10.0:@/path/goldbach_l0@/path/goldbach_bmg.spv" \
#        bash tools/tty_baseline.sh                # add reference build(s), interleaved
#
# What runs (bench.py, fresh process per run, rounds interleaved across arms):
#   1. windows bench,steady,end   x RUNS_SHORT (12; quick: 5)
#   2. windows long,long-end      x RUNS_LONG  (40; quick: 10)
# The current build's arm is labeled with its version (include/config.hpp).
# Rows are appended to <tidy>/bench/bench-results.csv (bench.py's journal). The
# tables bench.py prints land in <tidy>/tty-baseline-last.txt — read that when
# you are back; the full log is <tidy>/tty-baseline.log.
#
# Knobs (env): RUNS_SHORT, RUNS_LONG, WINDOWS_SHORT, WINDOWS_LONG,
#   BASELINE_ARMS (extra bench.py --arm specs, space-separated), BASELINE_OUT
#   (journal path), BASELINE_CEIL (seconds, default 7200: the desktop is back
#   by then no matter what), TTY_BASELINE_DRY=1 (keep the desktop up, run the
#   benchmark as the calling user — to test the script).
#
# Desktop apps WILL be closed when the desktop stops — save your work first.
# When it returns it is a RESTARTED session: expect the login screen.
# Manual recovery from a VT (ctrl-alt-f3):  sudo systemctl isolate graphical.target
set -u

DRY=${TTY_BASELINE_DRY:-0}
SELF=$(readlink -f "$0")
REPO=${GB_REPO:-$(dirname "$SELF")/../goldbach-l0-tidy}
[ -x "$REPO/build/goldbach_l0" ] || REPO=$PWD
[ -x "$REPO/build/goldbach_l0" ] || { echo "FATAL: build/goldbach_l0 not found (run from the tidy root or set GB_REPO)" >&2; exit 1; }
REPO=$(readlink -f "$REPO")
cd "$REPO" || exit 1
LOG=$REPO/tty-baseline.log
SUMMARY=$REPO/tty-baseline-last.txt
CEIL=${BASELINE_CEIL:-7200}
RUN_USER=${SUDO_USER:-$(id -un)}
if [ "$(id -u)" -eq 0 ]; then LOCK=/run/tty-baseline.lock; else LOCK=/tmp/tty-baseline.lock; fi

PRESET=${1:-full}
case "$PRESET" in
    full)  RS=${RUNS_SHORT:-12}; RL=${RUNS_LONG:-40} ;;
    quick) RS=${RUNS_SHORT:-5};  RL=${RUNS_LONG:-10} ;;
    *) echo "usage: sudo bash tools/tty_baseline.sh [full|quick]" >&2; exit 2 ;;
esac
WS=${WINDOWS_SHORT:-bench,steady,end}
WL=${WINDOWS_LONG:-long,long-end}
OUT=${BASELINE_OUT:-$REPO/bench/bench-results.csv}

[ "$(id -u)" -eq 0 ] || [ "$DRY" = 1 ] || { echo "FATAL: run with sudo (needs systemctl + runuser)" >&2; exit 1; }
[ "$RUN_USER" != root ] || [ "$DRY" = 1 ] || { echo "FATAL: run via sudo from your user account (SUDO_USER is empty)" >&2; exit 1; }
[ -f tools/bench.py ] || { echo "FATAL: tools/bench.py not found" >&2; exit 1; }
if pgrep -x goldbach_l0 >/dev/null 2>&1; then
    echo "FATAL: a goldbach_l0 run is active ($(pgrep -x goldbach_l0 | tr '\n' ' '))- finish it first; keeping the desktop up." >&2
    exit 1
fi

# ---------- phase 1: foreground launcher (what your terminal runs) ----------
if [ "${TTY_BASELINE_DETACHED:-0}" != "1" ]; then
    UNIT=tty-baseline.service
    VER=$(sed -n 's/.*VERSION = "\(.*\)";/\1/p' include/config.hpp)
    echo "tty_baseline: v$VER, preset $PRESET: $WS x $RS runs, then $WL x $RL runs"
    [ -n "${BASELINE_ARMS:-}" ] && echo "tty_baseline: reference arms: $BASELINE_ARMS"
    if [ "$DRY" = 1 ]; then
        echo "tty_baseline: DRY RUN — the desktop stays up; running in the foreground."
        TTY_BASELINE_DETACHED=1 exec bash "$SELF" "$@"
    fi
    systemctl is-active --quiet "$UNIT" 2>/dev/null && \
        { echo "FATAL: $UNIT is already running (systemctl status $UNIT)" >&2; exit 1; }
    echo "tty_baseline: desktop shuts down in 10 s — SAVE YOUR WORK NOW (apps will close)."
    echo "tty_baseline: it comes back by itself when the benchmark ends (ceiling $((CEIL/60)) min)."
    echo "tty_baseline: results: $SUMMARY   log: $LOG"
    echo "tty_baseline: expect the login screen when the desktop returns."
    systemctl reset-failed "$UNIT" 2>/dev/null
    SR=(systemd-run --collect --unit="$UNIT" --description="tty_baseline headless benchmark"
        --property=IgnoreOnIsolate=yes --property=TimeoutStartSec=infinity
        --property=SuccessExitStatus="0 124"
        --setenv=TTY_BASELINE_DETACHED=1
        --setenv=GB_REPO="$REPO"
        --setenv=SUDO_USER="$RUN_USER")
    for k in RUNS_SHORT RUNS_LONG WINDOWS_SHORT WINDOWS_LONG BASELINE_ARMS BASELINE_OUT BASELINE_CEIL; do
        [ -n "${!k:-}" ] && SR+=(--setenv="$k=${!k}")
    done
    SR+=(-- /usr/bin/bash "$SELF" "$@")
    if ! "${SR[@]}"; then
        echo "FATAL: systemd-run failed — nothing started, desktop untouched." >&2
        exit 1
    fi
    exit 0
fi

# ---------- phase 2: systemd service as root — off, bench, on (always) ----------
if [ "$DRY" != 1 ]; then
    exec >>"$LOG" 2>&1   # the service has no terminal — everything below goes to the log
fi
say() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*"; }
compositor_up() {
    pgrep -x Xorg >/dev/null 2>&1 || pgrep -x Xwayland >/dev/null 2>&1 \
        || pgrep -x kwin_wayland >/dev/null 2>&1 || pgrep -x plasmashell >/dev/null 2>&1 \
        || pgrep -x gnome-shell >/dev/null 2>&1
}

RESTORED=0
restore() {
    [ "$RESTORED" = 0 ] || return 0
    RESTORED=1
    [ "$DRY" = 1 ] && return 0
    say "restoring desktop (graphical.target)..."
    systemctl isolate graphical.target
    for _ in $(seq 1 60); do systemctl is-active --quiet graphical.target && break; sleep 1; done
    if ! systemctl is-active --quiet graphical.target; then
        say "WARNING: graphical.target not active — manual recovery: sudo systemctl isolate graphical.target"
        return 0
    fi
    # graphical.target active only means the login manager started; wait for
    # a real desktop process before declaring success (tty_auto.sh lesson).
    for _ in $(seq 1 120); do compositor_up && break; sleep 1; done
    compositor_up && say "desktop session up — log in again." \
        || say "graphical.target active, no desktop process after 120 s — if the screen stays dead: sudo systemctl isolate graphical.target"
}
trap restore EXIT INT TERM

exec 200>"$LOCK" || exit 1
flock -n 200 || { say "FATAL: lock held by another instance; exiting (desktop untouched)."; exit 1; }

VER=$(sed -n 's/.*VERSION = "\(.*\)";/\1/p' include/config.hpp)
ARMS=(--arm "v$VER")
for a in ${BASELINE_ARMS:-}; do ARMS+=(--arm "$a"); done

say "=== tty_baseline start — v$VER preset $PRESET tree=$REPO user=$RUN_USER ==="
if [ "$DRY" != 1 ]; then
    sleep 10
    say "stopping desktop (multi-user.target)..."
    systemctl isolate multi-user.target
    for _ in $(seq 1 30); do compositor_up || break; sleep 1; done
    compositor_up && say "WARNING: desktop still running after 30 s — results may include desktop load." \
                  || say "desktop is down."
    sleep 15   # let the card settle after the compositor released it
fi

as_user() {
    if [ "$DRY" = 1 ]; then "$@"
    else runuser -u "$RUN_USER" -- env -u DISPLAY -u WAYLAND_DISPLAY \
             HOME="$(getent passwd "$RUN_USER" | cut -d: -f6)" USER="$RUN_USER" LOGNAME="$RUN_USER" "$@"
    fi
}

{
    echo "tty_baseline v$VER preset $PRESET — $(date -Is)"
    echo "journal: $OUT"
    echo
} > "$SUMMARY"
[ "$DRY" = 1 ] || chown "$RUN_USER" "$SUMMARY" 2>/dev/null

T0=$(date +%s)
rc=0
for pass in short long; do
    if [ "$pass" = short ]; then W=$WS; R=$RS; else W=$WL; R=$RL; fi
    left=$(( CEIL - ($(date +%s) - T0) ))
    [ "$left" -gt 60 ] || { say "ceiling reached — skipping the $pass pass"; rc=124; break; }
    say "bench.py: windows $W, $R runs (+1 warm-up) per arm..."
    as_user timeout "$left" python3 tools/bench.py --windows "$W" --runs "$R" --warmup 1 \
        "${ARMS[@]}" --out "$OUT" 2>&1 | tee -a "$SUMMARY.tmp"
    r=${PIPESTATUS[0]}
    say "bench.py ($pass) finished rc=$r"
    [ "$r" = 0 ] || rc=$r
done
# keep the tables, drop the per-round progress lines
grep -vE '^  round ' "$SUMMARY.tmp" >> "$SUMMARY" 2>/dev/null
rm -f "$SUMMARY.tmp"
echo "rc=$rc (0 = ok, 124 = ceiling hit)" >> "$SUMMARY"

[ "$DRY" = 1 ] || sleep 20   # GPU settle before the desktop session restarts
restore
say "=== tty_baseline done rc=$rc — summary: $SUMMARY ==="
exit "$rc"
