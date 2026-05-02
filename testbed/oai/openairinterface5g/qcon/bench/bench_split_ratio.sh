#!/usr/bin/env bash
# QCON Stage 8 — split_ratio microbenchmark.
#
# Assumes already running:
#   - mock_ric (with multi-peer FIFO broadcast)
#   - eNB + gNB
#   - Pixel attached EN-DC; one DC_user registered with publisher
#
# Drives DL UDP traffic to the UE via Python blast (no iperf3 needed),
# sweeps target_split_ratio every $SEG s, reads kpm payload's
# lte_pkts/nr_pkts deltas, prints per-segment observed ratio vs commanded.
set -u

SEG=${SEG:-10}                       # seconds per ratio
RATIOS=${RATIOS:-"0.0 0.25 0.5 0.75 1.0"}
RATE_MBPS=${RATE_MBPS:-10}           # DL UDP rate
DST_PORT=${DST_PORT:-5201}
MOCK_LOG=${MOCK_LOG:-/tmp/qcon_runs/mock_ric.log}
GNB_LOG=${GNB_LOG:-/tmp/qcon_runs/gnb_latest.log}
RESULT=${RESULT:-/tmp/qcon_runs/bench_split_$(date +%Y%m%d_%H%M%S).json}
RIC_CMD=${RIC_CMD:-${REPO_ROOT}/ric/ric_send_command.py}
BLAST=${BLAST:-${REPO_ROOT}/testbed/scripts/qcon_udp_blast.py}

echo "[bench] $(date) seg=${SEG}s rate=${RATE_MBPS}Mbps ratios=$RATIOS"

# 1. Find DC_user — prefer the one with the most kpm publishes in the
# last few hundred messages (= currently active). Fall back to last
# Registered DC_user log line. Override via DC_USER env var.
DC_ID="${DC_USER:-}"
if [ -z "$DC_ID" ]; then
    DC_ID=$(tail -500 "$MOCK_LOG" 2>/dev/null \
            | grep -oE 'user_id\\":[0-9]+' \
            | grep -oE '[0-9]+' \
            | sort | uniq -c | sort -rn | head -1 | awk '{print $2}')
fi
if [ -z "$DC_ID" ]; then
    DC_ID=$(grep -aE "Registered DC_user [0-9]+" "$GNB_LOG" | tail -1 \
            | grep -oE "DC_user [0-9]+" | grep -oE "[0-9]+")
fi
if [ -z "$DC_ID" ]; then
    echo "[bench] FAIL: no DC_user registered. Attach Pixel first."
    exit 1
fi
echo "[bench] target DC_user = $DC_ID"

# 2. Start UDP sink on Pixel + DL blast from host.
UE_IP=$(adb shell "ip -4 addr show rmnet1 2>/dev/null" | grep -oE "inet [0-9.]+" | awk '{print $2}' | head -1)
[ -z "$UE_IP" ] && { echo "[bench] FAIL: no rmnet1 IP — Pixel not on EN-DC."; exit 1; }
echo "[bench] UE_IP=$UE_IP"

# Pixel UDP sink — keep-alive loop via setsid (Pixel toybox nc has no -k).
# qcon_pixel_sink.sh runs `while true; do nc -u -l -p PORT >/dev/null; done`
# which restarts nc on every datagram (toybox nc exits per UDP packet).
# setsid detaches so nohup-killed adb shell session doesn't take it down.
adb shell "pkill -f qcon_pixel_sink 2>/dev/null; pkill -f 'nc -u -l' 2>/dev/null"
sleep 1
SINK_SH=/data/local/tmp/qcon_pixel_sink.sh
if ! adb shell "[ -x $SINK_SH ] && echo present" 2>&1 | grep -q present; then
    adb push /tmp/qcon_pixel_sink.sh "$SINK_SH" >/dev/null 2>&1
    adb shell "chmod +x $SINK_SH"
fi
adb shell "setsid $SINK_SH $DST_PORT </dev/null >/dev/null 2>&1 &"
sleep 2
SINK_PROCS=$(adb shell "ps -ef 2>&1 | grep qcon_pixel_sink | grep -v grep | wc -l" | tr -d '\r')
echo "[bench] Pixel sink loop: $SINK_PROCS proc(s) running"

