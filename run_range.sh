#!/usr/bin/env bash
# run_range.sh — resumable production run of goldbach-l0 over [START, LIMIT].
#
# The range is cut into slices of SLICE integers; each slice is ONE fresh
# goldbach_l0 process, and every PASSED slice is appended to the journal as
#   PASS <slice_start> <slice_limit> <iso-ts> <full --csv key=value line> release=<id> [attempts=2]
# Re-running the same command resumes after the last contiguous PASS: the
# journal is checked as an unbroken chain starting at START, so a slice can
# never be skipped. A failed slice is retried once (transient GPU/driver
# errors); a second failure STOPS the run with the evidence in the log —
# a genuine Goldbach counterexample or a real fault must be looked at, never
# skipped.
#
# A slice that fails and then passes is NOT a silent PASS: the first failure
# is journaled as a RETRY row (with the GPU state at that moment), the PASS
# row carries attempts=2, the slice is queued in <journal>.recheck (re-run it
# later with `ci/goldbach_ci.py recheck`), and the notification hook fires.
# GB_RANGE_MAX_RETRIES RETRY rows among the last 100 journal rows (~50 slices
# or more) stop the run as flaky hardware.
#
# Usage (from the goldbach-l0-tidy root; plain bash, works from a TTY):
#   bash tools/run_range.sh START LIMIT [SLICE] [JOURNAL]
#   bash tools/run_range.sh 4000000000000000000 5000000000000000000
# defaults: SLICE=200000000000000 (2e14, ~25 min at ~65 G evens/s; production
#           passes 5e13, ~6.5 min, from ci-state/production.json),
#           JOURNAL=range-<START>-<LIMIT>.journal
# Stop any time with Ctrl-C; the interrupted slice simply re-runs. To stop
# cleanly between two slices: touch <journal-without-.journal>.stop (exit 3).
#
# Binary: GB_RANGE_BIN if set, else the local CI's active release
# (ci-state/active/goldbach_l0, with that release's kernels), else
# ./build/goldbach_l0. The path is resolved ONCE at start, so a CI promotion
# never swaps the binary of a running campaign; the release id is written to
# every PASS row.
#
# Only one GPU campaign at a time: the script holds an flock on
# GB_RANGE_LOCK (default .goldbach-gpu.lock, shared with the local CI) and
# refuses to start while any goldbach_l0 process runs.
#
# Environment passed through to goldbach_l0 (defaults set here):
#   GB_BUCKET_VERIFY=100  bucket-sieve tripwire on every 100th segment (~1%)
#   GB_RANGE_SEGMENT=N    segment span (default 467500000)
#   GB_RANGE_VERBOSE=1    render the run's per-segment progress as a single
#                         redrawing progress bar (percent, G ev/s, ETA)
# Script knobs:
#   GB_RANGE_BIN / GB_RANGE_SPV   binary / dense SPIR-V module to run
#   GB_RANGE_TIMEOUT=S    kill a slice after S seconds (counts as a failure;
#                         default 0 = no limit) — catches GPU/driver hangs
#   GB_RANGE_RETRY=0      no retry: the first failure stops the run
#   GB_RANGE_MAX_RETRIES=N  stop after N RETRY events in the last 50 slices (3)
#   GB_RANGE_STOPFILE=F   stop-request file (default <journal base>.stop)
#   GB_RANGE_LOCK=F       lock file (default .goldbach-gpu.lock)
set -u
# Segment span: v1.12.0's optimum (425M, mod-30 class map). v1.11.0 was
# tuned at 350M, older builds at 250M (GB_RANGE_SEGMENT=N).
SEG=${GB_RANGE_SEGMENT:-467500000}
cd "$(dirname "$(readlink -f "$0")")/../goldbach-l0-tidy" 2>/dev/null || cd "$(dirname "$0")/.." || exit 1

START=${1:?usage: run_range.sh START LIMIT [SLICE] [JOURNAL]}
LIMIT=${2:?usage: run_range.sh START LIMIT [SLICE] [JOURNAL]}
SLICE=${3:-200000000000000}
JOURNAL=${4:-range-$START-$LIMIT.journal}
LOG=${JOURNAL%.journal}.log
STOPFILE=${GB_RANGE_STOPFILE:-${JOURNAL%.journal}.stop}
RECHECK=${JOURNAL%.journal}.recheck
LOCK=${GB_RANGE_LOCK:-.goldbach-gpu.lock}
TIMEOUT=${GB_RANGE_TIMEOUT:-0}
RETRY=${GB_RANGE_RETRY:-1}
MAXRETRY=${GB_RANGE_MAX_RETRIES:-3}
export GB_BUCKET_VERIFY=${GB_BUCKET_VERIFY:-100}

