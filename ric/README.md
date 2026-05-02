# ric

QCON's QoE-aware multi-connectivity scheduler. Runs as a RAN
Intelligent Controller side-car: ingests KPM (`kpm`, `mcs_rb`) from
the data plane every 100 ms, infers per-application QoE, and pushes
back control (`split_ratio`, `reinject`) per UE. Two flavors:

- `ric_main/` — production C++ controller (per-frame scheduler).
- `mock_ric.py` — Python protocol prototype.

Both speak the same wire protocol (ZMQ ROUTER on `tcp://0.0.0.0:7878`,
JSON messages).

## Environment

- g++ 11+ (C++17)
- `libzmq3-dev`, `libjsoncpp-dev`, `libspdlog-dev`
- Python 3.10+, `pyzmq` (for `mock_ric.py`)

```bash
sudo apt install g++ make libzmq3-dev libjsoncpp-dev libspdlog-dev
pip install pyzmq
```

## Build (C++ controller)

```bash
cd ric_main
make           # → bin/ric_main
```

## Run

```bash
# Python prototype (drop-in for testing without rebuilding):
python3 mock_ric.py

# Production C++ controller:
./ric_main/bin/ric_main --config ric_main/config.json
```

## CLI

Send a control command:

```bash
python3 ric_cli.py split_ratio <user_id> 0.5
python3 ric_cli.py reinject    <user_id> <hex_payload>
```

## Wire protocol

| header | direction | payload fields |
|---|---|---|
| `ready`  | data → RIC | `build`, `node`, `pid` |
| `kpm`    | data → RIC | `user_id`, `ewma_lte_mbps`, `ewma_nr_mbps`, `lte_pkts`, `nr_pkts`, `lte_queue_bytes`, `nr_queue_bytes`, `target_split_ratio` |
| `mcs_rb` | data → RIC | `leg` (0=LTE, 1=NR), `mcs`, `rbs`, `layers`, `bler_pct`, `bw_est_mbps`, `slots_per_sec` |
| `split_ratio` | RIC → data | `user_id`, `ratio` (0=all NR, 1=all LTE) |
| `reinject`    | RIC → data | `user_id`, `rb_id`, `packet_data` (base64) |

## Logs

`ric_main` writes to `$QCON_LOG_DIR` (defaults `./logs/`). Git-ignored.
