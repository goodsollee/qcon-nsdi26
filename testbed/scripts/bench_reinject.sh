#!/usr/bin/env bash
# QCON Stage 8 — reinject microbenchmark.
#
# Crafts a UDP packet with a magic 8-byte signature in the payload and asks
# the gNB (via mock_ric FIFO) to inject it into the PDCP DL pipeline. We
# expect the magic to surface on the Pixel rmnet1 within a short window.
#
# We send a *complete IPv4+UDP* packet because sdap_data_req feeds into the
# DRB which expects an IP packet. Without a proper IP header the UE will
# drop it at the IP layer (or pass it up but no userspace listener picks it).
# So we make it a UDP packet to an open netcat listener on the UE.
#
# Pre-requisites (caller must have set up):
#   - mock_ric, eNB, gNB, Pixel attached, DC_user registered
# This script starts/stops its own Pixel-side `nc -u -l -p PORT > file` sink
# (no root needed — `/sdcard/Download/` is world-writable on Android).
set -u

GNB_LOG=${GNB_LOG:-/tmp/qcon_runs/gnb_latest.log}
RIC_CMD=${RIC_CMD:-${REPO_ROOT}/ric/ric_send_command.py}
MAGIC=${MAGIC:-deadbeefcafebabe}
RESULT=${RESULT:-/tmp/qcon_runs/bench_reinject_$(date +%Y%m%d_%H%M%S).log}

DC_ID=$(grep -aE "Registered DC_user [0-9]+" "$GNB_LOG" | tail -1 \
        | grep -oE "DC_user [0-9]+" | grep -oE "[0-9]+")
[ -z "$DC_ID" ] && { echo "[bench] FAIL: no DC_user; attach Pixel first."; exit 1; }
echo "[bench] target DC_user=$DC_ID"

# Pixel rmnet1 IP
UE_IP=$(adb shell "ip -4 addr show rmnet1 2>/dev/null" | grep -oE "inet [0-9.]+" | awk '{print $2}' | head -1)
[ -z "$UE_IP" ] && { echo "[bench] FAIL: no rmnet1 IP — Pixel not on EN-DC."; exit 1; }
echo "[bench] UE_IP=$UE_IP"

# Build IPv4+UDP packet — use python scapy if available, else manual.
PORT=9999
PKT_HEX=$(python3 - <<PY
import struct, ipaddress, sys
src_ip = ipaddress.ip_address("12.1.1.99").packed
dst_ip = ipaddress.ip_address("$UE_IP").packed
sport, dport = 5555, $PORT
payload = bytes.fromhex("$MAGIC") + b"QCON-REINJECT-TEST"

# UDP
udp_len = 8 + len(payload)
udp_hdr_no_csum = struct.pack("!HHHH", sport, dport, udp_len, 0)
# Pseudo-header for UDP checksum
ph = src_ip + dst_ip + b"\x00\x11" + struct.pack("!H", udp_len)
def csum(b):
    if len(b)%2: b += b"\x00"
    s = 0
    for i in range(0,len(b),2):
        s += (b[i]<<8) | b[i+1]
    while s>>16: s = (s & 0xffff) + (s>>16)
    return (~s) & 0xffff
udp_csum = csum(ph + udp_hdr_no_csum + payload)
udp = struct.pack("!HHHH", sport, dport, udp_len, udp_csum) + payload

# IPv4
ip_total = 20 + len(udp)
ip_hdr_no_csum = struct.pack("!BBHHHBBH4s4s",
    0x45, 0, ip_total, 0, 0, 64, 17, 0, src_ip, dst_ip)
ip_csum = csum(ip_hdr_no_csum)
ip = struct.pack("!BBHHHBBH4s4s",
    0x45, 0, ip_total, 0, 0, 64, 17, ip_csum, src_ip, dst_ip)

print((ip+udp).hex())
PY
)
echo "[bench] hex pkt (${#PKT_HEX}/2 B): ${PKT_HEX:0:80}..."

# Start UDP listener on Pixel — write incoming bytes to /sdcard for hex inspect.
SINK=/sdcard/Download/qcon_reinject.bin
adb shell "rm -f $SINK; pkill -f 'nc -u' 2>/dev/null; nohup nc -u -l -p $PORT > $SINK 2>/dev/null &"
sleep 1

# Send 3 reinject commands spaced 1s.
for i in 1 2 3; do
    python3 "$RIC_CMD" reinject "$DC_ID" "$PKT_HEX" 1 >/dev/null
    sleep 1
done
sleep 1

# Stop listener, pull file, hex dump.
adb shell "pkill -f 'nc -u' 2>/dev/null"
adb shell "ls -la $SINK; xxd $SINK 2>/dev/null | head -10" > "$RESULT"
echo "=== UE-side UDP sink ($SINK) ==="
cat "$RESULT"
echo ""
echo "=== gNB reinject log lines ==="
grep -aE "QCON-RIC.*reinject|QCON-RIC.*recv" "$GNB_LOG" | tail -10

# Verdict — magic appears in hex dump?
HITS=$(grep -c -iE "${MAGIC:0:8}|${MAGIC:8:8}" "$RESULT" 2>/dev/null || echo 0)
echo ""
if [ "$HITS" -gt 0 ]; then
    echo "[bench] PASS — magic ($MAGIC) seen $HITS times on UE rmnet1"
    exit 0
else
    echo "[bench] FAIL — magic not seen on UE."
    echo "  Common causes: PDCP/RLC dropped the pkt; IP header wrong; UE filtered;"
    echo "  reinject path crashed (check gNB log for QCON-RIC errors)."
    exit 1
fi
