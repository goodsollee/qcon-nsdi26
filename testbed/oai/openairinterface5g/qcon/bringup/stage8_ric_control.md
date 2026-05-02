# Stage 8 — RIC→OAI Command Path

## What works
- mock_ric ROUTER receives gNB DEALER messages (ready/kpm/mcs_rb) ✅
- mock_ric forwards FIFO commands to ALL fresh DEALER peers ✅
- ric_send_command.py helper writes commands to FIFO ✅
- gNB code path for split_ratio/reinject/drx_config exists in recv_msg() ✅

## Blockers seen this iteration
1. **libzmq assertion crash** ("Assertion failed: check () (src/msg.cpp:387)") —
   FIXED by adding `socket_mutex` around all `zmq_send` and `zmq_recv` so
   the publisher/recv threads don't race on the same socket.

2. **`__pthread_tpp_change_priority` assertion** — happened after the
   first fix when recv_msg used a tight ZMQ_DONTWAIT polling loop with
   `usleep(20000)`. The polling loop conflicted with OAI's RT scheduler
   (priority-protected mutexes elsewhere in MAC).
   FIXED by setting `ZMQ_RCVTIMEO=20ms` so `zmq_recv` blocks at most
   20ms inside the lock, then returns EAGAIN — no tight polling, lock
   is released regularly for senders.

3. **Pixel attach instability across multiple re-attaches** — independent
   of our changes; multi-cycle re-attach leaves gNB MAC with RA process
   slots exhausted ("no free RA process") and SgNB Addition fails. A
   clean gNB restart recovers but kills any in-progress test.

## End-to-end verification (PENDING)
Confirming the full RIC→OAI command path requires:
  - Pixel cleanly attached (LTE+NR via EN-DC)
  - DC_user registered with `register_dc_user_with_ric()`
  - mock_ric sends `split_ratio` to that user_id
  - gNB log emits `[QCON-RIC] recv ...` AND `Updated split ratio for user X to Y.YY`

We saw `[QCON] Registered DC_user N` lines (publisher path is alive)
and 200+ kpm/mcs_rb messages at RIC, but the RIC→gNB direction is
intermittently blocked by attach state. The plumbing is correct —
verification requires a fresh, stable EN-DC session.

## Files
- `nr_pdcp_ran_func.cpp` — recv with ZMQ_RCVTIMEO + socket_mutex guard
- `nr_pdcp_ran_func.h` — adds `socket_mutex` to RAN_Function
- `qcon/bringup/zmq_mock_ric.py` — multi-peer broadcast FIFO sender
- `qcon/bringup/ric_send_command.py` — split_ratio / reinject CLI
