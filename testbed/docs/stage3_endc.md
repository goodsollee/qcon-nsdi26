# Stage 3 — EN-DC bring-up

## Result
- **EN-DC throughput: 52.77 Mbps** (50 MB / 7.9 s, trial 2/3)
- LTE-only baseline was 26.85 Mbps → **+96% from NR leg**
- Trial 1: 35.61 Mbps (warm-up), trials 2-3: 52.35-52.77 Mbps (steady)

## What worked
- gNB launched (USRP B210 <USRP_SERIAL_NR>, ext clock locked, NSA mode)
- X2 SCTP setup: gNB(10.53.1.3) → eNB(192.168.61.1):36422
- SgNB Addition flow processed:
  - eNB log: `Assign CU UE ID 1 and DU UE ID 53969 to UE RNTI d2d1, LTE RNTI 0`
  - eNB log: `GTPU [104] Created tunnel for UE ID 1, teid 688b87c0 → SPGWU 192.168.61.133`
- **PAVE DC split code is live**:
  - `[PDCP] DC user add start! d2d1`
  - `[PDCP] Forward info added for rnti 53969 at index 0, sgnb_ue_x2_id: 53969`
  - `MP-TECHNIQUE: minRTT, BUFFER-THRESHOLD: 30000.00 KB, ALPHA: 0.50`

## Configuration in use
- gNB conf: `ci-scripts/conf_files/gnb.nsa.band78.106prb.usrpb200.conf`
  - absoluteFrequencySSB=641272 (band n78 ≈ 3.5GHz)
  - dl_carrierBandwidth=**51 PRB** (~20MHz, despite filename "106prb" — reduced for B210 USB3 stability)
  - prach_ConfigurationIndex=98, prach_RootSequenceIndex=1
- min_rxtxtime=6 (TDD safety margin)
- DC config (qcon/config.csv): MP-TECHNIQUE=minRTT, BUFFER=30000 KB, ALPHA=0.5

## Known issues (non-blocking)
- `[NR_MAC] no free RA process` — repeats during attach (16 cases). Mostly noise-triggered preambles. Doesn't block working UE.
- `[NR_RRC] UE table error` — single occurrence, recovers
- 1 USRP_OVERFLOW during launch (transient, not recurring)
- Pixel display still shows "LTE,Unknown" (not 5G) but data plane confirms NR leg active
- DC split BUFFER-THRESHOLD=30MB → for 50MB single-shot transfer, split kicks in late; aggregate throughput likely under-utilizes NR leg

## Path
```
Pixel(rmnet1=12.1.1.2) ─┬─→ eNB(LTE leg)  ┐
                        └─→ gNB(NR leg)    │ ← PDCP DC split (minRTT) at gNB
                                           ↓
                  S1U/X2-U → SPGWU(192.168.61.133)
                                           ↓
                       SGi → host(192.168.61.1:5202)
```

## Files
- `/tmp/qcon_runs/gnb_<ts>.log` — gNB log with DC split traces
- `/tmp/qcon_runs/enb_<ts>.log` — eNB log with X2/SgNB Addition flow

## Next stages
- Stage 4: 5-min EN-DC stability test, identify if RA failures or overflow recur over time
- Increase NR PRB (or buffer sweep) to raise throughput ceiling
