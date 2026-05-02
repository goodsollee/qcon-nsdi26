# testbed

Real-radio EN-DC bring-up so QCON's QoE-aware multi-connectivity
scheduler runs over commodity Android handsets. OAI eNB + gNB →
Magma 4G EPC → UE. The OAI fork in `oai/openairinterface5g/` is
patched with the QCON KPM publisher (per-leg `mcs_rb`, EWMA, queue)
and control-plane hooks (`split_ratio`, `reinject`) inside the gNB
PDCP entity.

## Hardware used in our setup

- 1× host PC, Ubuntu 22.04, 20+ cores, 64GB RAM
- 2× Ettus USRP B210 (USB 3.0)
  - one bound to eNB (band 7 FDD, 2.68 GHz)
  - one bound to gNB (band n78 TDD, 3.5 GHz)
- 1× external 10 MHz reference (clock_src = "external" in confs)
- 1× EN-DC capable Android phone with programmable SIM
  (we used Pixel 7a + sysmocom programmable SIM)

Replace USRP serials and SIM keys with your own (see env vars below
and the `magma/` README).

## Software environment

- Ubuntu 22.04 LTS (jammy)
- UHD 4.7+ (`apt install uhd-host libuhd-dev`); UHD images at
  `/usr/share/uhd/images`
- Docker + docker-compose
- OAI build deps (jsoncpp, libzmq, sctp, libnettle, libgnutls28, …):
  `oai/openairinterface5g/cmake_targets/build_oai -I` covers them
- Magma MME demo images (`docker pull rdefosseoai/magma-mme:latest`,
  plus `oaisoftwarealliance/oai-{hss,spgwc,spgwu-tiny}:latest`)

## Network layout

| Iface / IP | Role |
|---|---|
| `enp58s0:1` 192.168.61.1/24 | eNB S1 + S1U (host) |
| `enp58s0:2` 10.53.1.3/24 | gNB X2 (host) |
| 192.168.61.149 (docker) | Magma MME (S1AP) |
| 192.168.61.133 (docker) | SPGWU SGi tun0 |
| 192.168.61.130 (docker) | OAI HSS |
| 12.1.1.0/24 | UE pool (rmnet1 on phone) |

## Build OAI

```bash
cd oai/openairinterface5g/cmake_targets
sudo ./build_oai -I                 # one-time deps install
sudo ./build_oai --eNB --gNB -w USRP
```
Binaries land in `cmake_targets/ran_build/build/{lte-softmodem,nr-softmodem}`.

## Run

Order matters: **Magma → eNB → gNB → UE attach**.

```bash
# 1. Magma EPC
cd ../magma
sudo docker-compose up -d cassandra db_init
sudo docker logs demo-db-init -f          # wait for "OK", then:
sudo docker rm -f demo-db-init
sudo docker-compose up -d                 # brings up the rest

# 2. host network aliases
sudo ip addr add 192.168.61.1/24 dev enp58s0
sudo ip addr add 10.53.1.3/24    dev enp58s0
sudo ip route add 12.1.1.0/24 via 192.168.61.133

# 3. eNB
export USRP_SERIAL_LTE=<your_b210_serial_for_eNB>
cd ../scripts && sudo -E ./enb_start.sh

# 4. gNB (in another shell, after eNB is up)
export USRP_SERIAL_NR=<your_b210_serial_for_gNB>
sudo -E ./gnb_start.sh

# 5. RIC
python3 ../../ric/mock_ric.py
# (or ric_main/bin/ric_main --config ric_main/config.json)

# 6. UE — toggle airplane mode; verify rmnet1 gets 12.1.1.x
adb shell settings put global airplane_mode_on 1
sleep 5
adb shell settings put global airplane_mode_on 0
```

## Microbench

```bash
cd scripts
RATIOS="0.0 0.5 1.0" SEG=15 RATE_MBPS=10 ./bench_split_ratio.sh
./bench_reinject.sh
```

## Conf customisation

`conf/enb.nsa.band7.25prb.usrpb200.conf` — LTE anchor (band 7, 25 PRB,
FDD).
`conf/gnb.nsa.band78.106prb.usrpb200.conf` — NR secondary (n78, 51 PRB
DL effective, TDD).

PLMN is `001/01` (test). Change to your assigned PLMN before
production. SIM IMSI / Ki / OPc must match `magma/docker-compose.yml`
HSS env (see `magma/README.md`).
