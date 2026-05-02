# QCON: QoE-driven multi-CONnectivity Scheduler

QCON is a QoE-driven multi-connectivity framework that maintains complete transparency to RTS applications. It directly leverages application QoE signals on RAN to perform packet scheduling over multi-connectivity, enhancing RTS application performance with efficient radio resource usage.

## Key Features

- **RTP Header Inspection**: Extracts QoE signals from unencrypted RTP headers
- **Frame Delivery Tracking**: Monitors frame delivery progress with millisecond-level precision
- **Dynamic Deadline Adjustment**: Prevents jitter-induced bitrate reductions in CCA
- **Efficient Backup Link Usage**: Minimizes backup link usage while maintaining QoE
- **Packet Forwarding**: Implements intelligent packet forwarding for congested links

## Architecture

QCON consists of three main components:

1. **QoE Processor**: Uses XDP/BPF to extract RTP headers and track frame delivery progress
2. **QCON Scheduler**: Makes decisions based on frame delivery progress and link states
3. **Packet Forwarding**: Moves packets from congested links to faster alternatives

## Requirements

- Clang & LLVM for XDP/BPF compilation
- libbpf-dev and libelf-dev for BPF functionality
- Root privileges for XDP attachment to interfaces

## Usage

To build and use QCON:

1. Run the setup script (requires root):
   ```bash
   sudo ./setup_qcon.sh
   ```

2. Start the RIC with QCON (requires root):
   ```bash
   sudo ./bin/ric_main config.json
   ```

## Configuration

The QCON scheduler can be configured through the scheduler section in `config.json`:

```json
"scheduler": {
  "enabled": true,
  "scheduler_name": "qcon",
  "enable_logging": true
}
```

## Implementation Details

- **RTP Header Extraction**: Uses XDP/BPF to efficiently extract RTP headers with minimal overhead
- **Frame Tracking**: Groups packets by their corresponding video frames using timestamp information
- **QoE Measurement**: Identifies buffer status and acknowledged bytes to accurately track packet transmission
- **Link Selection**: Chooses subsets of available links that can deliver frames on time with minimal backup link usage
- **Deadline Adjustment**: Dynamically adjusts deadlines to avoid jitter-induced bitrate reductions
- **Packet Forwarding**: Uses RAN's inherent functionality to forward packets from delayed buffers