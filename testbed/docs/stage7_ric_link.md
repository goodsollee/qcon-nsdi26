# Stage 7 — RIC ZMQ link end-to-end verified

## Result — proof
mock_ric.log captured:
```
[mock-ric] listening on :7878 (ROUTER)
[mock-ric] ready #1: {"header": "ready", "payload": "{\"build\":\"qcon-bringup\",\"pid\":3266454}\n",
                      "sn": 0, "timestamp": 1777658878231573743, "ue_id": 0}
```

## What's wired
- `nr_pdcp_ran_func.{h,cpp}` registered in CMakeLists (Stage 7a)
- `init_ran_function()` called from `nr_pdcp_layer_init()` in oai_api.c (Stage 7b)
- ZMQ DEALER → ROUTER (mock RIC) at tcp://127.0.0.1:7878 connects
- recv_msg thread alive — handles split_ratio / reinject / drx_config commands from RIC
- Ready heartbeat sent at startup (Stage 7d) — confirms wire end-to-end

## Symbols in nr-softmodem (`nm`)
```
T deliver_info_to_ric
T deliver_mcs_rb_to_ric    ← Stage 6 BW publisher (built but not yet called)
T init_ran_function
T send_msg
T update_split_ratio
```

## Next: Stage 6 — feed MCS+RB BW estimate
Hook `deliver_mcs_rb_to_ric` from gNB MAC scheduler:
- Source of truth: `gNB_scheduler_dlsch.c` per-UE allocation (sched_pdsch->mcs, ->rbSize)
- Throttle to ~100ms cadence (every Nth slot)
- Compute bw_est_mbps = bits_per_RE × 12 × 14 × 0.8 × RBs × layers × slots/sec × (1-BLER)
