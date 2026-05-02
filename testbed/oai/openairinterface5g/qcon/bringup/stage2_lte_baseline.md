# Stage 2 — LTE-only baseline (DRX disabled)

## Result
- **Throughput: 26.85 Mbps** (50 MB / 14.825 s, TCP receiver = Pixel)
- 50 PRB FDD band 7 ≈ 75% of theoretical 36 Mbps (single-layer SISO, no MIMO)
- DRX disabled in eNB conf → no MAC sleep cycles

## Path
```
Pixel(rmnet1=12.1.1.2) → eNB(USRP B210, ext clock) → S1U GTP-U → SPGWU(192.168.61.133)
   → SGi (host route 12.1.1.0/24 via 192.168.61.133) → host(192.168.61.1:5202)
```

## Configuration changes
- `ci-scripts/conf_files/enb.nsa.band7.25prb.usrpb200.conf:101`
  `drx_Config_present = "prSetup"` → `"prRelease"` (DRX disabled per QCON paper req)

## Network setup applied
- `sudo ip addr add 192.168.61.1/24 dev enp58s0` (link can be DOWN — host-only IP for SCTP bind)
- `sudo ip route add 12.1.1.0/24 via 192.168.61.133` (UE pool reachable through SPGWU)
- CPU governor = performance, socket buffers 32MB

## Pixel side
- IMSI 001010000000008 (HSS pre-populated)
- Operator name "SRS RAN" (from carrier broadcast, ignore)
- gsm.network.type = LTE
- Re-attach via `adb shell settings put global airplane_mode_on 1; sleep 3; ...0` works without root

## Throughput tooling (no iperf3 on Pixel)
- Host: python TCP server at `:5202` pushes 50 MB on connect (`/tmp/qcon_runs/tcpsrv.py`)
- Pixel: `/system/bin/nc -w 30 ... | dd of=/dev/null bs=65536`
- `dd` reports bytes + duration → throughput

## Files
- `/tmp/qcon_runs/enb_<ts>.log` — eNB log
- `/tmp/qcon_runs/tcpsrv.py` — TCP throughput server
- `/tmp/qcon_runs/srv/test_50M.bin` — 50 MB random payload

## Issues observed (not blocking)
- `[MAC] non active DTCH ... dropping packet` early after attach — clears after first Pixel airplane toggle
- Multiple stale RNTIs in log from prior failed attaches — eNB recovers cleanly