die() { echo "FATAL: $*" | tee -a "$LOG" >&2; exit 1; }

# ---- binary / kernels / release id (resolved once) --------------------------
if [ -n "${GB_RANGE_BIN:-}" ]; then BIN=$GB_RANGE_BIN
elif [ -x ci-state/active/goldbach_l0 ]; then BIN=ci-state/active/goldbach_l0
else BIN=./build/goldbach_l0; fi
[ -x "$BIN" ] || die "$BIN not found (run from the goldbach-l0-tidy root, build first)"
BIN=$(readlink -f "$BIN")
BINDIR=$(dirname "$BIN")
SPV=${GB_RANGE_SPV:-}
[ -z "$SPV" ] && [ -f "$BINDIR/kernels/goldbach_bmg.spv" ] && SPV=$BINDIR/kernels/goldbach_bmg.spv
SPVARG=()
if [ "${GB_SPARSE:-0}" = 1 ]; then
    # the sparse path loads its geometry's module (kernels/sparse*_bmg.spv
    # relative to this directory); a dense --spv would be the wrong module
    [ -n "$SPV" ] && echo "note: GB_SPARSE=1 — sparse modules come from ./kernels, not $SPV" >&2
elif [ -n "$SPV" ]; then
    SPVARG=(--spv "$(readlink -f "$SPV")")
fi
if [ -f "$BINDIR/RELEASE" ]; then RELEASE=$(head -c 64 "$BINDIR/RELEASE" | tr -dc 'A-Za-z0-9._+-')
else RELEASE=wt-$(git rev-parse --short HEAD 2>/dev/null || echo unknown)$(git status --porcelain --untracked-files=no -- src include kernels CMakeLists.txt 2>/dev/null | grep -q . && echo +dirty); fi

command -v python3 >/dev/null && CALC=python3 || CALC=""
# 64-bit arithmetic: bash handles signed 64-bit only. 5e18 < 2^63 fits;
# anything >= 2^63 would wrap silently, so refuse it (19 digits max and
# below 9223372036854775807 - SLICE).
[[ "$START" =~ ^[0-9]+$ && "$LIMIT" =~ ^[0-9]+$ && "$SLICE" =~ ^[0-9]+$ ]] ||
    die "START/LIMIT/SLICE must be plain decimal integers (no e-notation here)"
