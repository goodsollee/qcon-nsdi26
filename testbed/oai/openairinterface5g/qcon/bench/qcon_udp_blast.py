#!/usr/bin/env python3
"""Rate-controlled UDP blast for QCON Stage 8 microbench.

Sends $count bursts of $bs-byte UDP datagrams to host:port at $rate Mbps.
Used in lieu of iperf3 (which we don't have on Pixel arm64) — purpose
is just to drive PDCP DL traffic; we measure split via gNB counters.

Usage:
  qcon_udp_blast.py <host> <port> <rate_mbps> <duration_s> [bs=1200]
"""
import socket, sys, time, os

host = sys.argv[1]
port = int(sys.argv[2])
rate_mbps = float(sys.argv[3])
duration = float(sys.argv[4])
bs = int(sys.argv[5]) if len(sys.argv) > 5 else 1200

pps = (rate_mbps * 1e6 / 8.0) / bs
sleep_per_pkt = 1.0 / pps
payload = os.urandom(bs)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

print(f"[blast] -> {host}:{port}  {rate_mbps} Mbps × {duration}s  bs={bs}  pps≈{pps:.0f}", flush=True)
t0 = time.monotonic()
deadline = t0 + duration
sent = 0
next_t = t0
while time.monotonic() < deadline:
    try:
        sock.sendto(payload, (host, port))
        sent += 1
    except OSError:
        time.sleep(0.001)
        continue
    next_t += sleep_per_pkt
    delay = next_t - time.monotonic()
    if delay > 0:
        time.sleep(delay)

elapsed = time.monotonic() - t0
print(f"[blast] sent={sent}  elapsed={elapsed:.2f}s  effective_mbps={sent*bs*8/elapsed/1e6:.2f}", flush=True)
