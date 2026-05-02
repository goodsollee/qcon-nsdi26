# Stage 6 — KPM publisher (per-UE throughput → RIC)

## Stage 6a result — verified
mock_ric.log:
```
[mock-ric] ready #1: {"build":"qcon-bringup","pid":3282439}
[mock-ric] kpm #1:  {"ewma_lte_mbps":0.0,           "ewma_nr_mbps":0.0,  ..., "user_id":59843}
[mock-ric] kpm #2:  {"ewma_lte_mbps":0.0303,        "ewma_nr_mbps":0.0,  ..., "user_id":59843}
[mock-ric] kpm #3:  {"ewma_lte_mbps":0.0152,        "ewma_nr_mbps":0.0,  ..., "user_id":59843}
```

Cadence: 100 ms (configurable via `kpm_period_ms`).
Per UE message contains:
  - ewma_lte_mbps / ewma_nr_mbps (already maintained by dc_split EWMA thread)
  - lte_queue_bytes / nr_queue_bytes
  - target_split_ratio (current setpoint)
  - user_id (RNTI)

## Wiring
- nr_pdcp_ran_func.cpp: `kpm_publisher_thread` spawns from init_ran_function
- nr_pdcp_oai_api.c: `register_dc_user_with_ric(user)` called after DC_user added
- nr_pdcp_ran_func.h: register/unregister API exposed
- ZMQ DEALER → ROUTER (mock_ric.py @ :7878) → JSON parsed clean

## What's next (Stage 6b)
The user requirement: "채널 측정(MCS) + 사용가능한 RB로 BW 추정" — currently
we publish *measured EWMA* throughput. Need to add the *capacity* estimate:
  bw_est_mbps = bits_per_RE × 12 × 14 × 0.8 × RBs × layers × slots/sec × (1 - BLER)

Hook location: `gNB_scheduler_dlsch.c` near `sched_pdsch->mcs / ->rbSize`
assignment. Already declared `deliver_mcs_rb_to_ric()` in ran_func.h —
just need to call it from the scheduler with throttle to ~100ms cadence.
