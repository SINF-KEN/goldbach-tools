#!/usr/bin/env bash
# live_stats.sh — live dashboard for a run_range.sh campaign, second terminal.
#
#   bash tools/live_stats.sh START LIMIT [JOURNAL] [INTERVAL_S]
#
# JOURNAL defaults to range-$START-$LIMIT.journal (run_range.sh's convention);
# the live per-segment feed is read from ${JOURNAL%.journal}.log.stderr, which
# run_range.sh's running verifier writes in real time. Works for both paths:
#   sparse:  "  seg N=.. dl=..: tile .. large .. A .. B .. ms, pending .. (..%) left .."
#   dense:   "  segment N: evens [a, b]  leftovers_total=.."
#
# Start the campaign in another terminal first, e.g.
#   GB_SPARSE=1 bash tools/run_range.sh 4000000000000000000 5000000000000000000
# (GB_RANGE_VERBOSE is unnecessary: its bar cannot parse the sparse segment
# lines; live_stats computes progress from the same feed itself.)
#
# Read-only: it only reads the journal, the stderr feed and GPU sysfs.
set -u
export LC_ALL=C   # decimal point in awk/bash printf (comma locales break the bar)
START=${1:?usage: live_stats.sh START LIMIT [JOURNAL] [INTERVAL_S]}
LIMIT=${2:?usage: live_stats.sh START LIMIT [JOURNAL] [INTERVAL_S]}
JOURNAL=${3:-range-$START-$LIMIT.journal}
INTERVAL=${4:-2}
lver=""; lgpu=""; lrate=""; fill=""; pct=0; rate=0; mean=0
LOGERR=${JOURNAL%.journal}.log.stderr
E="grep -oE"

# Intel GPU (xe) card + hwmon, resolved once.
CARD=""
for c in /sys/class/drm/card?; do
    [ "$(cat "$c/device/vendor" 2>/dev/null)" = "0x8086" ] && CARD=$c && break
done
HWM=$(ls -d "$CARD"/device/hwmon/hwmon* 2>/dev/null | head -1)
FREQF="$CARD/device/tile0/gt0/freq0/act_freq"
# pkg temperature: the tempN_input whose _label contains "pkg"
TEMPI=""
if [ -n "$HWM" ]; then
    for l in "$HWM"/temp*_label; do
        grep -qi pkg "$l" 2>/dev/null || continue
        TEMPI=${l%_label}_input; break
    done
fi
ENERGYI=""
for l in "$HWM"/energy*_label; do
    grep -qi card "$l" 2>/dev/null || continue
    ENERGYI=${l%_label}_input; break
done
[ -z "$ENERGYI" ] && ENERGYI=$(ls "$HWM"/energy1_input 2>/dev/null || true)

human_n() { # compact 1.23e15 style
    awk -v x="${1:-0}" 'BEGIN { if (x + 0 <= 0) { print "0"; exit }
        printf "%.3fe%d", x / 10 ^ int(log(x) / log(10)), int(log(x) / log(10)) }' 2>/dev/null
}
human_t() { # seconds -> [Dd]HH:MM:SS
    awk -v s="$1" 'BEGIN { d = int(s / 86400); s -= d * 86400; printf "%02dd%02d:%02d:%02d", d, int(s/3600), int(s/60)%60, int(s)%60 }'
}

prev_pos=0; prev_t=0; prev_e=0
e0=$(cat "$ENERGYI" 2>/dev/null || echo 0); t_e=$(date +%s.%N)

