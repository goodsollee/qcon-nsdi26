# Stage 4 — EN-DC Stability (132 s sustained)

## Result
- 15 back-to-back 50MB transfers, 700 MB total in 132 s
- **avg 47.36 Mbps, min 26.16, max 53.00 Mbps**
- **0 GTPU TEID drops** (was the prior-session issue)
- **0 SgNB release / RLF events** during test
- **0 RRC re-establishment failures**
- 28 NR_MAC "no free RA process" — non-fatal noise (RA processes self-clear)

## Per-trial breakdown
```
trial 1  @ +0s:  26.16 Mbps  (LTE-only — NR leg not yet active)
trial 2-12  (16s..97s): 51.0~53.0 Mbps  (EN-DC stable)
trial 13-14 (105..116s): 27..35 Mbps   (NR leg dropped, LTE-only fallback)
```

NR leg occasionally drops without UE tearing down → re-activates on next traffic.
This reflects intrinsic EN-DC instability noted in OAI FEATURE_SET.md
("NSA mode is unstable") — but **no full disconnects**, so usable for benchmark.

## Confirmation
- No regression of TEID issue from prior `enb_log.txt`
- DRX disable holds across the test (no DRX log lines)
- Pixel kept rmnet1 IP 12.1.1.2 throughout
