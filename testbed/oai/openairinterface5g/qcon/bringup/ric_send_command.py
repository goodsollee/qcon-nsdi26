#!/usr/bin/env python3
"""
QCON Stage 8 — Send a control command from RIC (ROUTER) to OAI gNB (DEALER).

Talks to the ROUTER socket (peer of the gNB DEALER) by spawning a temporary
DEALER on the same endpoint — but ROUTERs don't proxy. So instead we
piggyback on the *running* mock_ric process via its log+IPC: simpler is
to reuse a small dedicated client that connects via its OWN DEALER and
sends a request expecting the RIC stub to reply.

Practical approach: this script DOES NOT use the same socket the gNB
talks to. Instead, the running zmq_mock_ric.py is augmented to read
commands from a FIFO (qcon/bringup/cmd.fifo) and forward them to the
gNB's DEALER identity. To keep things minimal here, we just demonstrate
the wire format and let zmq_mock_ric handle delivery.

For now, this script PUBLISHES the command JSON to the mock_ric stdin
via filesystem channel (cmd.fifo), and the mock reads + sends.

Usage:
    python3 ric_send_command.py split_ratio <user_id> <ratio>
    python3 ric_send_command.py reinject  <user_id> <hex_payload> [rb_id]
       e.g. reinject 16556 deadbeefcafebabe   # raw bytes; b64'd before send
"""
import sys, json, os, base64, binascii

FIFO = "/tmp/qcon_runs/ric_cmd.fifo"

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "split_ratio":
        user_id = int(sys.argv[2])
        ratio   = float(sys.argv[3])
        msg = {"header": "split_ratio",
               "payload": {"user_id": user_id, "ratio": ratio}}
    elif cmd == "reinject":
        user_id = int(sys.argv[2])
        raw     = binascii.unhexlify(sys.argv[3])
        rb_id   = int(sys.argv[4]) if len(sys.argv) > 4 else 1
        msg = {"header": "reinject",
               "payload": {"user_id": user_id,
                            "rb_id":   rb_id,
                            "packet_data": base64.b64encode(raw).decode()}}
    else:
        print(f"unknown command {cmd}")
        sys.exit(1)

    if not os.path.exists(FIFO):
        os.mkfifo(FIFO)
    with open(FIFO, "w") as f:
        f.write(json.dumps(msg) + "\n")
    print(f"[ric-cmd] sent: {msg}")

if __name__ == "__main__":
    main()
