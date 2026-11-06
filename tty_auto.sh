#!/usr/bin/env bash
# tty_auto.sh — ONE command: stop desktop -> run headless same-range suite
# (tools/tty_cmp5090.sh) with the GPU to itself -> restore desktop.
#
# Why the two-phase shape: the command is launched from a desktop terminal,
# and stopping the desktop tears down that session's cgroup — a setsid'd
# child does NOT survive it (2026-09-24, tty_baseline.sh: user@1000.service
# teardown SIGKILLed the detached script mid-run; no bench, no restore, the
# machine had to be rebooted). So phase 1 hands phase 2 to systemd as a
# transient SYSTEM service (systemd-run, root): it lives in system.slice,
# immune to the session teardown, and carries IgnoreOnIsolate=yes so BOTH
# isolates (multi-user on the way down, graphical at restore) leave it
# running. It does the work and ALWAYS restores graphical.target (EXIT trap
# + a hard time ceiling) so a hang cannot strand you.
#
# Usage (from the tidy root, ONE command):
#   sudo bash tools/tty_auto.sh              # full suite 1e9..1e12 + 1e14 (~70 min)
#   sudo bash tools/tty_auto.sh 9 10 11 12   # his table rows only (~3 min)
#   sudo bash tools/tty_auto.sh 12 14        # headline rows only (~70 min)
#
# Desktop apps WILL be closed when X stops — save your work first.
# Log: <tidy>/tty-auto.log   Journal: compare-5090-same-range.csv
# Worst-case desktop-off window: AUTO_CEIL (default 6.5 h) — a hung suite is
# killed there and the desktop restored. Manual recovery from a VT (ctrl-alt-f3):
#   sudo systemctl isolate graphical.target
set -u

DRY=${TTY_AUTO_DRY:-0}
DMARK=""; DRUN=""
[ "$DRY" = 1 ] && { DMARK=" (DRY)"; DRUN=" (DRY RUN)"; }
SELF=$(readlink -f "$0")
REPO=${GB_REPO:-$(dirname "$SELF")/../goldbach-l0-tidy}
[ -x "$REPO/build/goldbach_l0" ] || REPO=$PWD
[ -x "$REPO/build/goldbach_l0" ] || { echo "FATAL: build/goldbach_l0 not found (run from the tidy root or set GB_REPO)" >&2; exit 1; }
cd "$REPO" || exit 1
LOG=$REPO/tty-auto.log
CEIL=${AUTO_CEIL:-23400}
if [ "$(id -u)" -eq 0 ]; then LOCK=/run/tty-auto-cmp5090.lock
else LOCK=/tmp/tty-auto-cmp5090.lock; fi

[ "$(id -u)" -eq 0 ] || [ "$DRY" = 1 ] || { echo "FATAL: run with sudo (needs systemctl + runuser)" >&2; exit 1; }
[ -f tools/tty_cmp5090.sh ] || { echo "FATAL: tools/tty_cmp5090.sh not found" >&2; exit 1; }
if [ "$DRY" != 1 ] && pgrep -x goldbach_l0 >/dev/null 2>&1; then
    echo "FATAL: a goldbach_l0 run is active ($(pgrep -x goldbach_l0 | tr '\n' ' '))- finish it first; keeping the desktop up." >&2
    exit 1
fi

# ---------- phase 1: foreground launcher (what your terminal runs) ----------
if [ "${TTY_AUTO_DETACHED:-0}" != "1" ]; then
    UNIT=tty-auto.service
    systemctl is-active --quiet "$UNIT" 2>/dev/null && \
        { echo "FATAL: $UNIT is already running (systemctl status $UNIT)" >&2; exit 1; }
    echo "tty_auto: desktop shuts down in 5 s — SAVE YOUR WORK NOW (apps will close)."
    echo "tty_auto: suite exponents [${*:-9 10 11 12 14}], ceiling $((CEIL/60)) min${DRUN}"
    echo "tty_auto: log: $LOG"
    echo "tty_auto: the desktop restores itself when the suite ends. You can close this terminal."
    echo "tty_auto: NOTE — when the desktop returns it is a RESTARTED session: expect the"
    echo "tty_auto: login screen and log in again; the screen may need ~1 min to wake fully."
    systemctl reset-failed "$UNIT" 2>/dev/null
    SR=(systemd-run --collect --unit="$UNIT" --description="tty_auto headless suite"
        --property=IgnoreOnIsolate=yes --property=TimeoutStartSec=infinity
        --property=SuccessExitStatus="0 3 124"
        --setenv=TTY_AUTO_DETACHED=1
        --setenv=GB_REPO="$REPO")
    [ "$DRY" = 1 ] && SR+=(--setenv=TTY_AUTO_DRY=1)
    [ -n "${AUTO_CEIL:-}" ] && SR+=(--setenv=AUTO_CEIL="$CEIL")
    SR+=(-- /usr/bin/bash "$SELF" "$@")
    if ! "${SR[@]}"; then
        echo "FATAL: systemd-run failed — nothing started, desktop untouched." >&2
        exit 1
    fi
    exit 0
