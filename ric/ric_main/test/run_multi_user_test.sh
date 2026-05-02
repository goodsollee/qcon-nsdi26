#!/bin/bash

# Script to compile and run the multi-user test for RIC

set -e  # Exit on any error

# Directory setup
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
RIC_DIR=$(dirname "$SCRIPT_DIR")
BUILD_DIR="$RIC_DIR/build"

# Ensure directories exist
mkdir -p "$BUILD_DIR"

# Default values
NUM_USERS=5
DURATION=30
CONFIG_FILE="$RIC_DIR/config.json"

# Parse command line arguments
while [[ $# -gt 0 ]]; do
  case $1 in
    -n|--num-users)
      NUM_USERS="$2"
      shift 2
      ;;
    -d|--duration)
      DURATION="$2"
      shift 2
      ;;
    -c|--config)
      CONFIG_FILE="$2"
      shift 2
      ;;
    -h|--help)
      echo "Usage: $0 [options]"
      echo "Options:"
      echo "  -n, --num-users NUM     Number of users for the test (default: 5)"
      echo "  -d, --duration SEC      Test duration in seconds (default: 30)"
      echo "  -c, --config PATH       Path to config file (default: ric/config.json)"
      echo "  -h, --help              Show this help message"
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      exit 1
      ;;
  esac
done

echo "Building multi_user_test..."
cd "$BUILD_DIR"

# Simple direct compilation of the standalone test
g++ -o multi_user_test \
  "$SCRIPT_DIR/multi_user_test.cpp" \
  -std=c++17 -pthread -ljsoncpp

echo "Running multi-user test with $NUM_USERS users for $DURATION seconds..."
"$BUILD_DIR/multi_user_test" --num-users "$NUM_USERS" --duration "$DURATION" --config "$CONFIG_FILE"

echo "Test completed. Check the output CSV file for results."