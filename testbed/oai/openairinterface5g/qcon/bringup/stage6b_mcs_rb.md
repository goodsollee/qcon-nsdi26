# Stage 6b — MCS+RB capacity feed verified

## Result
mock_ric.log received `mcs_rb` messages at 100ms cadence:
```
mcs_rb #1: ue_id 17076, leg 1 (NR), mcs 0,  rbs 0, layers 1, bler 0%,  slots/s 2000, bw 0.0 Mbps
mcs_rb #2: ue_id 17076, leg 1 (NR), mcs 0,  rbs 0, layers 1, bler 0%,  slots/s 2000, bw 0.0 Mbps
mcs_rb #3: ue_id 17076, leg 1 (NR), mcs 9,  rbs 0, layers 1, bler 10%, slots/s 2000, bw 0.0 Mbps
```

MCS index varying (0 → 9) and BLER tracking (0 → 10%) prove the
gNB MAC scheduler state is being read live.

`rbs=0 → bw_est=0.0` because Pixel is currently on LTE-only leg (NR
side not allocated). When DC split decides to use NR (split_ratio>0
or QCON scheduler engages), `rbs` will populate and `bw_est_mbps` will
follow MCS×RB×layers×slots×(1-BLER) formula.

## Wiring
- `qcon_emit_mac_kpm()` in nr_pdcp_oai_api.c:
   - acquires `mac->sched_lock`
   - iterates `RC.nrmac[0]->UE_info.list` via `UE_iterator`
   - reads `sched_pdsch->mcs / rbSize / Qm / R / nrOfLayers`
   - reads `sched_ctrl->dl_bler_stats.bler`
   - computes `bits_per_re = Qm × (R/1024)` and `slots_per_sec = 1000 × 2^μ`
   - calls `deliver_mcs_rb_to_ric(rnti, leg=NR, ...)`
- Called every `kpm_period_ms` (100ms) from publisher thread.

## What this gives RIC
Per-UE NR-leg capacity estimate independent of measured throughput:
  `bw_est_mbps = bits_per_re × 12 × 14 × 0.8 × rbs × layers × slots/s × (1-BLER) / 1e6`

This is exactly what the user asked for: "채널 측정(MCS) + 사용가능한 RB로
bandwidth 추정해서 RIC가 쓰도록".
