# RIC-RAN Communication Message Format

This document describes the message formats used for communication between RIC and RAN components.

## Overview

All messages use JSON format for serialization with the following general structure:

```json
{
    "type": "<message_type>",
    "component_id": "<source_component_id>",
    "timestamp": <unix_timestamp_ms>,
    ... additional fields specific to message type ...
}
```

## Message Types

### 1. KPM Metrics Message

Used by RAN components to report performance metrics to the RIC.

**Type**: `kpm_metrics`

**Fields**:
- `component_id`: ID of the reporting component
- `metric_type`: Type of metric being reported
- `metric_value`: Value of the metric
- `timestamp`: Unix timestamp in milliseconds

**Example**:
```json
{
    "type": "kpm_metrics",
    "component_id": "du-001",
    "metric_type": "prb_usage",
    "metric_value": "87.5",
    "timestamp": 1648756234123
}
```

### 2. RC Command Message

Used by RIC to send control commands to RAN components.

**Type**: `rc_command`

**Fields**:
- `component_id`: ID of the target component
- `command_type`: Type of command
- `command_params`: Parameters for the command as a string
- `timestamp`: Unix timestamp in milliseconds

**Example**:
```json
{
    "type": "rc_command",
    "component_id": "du-001",
    "command_type": "update_scheduling_policy",
    "command_params": "policy=proportional_fair,weight=0.75",
    "timestamp": 1648756240567
}
```

### 3. RC Response Message

Used by RAN components to respond to RC commands from the RIC.

**Type**: `rc_response`

**Fields**:
- `component_id`: ID of the responding component
- `response_code`: Status code (e.g., "SUCCESS", "ERROR")
- `response_data`: Additional data or error information
- `timestamp`: Unix timestamp in milliseconds

**Example**:
```json
{
    "type": "rc_response",
    "component_id": "du-001",
    "response_code": "SUCCESS",
    "response_data": "Scheduling policy updated successfully",
    "timestamp": 1648756240789
}
```

## Common Metric Types

The following metric types are supported in KPM messages:

### PHY Layer Metrics
- `rsrp`: Reference Signal Received Power (dBm)
- `rsrq`: Reference Signal Received Quality (dB)
- `sinr`: Signal to Interference plus Noise Ratio (dB)
- `bler_percent`: Block Error Rate (percentage)

### MAC Layer Metrics
- `prb_usage`: Physical Resource Block usage (percentage)
- `scheduler_throughput`: Scheduler throughput (Mbps)
- `mac_pdus_tx`: Number of MAC PDUs transmitted
- `mac_pdus_rx`: Number of MAC PDUs received
- `harq_retransmissions`: Number of HARQ retransmissions

### RLC Layer Metrics
- `rlc_buffer_occupancy`: RLC buffer occupancy (bytes)
- `rlc_pdus_tx`: Number of RLC PDUs transmitted
- `rlc_pdus_rx`: Number of RLC PDUs received
- `rlc_discard_rate`: RLC discard rate (percentage)

### PDCP Layer Metrics
- `pdcp_throughput_ul`: PDCP uplink throughput (Mbps)
- `pdcp_throughput_dl`: PDCP downlink throughput (Mbps)
- `pdcp_latency`: PDCP layer latency (ms)
- `pdcp_packet_loss`: PDCP packet loss rate (percentage)

## Common RC Command Types

The following RC command types are supported:

### Resource Management Commands
- `update_scheduling_policy`: Update the scheduling policy
- `adjust_prb_allocation`: Adjust PRB allocation for a UE
- `set_transmission_power`: Set transmission power levels

### Mobility Management Commands
- `prepare_handover`: Prepare for handover
- `execute_handover`: Execute handover
- `cancel_handover`: Cancel pending handover

### Multi-Connectivity Commands
- `activate_link`: Activate a backup link
- `deactivate_link`: Deactivate a link
- `modify_link_priority`: Modify the priority of a link
- `set_duplication_mode`: Set packet duplication mode
- `update_splitting_ratio`: Update the packet splitting ratio between links