while true; do
    now=$(date +%s.%N)
    out=""
    # ---- journal: banked slices ----
    last=""; npass=0
    if [ -f "$JOURNAL" ]; then
        last=$(grep '^PASS ' "$JOURNAL" | tail -1)
        npass=$(grep -c '^PASS ' "$JOURNAL")
    fi
    banked=0; last_b=""
    if [ -n "$last" ]; then
        last_b=$(echo "$last" | awk '{print $3}')
        banked=$(( (last_b - START) / 2 + 1 ))
        csv=$(echo "$last" | cut -d' ' -f5-)
        lver=$(echo "$csv" | $E 'version=[^ ]*' | cut -d= -f2)
        lrate=$(echo "$csv" | $E 'evens_per_sec=[^ ]*' | cut -d= -f2)
        lgpu=$(echo "$csv" | $E 'gpu_s=[^ ]*' | cut -d= -f2)
    fi
    total=$(( (LIMIT - START) / 2 + 1 ))
    a_cur=$START; [ -n "$last_b" ] && a_cur=$((last_b + 2))

    # ---- live feed: latest segment line ----
    segline=""; pos=0; kern=""; pend=""; N=""; dl=""; tle=""; lge=""; A=""; B=""; pleft=""; prate=""
    if [ -f "$LOGERR" ]; then
        segline=$(tail -c 400000 "$LOGERR" | grep -E 'seg N=|segment [0-9]+: evens' | tail -1)
    fi
    M=${GB_SCM_M:-30030}
    if echo "$segline" | grep -q 'seg N='; then
        read -r N dl tle lge A B pleft prate <<< "$(echo "$segline" | sed -nE \
            's/.*seg N=([0-9]+) dl=([0-9]+): tile ([0-9.]+) large ([0-9.]+) A ([0-9.]+) B ([0-9.]+) ms, pending [0-9]+ \(([0-9.]+)%\) left ([0-9]+).*/\1 \2 \3 \4 \5 \6 \8 \7/p')"
        if [ -n "$N" ]; then
            pos=$(( (N - a_cur) / 2 + M * dl / 2 ))   # evens done in this slice
            kern=$(printf "tile %s large %s | A %s B %s ms" "$tle" "$lge" "$A" "$B")
            pend=$(printf "pending %s%%  left %s" "$prate" "$pleft")
        fi
    elif echo "$segline" | grep -q 'segment [0-9]*: evens'; then
        b=$(echo "$segline" | sed -nE 's/.*evens \[([0-9]+), ([0-9]+)\].*/\2/p')
        [ -n "$b" ] && { pos=$(( (b - a_cur) / 2 + 1 )); kern="(dense path)"; }
    fi
    done_g=$((banked + pos))

    # ---- rates ----
    rate=0
    if [ "$prev_pos" -gt 0 ] && [ "$pos" -gt "$prev_pos" ]; then
        rate=$(awk -v d="$((pos - prev_pos))" -v t="$(awk -v a="$now" -v b="$prev_t" 'BEGIN{print a-b}')" 'BEGIN{ if (t > 0) printf "%.0f", d / t }')
    fi
    prev_pos=$pos; prev_t=$now
    mean=0
    # mean over banked slices: banked evens / wall time from the first PASS
    # row's timestamp to the last one
    if [ -f "$JOURNAL" ] && [ "$npass" -gt 0 ]; then
        mean=$(awk -v banked="$banked" -v now="$(date +%s)" '
            /^PASS / { n++; if (n == 1) t0 = $4; t1 = $4 }
            END { if (n == 0) exit
                  "date -d " t0 " +%s" | getline s0; close("date -d " t0 " +%s")
                  "date -d " t1 " +%s" | getline s1; close("date -d " t1 " +%s")
                  dt = (n == 1) ? now - s0 : s1 - s0
                  if (dt > 0) printf "%.0f", banked / dt }' "$JOURNAL")
    fi
    show_rate=$rate; [ "$show_rate" -eq 0 ] && show_rate=$mean

    # ---- GPU ----
    gline="no Intel GPU found"
    if [ -n "$CARD" ]; then
        f=$(cat "$FREQF" 2>/dev/null); fmax=$(cat "$CARD/device/tile0/gt0/freq0/max_freq" 2>/dev/null)
        t=$(cat "$TEMPI" 2>/dev/null); [ -n "$t" ] && t=$(awk -v x="$t" 'BEGIN{printf "%.0f", x/1000}')
        e1=$(cat "$ENERGYI" 2>/dev/null || echo 0)
        w=$(awk -v e1="$e1" -v e0="$prev_e" -v n="$now" -v t0="$t_e" 'BEGIN{ d=e1-e0; dt=n-t0; if (dt>0 && d>0) printf "%.0f", d/1e6/dt; else print "-" }')
        prev_e=$e1; t_e=$now
        gline=$(printf "%s MHz (cap %s) | %s C | %s W" "${f:-?}" "${fmax:-?}" "${t:-?}" "$w")
    fi

    # ---- render ----
    pct=$(awk -v d="$done_g" -v t="$total" 'BEGIN{ if (t>0) printf "%.4f", 100*d/t; else print 0 }')
    eta="-"
    if [ "$show_rate" -gt 0 ] && [ "$done_g" -lt "$total" ]; then
        eta=$(human_t "$(awk -v r="$show_rate" -v d="$((total - done_g))" 'BEGIN{printf "%.0f", d / r}')")
    fi
    fill=$(awk -v p="$pct" 'BEGIN{ w=30; f=int(w*p/100+0.5); b=""; for(i=0;i<w;i++) b = b ((i<f)?"#":"-"); print b }')
    age=$(awk -v m="$(stat -c %Y "$LOGERR" 2>/dev/null || echo 0)" -v n="$(date +%s)" 'BEGIN{ x=n-m; if (x<180) printf "%ss ago", x; else printf "STALE %dm", x/60 }' 2>/dev/null)
    printf '\033[H\033[2J'
    printf 'live stats [%s, %s]  %s\n' "$(human_n "$START")" "$(human_n "$LIMIT")" "$(date '+%H:%M:%S')"
    printf 'campaign [%s] %s %.2f%%  banked %s / %s evens\n' "$npass" "$fill" "$pct" "$(human_n "$done_g")" "$(human_n "$total")"
    printf '         overall ETA %s  at %s G ev/s\n' "$eta" "$(awk -v r="$show_rate" 'BEGIN{printf "%.1f", r/1e9}')"
    printf '         mean(banked) %s G | last slice %s G (%s, gpu %s s)\n' \
        "$(awk -v r="$mean" 'BEGIN{printf "%.1f", r/1e9}')" \
        "$(awk -v r="$lrate" 'BEGIN{printf "%.1f", r/1e9}')" "${lver:-}" "${lgpu:-}"
    if [ -n "$segline" ]; then
        printf 'live     slice from %s | last segment line %s\n' "$a_cur" "$age"
        printf '         instant %s G ev/s | %s\n' "$(awk -v r="${rate:-0}" 'BEGIN{printf "%.1f", r/1e9}')" "$kern"
        printf '         %s\n' "$pend"
    elif [ "$npass" -gt 0 ]; then
        printf 'live     idle (no run writing %s)\n' "$LOGERR"
    elif [ -f "$LOGERR" ]; then
        printf 'live     run is up but no per-segment feed: restart it with\n'
        printf '         GB_RANGE_VERBOSE=1 (or GB_PHASE=1) for kernel stats\n'
    else
        printf 'live     waiting for the run to create %s\n' "$LOGERR"
    fi
    printf 'gpu      %s\n' "$gline"

    # ---- ridges: persistent top-30 store written by dense verifier runs ----
    # Pure string handling: the evens exceed awk's exact double range.
    RFILE=${JOURNAL%/*}/goldbach_ridges.csv; [ "$RFILE" = "$JOURNAL" ] && RFILE=goldbach_ridges.csv
    if [ -f "$RFILE" ]; then
        rage=$(awk -v m="$(stat -c %Y "$RFILE" 2>/dev/null || echo 0)" -v n="$(date +%s)" \
            'BEGIN{ x=n-m; if (x<3600) printf "%dm", x/60; else printf "%.1fh", x/3600 }')
        printf 'ridges   My Top 30 Largest Goldbach Ridges (%s, updated %s ago)\n' "$RFILE" "$rage"
        grep -v '^#' "$RFILE" | head -30 | while IFS=, read -r rk rn rp rq rseg rfound; do
            [ -n "$rn" ] || continue
            printf '  %3s. ridge %-6s %s = %s + %s\n' "$rk" "$rp" "$rn" "$rq" "$rp"
        done
    else
        printf 'ridges   no store yet (%s; dense runs write it)\n' "$RFILE"
    fi
    sleep "$INTERVAL"
done
