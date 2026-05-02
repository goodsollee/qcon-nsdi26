#!/bin/bash

# Script to run a series of multi-user tests with different user counts
# and generate plots for all results

set -e  # Exit on any error

# Directory setup
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

# Default values
DURATION=60
CONFIG_FILE="$SCRIPT_DIR/../config.json"
OUTPUT_DIR="$SCRIPT_DIR/results_$(date +%Y%m%d_%H%M%S)"

# Default user counts to test
USER_COUNTS=(5 10 20 50 100)

# Parse command line arguments
while [[ $# -gt 0 ]]; do
  case $1 in
    -d|--duration)
      DURATION="$2"
      shift 2
      ;;
    -c|--config)
      CONFIG_FILE="$2"
      shift 2
      ;;
    -o|--output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    -u|--users)
      # Parse comma-separated list of user counts
      IFS=',' read -ra USER_COUNTS <<< "$2"
      shift 2
      ;;
    -h|--help)
      echo "Usage: $0 [options]"
      echo "Options:"
      echo "  -d, --duration SEC      Test duration in seconds for each run (default: 60)"
      echo "  -c, --config PATH       Path to config file (default: ../config.json)"
      echo "  -o, --output-dir DIR    Output directory for results (default: ./results_TIMESTAMP)"
      echo "  -u, --users LIST        Comma-separated list of user counts (default: 5,10,20,50,100)"
      echo "  -h, --help              Show this help message"
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      exit 1
      ;;
  esac
done

# Create output directory
mkdir -p "$OUTPUT_DIR"
echo "Results will be saved to: $OUTPUT_DIR"

# Run tests for each user count
for USERS in "${USER_COUNTS[@]}"; do
  echo "========================================================"
  echo "Running test with $USERS users for $DURATION seconds..."
  echo "========================================================"
  
  # Simple direct call to run_multi_user_test.sh
  "$SCRIPT_DIR/run_multi_user_test.sh" --num-users "$USERS" --duration "$DURATION" --config "$CONFIG_FILE"
  
  # Find the most recent result file (should be the one we just created)
  RESULT_FILE=$(ls -t multi_user_test_"$USERS"users_*.csv | head -1)
  
  if [ -f "$RESULT_FILE" ]; then
    # Copy to output directory
    cp "$RESULT_FILE" "$OUTPUT_DIR/"
    
    # Generate plot
    "$SCRIPT_DIR/plot_multi_user_results.py" "$RESULT_FILE"
    
    # If plot was generated, copy it too
    PLOT_FILE="${RESULT_FILE%.csv}.png"
    if [ -f "$PLOT_FILE" ]; then
      cp "$PLOT_FILE" "$OUTPUT_DIR/"
    fi
  else
    echo "Warning: No result file found for $USERS users"
  fi
  
  # Small delay between tests to ensure system stabilizes
  sleep 5
done

# Generate summary file
SUMMARY_FILE="$OUTPUT_DIR/summary.txt"
echo "Multi-User Scalability Test Summary" > "$SUMMARY_FILE"
echo "=================================" >> "$SUMMARY_FILE"
echo "Date: $(date)" >> "$SUMMARY_FILE"
echo "Duration per test: $DURATION seconds" >> "$SUMMARY_FILE"
echo "" >> "$SUMMARY_FILE"
echo "User Counts: ${USER_COUNTS[*]}" >> "$SUMMARY_FILE"
echo "" >> "$SUMMARY_FILE"
echo "CPU Usage Summary:" >> "$SUMMARY_FILE"
echo "----------------" >> "$SUMMARY_FILE"

for USERS in "${USER_COUNTS[@]}"; do
  RESULT_FILE="$OUTPUT_DIR/multi_user_test_${USERS}users_"*.csv
  if [ -f "$RESULT_FILE" ]; then
    # Calculate average CPU and memory using awk
    CPU_AVG=$(awk -F, 'NR>1 {total+=$3; count++} END {print total/count}' "$RESULT_FILE")
    MEM_AVG=$(awk -F, 'NR>1 {total+=$4/1024; count++} END {print total/count}' "$RESULT_FILE")
    
    echo "$USERS users: CPU avg=${CPU_AVG:.2f}%, Memory avg=${MEM_AVG:.2f} MB" >> "$SUMMARY_FILE"
  else
    echo "$USERS users: No data available" >> "$SUMMARY_FILE"
  fi
done

echo "" >> "$SUMMARY_FILE"
echo "All test results and plots are saved in: $OUTPUT_DIR" >> "$SUMMARY_FILE"

echo "Tests completed! Summary available in $SUMMARY_FILE"
echo "All results saved to: $OUTPUT_DIR"