for v in "$START" "$LIMIT" "$SLICE"; do
    # string < on two 19-digit decimals orders them numerically (intended)
    # shellcheck disable=SC2071
    [ ${#v} -le 19 ] && [[ ${#v} -lt 19 || "$v" < 9223372036854775807 ]] ||
        die "$v is >= 2^63: bash arithmetic is signed 64-bit (run such ranges another way)"
done
(( LIMIT + SLICE > LIMIT )) || die "LIMIT + SLICE overflows signed 64-bit"
(( START % 2 == 0 && LIMIT % 2 == 0 && SLICE % 2 == 0 )) || die "START, LIMIT, SLICE must be even"
(( START < LIMIT )) || die "START must be < LIMIT"
(( SLICE > 0 )) || die "SLICE must be > 0"

# ---- one GPU campaign at a time ---------------------------------------------
exec 9>>"$LOCK" || die "cannot open lock file $LOCK"
flock -n 9 || die "$LOCK is held (another run_range.sh or the local CI owns the GPU) — not starting a second campaign"
others=$(pgrep -x "${GB_RANGE_PGREP:-goldbach_l0}" | tr '\n' ' ')
[ -z "$others" ] || die "${GB_RANGE_PGREP:-goldbach_l0} already running (pid $others) — not starting a second GPU job"
# a stop request addressed to an earlier instance must not block this one
[ -e "$STOPFILE" ] && { echo "$(date -Is) note: removed stale stop request $STOPFILE" | tee -a "$LOG"; rm -f "$STOPFILE"; }
touch "$JOURNAL" || die "cannot write $JOURNAL"

# Resume point: follow the chain START -> ... in the journal (same rule as
# ci/goldbach_ci.py chain_next). A row cut off by a crash is ignored; a row
# that extends the chain must agree with its own verifier output (start= /
# limit= of the --csv line), or a damaged b would skip unverified evens.
next=$START
while read -r tag a b rest; do
    [ "$tag" = PASS ] || continue
    if ! [[ "$a" =~ ^[0-9]+$ && "$b" =~ ^[0-9]+$ ]]; then
        echo "warning: malformed journal row ignored: $tag $a $b" >&2; continue
    fi
    if [ "$a" = "$next" ]; then
        [[ " $rest " == *" start=$a "* && " $rest " == *" limit=$b "* ]] ||
            die "journal PASS row [$a, $b] disagrees with its own --csv start=/limit= — $JOURNAL damaged? Investigate before resuming."
        next=$((b + 2))
    fi
done < "$JOURNAL"
if (( next > LIMIT )); then
    echo "range [$START, $LIMIT] already complete per $JOURNAL"
    exit 0
fi
total=$(( (LIMIT - START) / 2 + 1 ))
echo "$(date -Is) run_range: [$START, $LIMIT] slice $SLICE, resuming at $next release=$RELEASE bin=$BIN pid=$$" | tee -a "$LOG"
echo "journal: $(readlink -f "$JOURNAL")   log: $(readlink -f "$LOG")"

# GPU state snapshot (one line of key=value) for failure forensics
gpu_state() {
    local c card="" hw f
    for c in /sys/class/drm/card[0-9]*; do
        [ "$(cat "$c/device/vendor" 2>/dev/null)" = 0x8086 ] && { card=$c; break; }
    done
    [ -n "$card" ] || { echo "gpu=none"; return; }
    f=$card/device/tile0/gt0
    hw=$(ls -d "$card"/device/hwmon/hwmon* 2>/dev/null | head -1)
    printf 'act_mhz=%s cur_mhz=%s mem_speed=%s throttle=%s' \
        "$(cat "$f/freq0/act_freq" 2>/dev/null)" "$(cat "$f/freq0/cur_freq" 2>/dev/null)" \
        "$(cat "$f/oc/mem_speed" 2>/dev/null || echo -)" \
        "$(cat "$f/freq0/throttle/status" 2>/dev/null || echo -)"
    for t in "$hw"/temp*_input; do
        [ -f "$t" ] || continue
        printf ' %s_c=%s' "$(tr ' ' _ < "${t%_input}_label" 2>/dev/null || basename "$t")" "$(( $(cat "$t") / 1000 ))"
    done
    printf ' kmsg_xe_errors_1h=%s\n' \
        "$(journalctl -k --since=-1h --no-pager -q -g 'xe .*(ERROR|reset|hang|GuC|lost)' 2>/dev/null | wc -l)"
}
alert() {   # alert TAG MESSAGE: notification hook (desktop / ntfy / mail), best effort
    local hook=${GB_RECORD_HOOK-bash scripts/notify_record.sh}
    # shellcheck disable=SC2086
    [ -n "$hook" ] && $hook ALERT "$1" "$2" - - >/dev/null 2>&1 &
}

while (( next <= LIMIT )); do
    if [ -e "$STOPFILE" ]; then
        echo "$(date -Is) STOP requested ($STOPFILE): exiting cleanly before slice $next (journal intact, resume point $next)" | tee -a "$LOG"
        rm -f "$STOPFILE"
        exit 3
    fi
    a=$next
    b=$(( a + SLICE - 2 ))
    (( b > LIMIT || b < a )) && b=$LIMIT
    ok=0
    first_fail=""
    VERB=${GB_RANGE_VERBOSE:+--verbose}
    TO=()
    (( TIMEOUT > 0 )) && TO=(timeout --signal=TERM --kill-after=60 "$TIMEOUT")
    for attempt in 1 2; do
        t0=$(date +%s)
        if [ -n "$VERB" ]; then
            # the binary's per-segment progress lines are rendered as one
            # redrawing bar on stderr; stdout still yields the final --csv
            # line for the journal row
            nseg=$(( (b - a + SEG - 1) / SEG ))
            out=$("${TO[@]}" stdbuf -oL "$BIN" --start "$a" --limit "$b" --segment "$SEG" --split 0 "${SPVARG[@]}" --csv $VERB 2>"$LOG.stderr" \
                | awk -v seg="$SEG" -v nseg="$nseg" '
                    BEGIN { w = 28; t0 = systime(); last = -1; total = nseg * seg / 2 }
                    /segment [0-9]+: evens \[/ {
                        n = $2 + 0
                        done = n * seg / 2
                        if (done > total) done = total
                        now = systime()
                        if (n >= nseg || now != last) {
                            last = now
                            el = now - t0
                            rate = (el > 0) ? done / el : 0
                            eta = (rate > 0) ? (total - done) / rate : 0
                            fill = int(w * done / total + 0.5)
                            bar = ""
                            for (i = 0; i < w; i++) bar = bar ((i < fill) ? "#" : "-")
                            printf "\r[%s] %5.1f%% | %6.2f G ev/s | elapsed %02d:%02d:%02d | ETA %02d:%02d:%02d   ", \
                                bar, 100 * done / total, rate / 1e9, \
                                el / 3600, (el / 60) % 60, el % 60, \
                                eta / 3600, (eta / 60) % 60, eta % 60 > "/dev/stderr"
                            if (n >= nseg) printf "\n" > "/dev/stderr"
                        }
                        next
                    }
                    { print }'
                # the binary's status, not awk's: PIPESTATUS only exists inside
                # this subshell (outside, a crash or timeout read as rc=0)
                exit "${PIPESTATUS[0]}")
            rc=$?
            [ "$rc" -ne 0 ] && printf '\n' >&2
        else
            out=$("${TO[@]}" "$BIN" --start "$a" --limit "$b" --segment "$SEG" --split 0 "${SPVARG[@]}" --csv 2>"$LOG.stderr")
            rc=$?
        fi
        cat "$LOG.stderr" >> "$LOG"
        line=$(printf '%s\n' "$out" | grep 'result=' | tail -1)
        if [ "$rc" -eq 0 ] && [[ "$line" == *result=PASS* ]]; then ok=1; break; fi
        why=$(grep -E 'error|FAILED|overflow|differs|flags|what|lost|cannot' "$LOG.stderr" | tail -1 | tr -s ' \t' ' ' | cut -c1-300)
        [ "$rc" = 124 ] && why="timeout after ${TIMEOUT}s (GPU/driver hang?) $why"
        state=$(gpu_state)
        echo "$(date -Is) slice [$a, $b] attempt $attempt FAILED rc=$rc: $line $(tail -3 "$LOG.stderr" | tr '\n' ' ')" | tee -a "$LOG"
        echo "    gpu state: $state" | tee -a "$LOG"
        if [ "$attempt" = 1 ]; then
            first_fail="rc=$rc $why"
            echo "RETRY $a $b $(date -Is) attempt=1 rc=$rc release=$RELEASE $state reason=${why:-unknown}" >> "$JOURNAL"
            [ "$RETRY" = 0 ] && break
            sleep 30
        elif [ "rc=$rc $why" = "$first_fail" ]; then
            echo "$(date -Is) the failure repeated identically (deterministic: a code bug, a real counterexample, or a persistent fault)" | tee -a "$LOG"
        fi
    done
    rm -f "$LOG.stderr"
    if [ "$ok" != 1 ]; then
        echo "$(date -Is) STOPPED: slice [$a, $b] failed$([ "$RETRY" = 0 ] && echo " (retry disabled)" || echo " twice"). Investigate before re-running (see $LOG)." | tee -a "$LOG"
        alert "run STOPPED" "slice [$a, $b] failed: ${first_fail:-?} — investigate $LOG"
        exit 1
    fi
    extra=" release=$RELEASE"
    if [ -n "$first_fail" ]; then
        # passed only on the second attempt: bank it (the second run is a
        # complete, independent verification) but flag it as a hardware-
        # integrity event and queue it for an independent re-run
        extra="$extra attempts=2"
        echo "$a $b $(date -Is) release=$RELEASE first_failure=$first_fail" >> "$RECHECK"
        echo "$(date -Is) WARNING: slice [$a, $b] passed only after a retry (first: $first_fail) — hardware-integrity event, queued in $RECHECK" | tee -a "$LOG"
        alert "slice passed after retry" "[$a, $b] first attempt: $first_fail — queued for recheck"
    fi
    echo "PASS $a $b $(date -Is) $line$extra" >> "$JOURNAL"
    done_evens=$(( (b - START) / 2 + 1 ))
    rate=$(printf '%s\n' "$line" | sed -n 's/.*evens_per_sec=\([^ ]*\).*/\1/p')
    secs=$(( $(date +%s) - t0 ))
    if [ -n "$CALC" ]; then
        prog=$($CALC -c "d=$done_evens;t=$total;r=float('$rate');print(f'{100*d/t:.4f}% done, ~{(t-d)/r/86400:.1f} days left at {r/1e9:.2f} G evens/s')")
    else
        prog="$done_evens / $total evens"
    fi
    echo "$(date -Is) PASS [$a, $b] in ${secs}s — $prog" | tee -a "$LOG"
    next=$(( b + 2 ))
    if [ -n "$first_fail" ]; then
        nretry=$(tail -n 100 "$JOURNAL" | grep -c '^RETRY ')
        if (( nretry >= MAXRETRY )); then
            echo "$(date -Is) STOPPED: $nretry retried slices among the last journal rows — flaky hardware? (GB_RANGE_MAX_RETRIES=$MAXRETRY). Resume point $next is intact." | tee -a "$LOG"
            alert "run STOPPED (flaky hardware)" "$nretry retried slices recently — see $LOG"
            exit 4
        fi
    fi
done
echo "$(date -Is) COMPLETE: every even in [$START, $LIMIT] verified (journal $JOURNAL)" | tee -a "$LOG"
