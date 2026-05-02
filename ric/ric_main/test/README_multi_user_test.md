# Multi-User Testing for QCON RIC

This directory contains tools for testing the QCON RIC with multiple simulated users to evaluate system resource usage and performance.

## Overview

The multi-user test creates simulated RTP flows and dummy KPM metrics for a specified number of users to measure:

1. CPU usage as the number of users increases
2. Memory consumption as the number of users increases
3. Scheduling performance with multiple concurrent users

## Usage

You can run the multi-user test using the provided script:

```bash
./run_multi_user_test.sh [options]
```

### Options

- `-n, --num-users NUM`: Number of users to simulate (default: 5)
- `-d, --duration SEC`: Duration of the test in seconds (default: 30)
- `-c, --config PATH`: Path to the RIC config file (default: ric/config.json)
- `-h, --help`: Show help message

## Output

The test generates a CSV file with CPU and memory measurements over time. The filename format is:

```
multi_user_test_<NUM>users_<TIMESTAMP>.csv
```

### CSV Format

The CSV file contains the following columns:

- `timestamp`: Time of the measurement
- `num_users`: Number of users in the test
- `cpu_percent`: CPU usage percentage
- `memory_kb`: Memory usage in kilobytes

## Visualizing Results

A Python script is included to visualize the test results:

```bash
./plot_multi_user_results.py multi_user_test_<NUM>users_<TIMESTAMP>.csv
```

This will generate a PNG image with CPU and memory usage graphs, including mean and maximum values.

## Running Multiple Tests

To compare system performance with different numbers of users, you can run a series of tests:

```bash
# Run tests with increasing user count
for users in 5 10 20 50 100; do
  ./run_multi_user_test.sh --num-users $users --duration 60
done

# Plot all results
for csv in multi_user_test_*.csv; do
  ./plot_multi_user_results.py $csv
done
```

## Example

To run a test with 10 users for 60 seconds:

```bash
./run_multi_user_test.sh --num-users 10 --duration 60
```

## How It Works

The test:

1. Creates a specified number of simulated users
2. Generates RTP packets that mimic video traffic for each user
3. Creates dummy KPM metrics for PDCP and RLC layers
4. Measures system resource usage during the test duration
5. Outputs results to a CSV file for analysis

## Interpreting Results

Results can be used to:

- Determine how RIC performance scales with user count
- Identify CPU or memory bottlenecks
- Optimize scheduler settings for multi-user scenarios
- Plan hardware requirements for specific deployment scenarios

### Performance Analysis

When analyzing the results, consider:

1. **Linear Scaling**: Ideally, CPU and memory usage should scale linearly with user count
2. **Bottlenecks**: Watch for sudden increases in resource usage, which may indicate bottlenecks
3. **Memory Leaks**: If memory usage constantly increases over time, there might be memory leaks
4. **System Limits**: Determine the maximum user count your system can handle before resource exhaustion