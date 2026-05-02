#!/usr/bin/env python3
"""
QCON Stage 7+8 — Mock RIC server.

Listens on tcp://*:7878 (ROUTER) for messages from OAI gNB DEALER:
  - "ready"   one-shot heartbeat
  - "kpm"     per-UE EWMA throughput / queue / split (from publisher thread)
  - "mcs_rb"  per-UE MCS-derived BW capacity (Stage 6b)

Also reads commands from /tmp/qcon_runs/ric_cmd.fifo (one JSON per line)
and forwards them to the most recently seen DEALER, enabling RIC→OAI control.
"""
import zmq, json, time, os, threading

FIFO_PATH = "/tmp/qcon_runs/ric_cmd.fifo"

ctx = zmq.Context()
sock = ctx.socket(zmq.ROUTER)
sock.bind("tcp://0.0.0.0:7878")
print("[mock-ric] listening on :7878 (ROUTER)", flush=True)

peer_lock = threading.Lock()
# All seen peers — broadcast FIFO commands to ALL of them so a stray
# test DEALER doesn't steal traffic from the gNB. Peers are evicted if
# unseen for STALE_AFTER seconds.
peers = {}  # bytes(identity) -> last_seen_ts
STALE_AFTER = 10.0  # seconds
counts = {}

def fifo_sender():
    """Consume commands from FIFO and forward to ALL fresh DEALER peers."""
    if not os.path.exists(FIFO_PATH):
        try: os.mkfifo(FIFO_PATH)
        except FileExistsError: pass
    while True:
        try:
            with open(FIFO_PATH, "r") as f:
                for line in f:
                    line = line.strip()
                    if not line: continue
                    now = time.time()
                    with peer_lock:
                        # purge stale
                        for pid in list(peers):
                            if now - peers[pid] > STALE_AFTER:
                                peers.pop(pid, None)
                        live = list(peers.keys())
                    if not live:
                        print(f"[mock-ric] FIFO cmd ignored (no live peers): {line[:120]}", flush=True)
                        continue
                    for pid in live:
                        sock.send_multipart([pid, line.encode()])
                    print(f"[mock-ric] >> ({len(live)} peers) {line[:160]}", flush=True)
        except Exception as e:
            print(f"[mock-ric] fifo err: {e}", flush=True)
            time.sleep(0.5)

threading.Thread(target=fifo_sender, daemon=True).start()

start = time.time()
while True:
    try:
        parts = sock.recv_multipart()
        if len(parts) >= 2:
            with peer_lock:
                peers[parts[0]] = time.time()
            payload = parts[-1]
        else:
            payload = parts[0]
        try:
            obj = json.loads(payload.decode())
            hdr = obj.get("header", "?")
            counts[hdr] = counts.get(hdr, 0) + 1
            if counts[hdr] <= 3 or counts[hdr] % 50 == 0:
                print(f"[mock-ric] {hdr} #{counts[hdr]}: {json.dumps(obj)[:240]}", flush=True)
        except Exception as e:
            print(f"[mock-ric] non-json {len(payload)}B: {payload[:80]} ({e})", flush=True)
    except KeyboardInterrupt:
        break
    except Exception as e:
        print(f"[mock-ric] err: {e}", flush=True)
        time.sleep(0.5)
print(f"\n[mock-ric] summary after {time.time()-start:.0f}s: {counts}")
