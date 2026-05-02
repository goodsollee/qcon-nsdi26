# emulator

User-space PDCP + multi-link bandwidth emulator. Replays per-link
bandwidth traces and runs QCON's QoE-aware multi-connectivity
scheduler on real packet flows — no SDR, no SIM. Useful for
fast, repeatable scheduler experiments on a single Linux host.

## Environment

- Linux ≥ 5.x with namespaces / TUN
- g++ 11+ (C++17)
- `libjsoncpp-dev`, `libzmq3-dev`, `libpcap-dev`
- `iproute2`, `iptables`, `tc`

```bash
sudo apt install g++ make libjsoncpp-dev libzmq3-dev libpcap-dev iproute2
```

## Build

```bash
cd mc_emulator        && make && cd ..
cd pdcp_emulator      && make && cd ..
cd network_emulation  && make && cd ..
cd enhanced_tun_emulator && make && cd ..
```

## Run

```bash
cd mc_emulator
sudo bash setup_network.sh
./bin/pdcp_emulator_manager -c config.json pdcp_manager &
./bin/emulator <bandwidth_trace.csv> emulator &
# drive a workload inside app_ns:
sudo ip netns exec app_ns iperf3 -c <host_ip> -u -b 20M -t 30
```

Trace CSV format: `time_ms,link0_kbps,link1_kbps`. See sample
`additional_used.csv`, `driving_traces.csv`.

## Teardown

```bash
sudo pkill -f pdcp_emulator_manager
sudo pkill -f emulator
sudo ip netns del app_ns
sudo ip tuntap del mode tun dev tun_host
```