N_SEG=$(echo "$RATIOS" | wc -w)
TOTAL=$(( N_SEG * SEG + 2 ))
echo "[bench] python3 udp_blast → $UE_IP:$DST_PORT  ${RATE_MBPS}Mbps × ${TOTAL}s"
python3 "$BLAST" "$UE_IP" "$DST_PORT" "$RATE_MBPS" "$TOTAL" \
    > /tmp/qcon_runs/bench_blast.log 2>&1 &
BLAST_PID=$!

# 3. Sweep ratios — capture lte_pkts/nr_pkts before & after each segment.
declare -A BEFORE_LTE BEFORE_NR AFTER_LTE AFTER_NR
read_counts() {
    # Returns "lte_pkts nr_pkts" from the most recent kpm payload for $DC_ID.
    grep -aE "kpm.*\"user_id\\\\\":${DC_ID}" "$MOCK_LOG" | tail -1 \
        | grep -oE 'lte_pkts\\":[0-9]+|nr_pkts\\":[0-9]+' \
        | awk -F'":' 'BEGIN{l=0;n=0} /lte_pkts/{l=$2} /nr_pkts/{n=$2} END{print l, n}'
}

# Settle window so iperf is in steady state before first measurement.
sleep 2

i=0
for r in $RATIOS; do
    i=$((i+1))
    read l n < <(read_counts); echo "[seg $i] before  lte=$l  nr=$n"
    BEFORE_LTE[$i]=$l; BEFORE_NR[$i]=$n
    python3 "$RIC_CMD" split_ratio "$DC_ID" "$r" >/dev/null
    sleep "$SEG"
    read l n < <(read_counts); echo "[seg $i] after   lte=$l  nr=$n  (cmd=$r)"
    AFTER_LTE[$i]=$l; AFTER_NR[$i]=$n
done

wait $BLAST_PID 2>/dev/null
adb shell "pkill -f qcon_pixel_sink 2>/dev/null; pkill -f 'nc -u -l' 2>/dev/null"

# 4. Report.
echo ""
echo "=================  RESULTS  ================="
printf "%-4s %-8s %-12s %-12s %-12s %-8s\n" "seg" "cmd" "delta_lte" "delta_nr" "obs_ratio" "verdict"
i=0
PASS=0; TOTAL_RUNS=0
echo "{\"segments\": [" > "$RESULT"
for r in $RATIOS; do
    i=$((i+1))
    dl=$(( AFTER_LTE[$i] - BEFORE_LTE[$i] ))
    dn=$(( AFTER_NR[$i]  - BEFORE_NR[$i]  ))
    sum=$(( dl + dn ))
    if [ "$sum" -eq 0 ]; then
        obs="N/A"
        verdict="NO_TRAFFIC"
    else
        obs=$(awk -v l=$dl -v s=$sum 'BEGIN{printf "%.3f", l/s}')
        verdict=$(awk -v o=$obs -v t=$r 'BEGIN{d=o-t; if(d<0)d=-d; print (d<=0.05)?"PASS":"FAIL"}')
        if [ "$verdict" = "PASS" ]; then PASS=$((PASS+1)); fi
        TOTAL_RUNS=$((TOTAL_RUNS+1))
    fi
    printf "%-4s %-8s %-12s %-12s %-12s %-8s\n" "$i" "$r" "$dl" "$dn" "$obs" "$verdict"
    [ "$i" -gt 1 ] && echo "  ," >> "$RESULT"
    echo "  {\"seg\":$i,\"cmd\":$r,\"delta_lte\":$dl,\"delta_nr\":$dn,\"obs\":\"$obs\",\"verdict\":\"$verdict\"}" >> "$RESULT"
done
echo "]," >> "$RESULT"
echo "\"pass\":$PASS, \"total\":$TOTAL_RUNS}" >> "$RESULT"

echo ""
echo "[bench] Updated split ratio log lines in gNB:"
grep -ac "Updated split ratio" "$GNB_LOG"
echo "[bench] [QCON-RIC] recv lines in gNB:"
grep -ac "QCON-RIC.*recv" "$GNB_LOG"
echo ""
echo "[bench] $PASS / $TOTAL_RUNS segments PASS  (±0.05 tolerance)"
echo "[bench] result JSON: $RESULT"
[ "$PASS" -eq "$TOTAL_RUNS" ] && [ "$TOTAL_RUNS" -gt 0 ]