fi

# ---------- phase 2: systemd service as root — off, bench, on (always) ----------
if [ "${TTY_AUTO_DETACHED:-0}" = 1 ]; then
    exec >>"$LOG" 2>&1   # the service has no terminal — everything below goes to the log
fi
say() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*"; }
compositor_up() {
    pgrep -x Xorg >/dev/null 2>&1 || pgrep -x kwin_wayland >/dev/null 2>&1 \
        || pgrep -x plasmashell >/dev/null 2>&1 || pgrep -x gnome-shell >/dev/null 2>&1
}
systemctl_() {
    if [ "$DRY" = 1 ]; then say "DRY: systemctl $*"; return 0; fi
    systemctl "$@"
}
x_down() { [ "$DRY" = 1 ] || ! compositor_up; }

RESTORED=0
restore() {
    [ "$RESTORED" = 0 ] || return 0
    RESTORED=1
    say "restoring desktop (graphical.target)..."
    systemctl_ isolate graphical.target
    for _ in $(seq 1 60); do
        if [ "$DRY" = 1 ] || systemctl is-active --quiet graphical.target; then break; fi
        sleep 1
    done
    if [ "$DRY" != 1 ] && ! systemctl is-active --quiet graphical.target; then
        say "WARNING: graphical.target not active — manual recovery: sudo systemctl isolate graphical.target"
        return 0
    fi
    # target active only means sddm started; the session itself needs up to a
    # minute (2026-09-21: declaring success at 5 s = user staring at a frozen
    # screen with a live cursor). Wait for an actual desktop process.
    for _ in $(seq 1 120); do
        if [ "$DRY" = 1 ] || compositor_up; then break; fi
        sleep 1
    done
    if [ "$DRY" = 1 ] || compositor_up; then
        say "desktop session up (compositor running) — you may need to log in again."
    else
        say "graphical.target active but no desktop process after 120 s — screen may look frozen; if it stays dead: sudo systemctl isolate graphical.target"
    fi
}
trap restore EXIT INT TERM

exec 200>"$LOCK" || exit 1
flock -n 200 || { say "FATAL: lock held by another instance; exiting (desktop untouched)."; exit 1; }

say "=== tty_auto start${DMARK} — exponents [${*:-9 10 11 12 14}] tree=$REPO ==="
sleep 5
say "stopping desktop (multi-user.target)..."
systemctl_ isolate multi-user.target
for _ in $(seq 1 30); do x_down && break; sleep 1; done
x_down && say "desktop compositor is down." || say "WARNING: desktop compositor still alive after 30 s — bench rows will be labeled desktop."

say "running suite as ken (tty_cmp5090.sh)..."
if [ "$DRY" = 1 ]; then
    say "DRY: would run: timeout $CEIL runuser -u ken -- env -u DISPLAY -u WAYLAND_DISPLAY HOME=/home/ken bash tools/tty_cmp5090.sh $*"
    rc=0
else
    timeout "$CEIL" runuser -u ken -- env -u DISPLAY -u WAYLAND_DISPLAY HOME=/home/ken USER=ken LOGNAME=ken \
        bash tools/tty_cmp5090.sh "$@"
    rc=$?
fi
say "suite finished rc=$rc (0=ok, 3=first-run fail, 124=ceiling hit)"
sleep 20   # GPU settle before the desktop session restarts (skill rule: 15-30 s)
restore
say "=== tty_auto done rc=$rc — results: compare-5090-same-range.csv; full log: $LOG ==="
exit "$rc"
