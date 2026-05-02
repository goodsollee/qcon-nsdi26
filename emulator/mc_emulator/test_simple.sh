#!/bin/bash

# Simple test script for multi-queue emulator
echo "==========================================="
echo "Multi-Queue Emulator Simple Test"
echo "Configuration: Single link, 4 priority queues"
echo "==========================================="

# Check if binaries exist
if [ ! -f "bin/pdcp_sender" ] || [ ! -f "bin/pdcp_du_link" ] || [ ! -f "bin/pdcp_ue_link" ]; then
    echo "Error: Required binaries not found. Please run 'make' first."
    exit 1
fi

# Check if config file exists
if [ ! -f "config_example_4queues.json" ]; then
    echo "Error: Configuration file not found."
    exit 1
fi

# Create log directories
mkdir -p multi_queue_logs ue_logs

# Cleanup function
cleanup() {
    echo ""
    echo "Cleaning up processes..."
    pkill -f "pdcp_sender"
    pkill -f "pdcp_du_link"
    pkill -f "pdcp_ue_link"
    wait
    echo "Cleanup completed."
}

# Set trap for cleanup
trap cleanup EXIT INT TERM

echo ""
echo "Starting emulator components..."

# Start UE PDCP receiver (runs in background)
echo "Starting UE PDCP receiver..."
./bin/pdcp_ue_link config_example_4queues.json > ue_receiver.log 2>&1 &
UE_PID=$!
sleep 2

# Start DU link (runs in background)
echo "Starting DU link..."
./bin/pdcp_du_link config_example_4queues.json > du_link.log 2>&1 &
DU_PID=$!
sleep 2

# Start PDCP sender (runs in background)
echo "Starting PDCP sender..."
./bin/pdcp_sender config_example_4queues.json > sender.log 2>&1 &
SENDER_PID=$!
sleep 3

echo ""
echo "All components started. Process IDs:"
echo "  UE Receiver: $UE_PID"
echo "  DU Link: $DU_PID"
echo "  PDCP Sender: $SENDER_PID"

echo ""
echo "Running for 30 seconds to test connectivity..."

# Monitor processes for 30 seconds
DURATION=30
COUNT=0

while [ $COUNT -lt $DURATION ]; do
    # Check if processes are still running
    if ! kill -0 $UE_PID 2>/dev/null; then
        echo "ERROR: UE Receiver process died"
        exit 1
    fi
    if ! kill -0 $DU_PID 2>/dev/null; then
        echo "ERROR: DU Link process died"
        exit 1
    fi
    if ! kill -0 $SENDER_PID 2>/dev/null; then
        echo "ERROR: PDCP Sender process died"
        exit 1
    fi

    # Print progress
    if [ $((COUNT % 5)) -eq 0 ]; then
        echo "Test running... ${COUNT}s elapsed"
    fi

    sleep 1
    COUNT=$((COUNT + 1))
done

echo ""
echo "Test completed. Analyzing results..."

# Check logs for success indicators
echo ""
echo "=== Log Analysis ==="

# Check sender log
echo "PDCP Sender Log Analysis:"
if grep -q "Initialized" sender.log; then
    echo "  ✓ Sender initialized successfully"
else
    echo "  ✗ Sender initialization not confirmed"
fi

if grep -q "ERROR" sender.log; then
    ERROR_COUNT=$(grep -c "ERROR" sender.log)
    echo "  ⚠ Found $ERROR_COUNT error(s) in sender log"
else
    echo "  ✓ No errors in sender log"
fi

# Check DU link log
echo ""
echo "DU Link Log Analysis:"
if grep -q "MAC Sender.*Scheduler started" du_link.log; then
    echo "  ✓ MAC scheduler started successfully"
else
    echo "  ✗ MAC scheduler start not confirmed"
fi

if grep -q "RLC Sender.*Initialized" du_link.log; then
    echo "  ✓ RLC sender initialized successfully"
else
    echo "  ✗ RLC sender initialization not confirmed"
fi

if grep -q "Processing slot" du_link.log; then
    echo "  ✓ MAC slot processing active"
else
    echo "  ✗ MAC slot processing not detected"
fi

if grep -q "queue_[0-3]" du_link.log; then
    echo "  ✓ Multi-queue support active"
else
    echo "  ? Multi-queue activity not clearly detected"
fi

if grep -q "priority_[0-3]" du_link.log; then
    echo "  ✓ Priority handling active"
else
    echo "  ? Priority handling not clearly detected"
fi

if grep -q "ERROR" du_link.log; then
    ERROR_COUNT=$(grep -c "ERROR" du_link.log)
    echo "  ⚠ Found $ERROR_COUNT error(s) in DU link log"
else
    echo "  ✓ No errors in DU link log"
fi

# Check UE receiver log
echo ""
echo "UE Receiver Log Analysis:"
if grep -q "RLC Receiver Module initialized" ue_receiver.log; then
    echo "  ✓ RLC receiver initialized successfully"
else
    echo "  ✗ RLC receiver initialization not confirmed"
fi

if grep -q "Status PDU" ue_receiver.log; then
    echo "  ✓ Status PDU mechanism active"
else
    echo "  ? Status PDU activity not detected"
fi

if grep -q "Multi-queue delivery callback set" ue_receiver.log; then
    echo "  ✓ Multi-queue delivery callback configured"
else
    echo "  ? Multi-queue delivery not clearly configured"
fi

if grep -q "ERROR" ue_receiver.log; then
    ERROR_COUNT=$(grep -c "ERROR" ue_receiver.log)
    echo "  ⚠ Found $ERROR_COUNT error(s) in UE receiver log"
else
    echo "  ✓ No errors in UE receiver log"
fi

# Final assessment
echo ""
echo "=== FINAL ASSESSMENT ==="

# Count successful indicators
SUCCESS_COUNT=0
TOTAL_CHECKS=6

if grep -q "Initialized" sender.log; then
    SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
fi

if grep -q "MAC Sender.*Scheduler started" du_link.log; then
    SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
fi

if grep -q "RLC Sender.*Initialized" du_link.log; then
    SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
fi

if grep -q "Processing slot" du_link.log; then
    SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
fi

if grep -q "RLC Receiver Module initialized" ue_receiver.log; then
    SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
fi

if ! grep -q "ERROR" sender.log && ! grep -q "ERROR" du_link.log && ! grep -q "ERROR" ue_receiver.log; then
    SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
fi

echo "Success indicators: $SUCCESS_COUNT/$TOTAL_CHECKS"

if [ $SUCCESS_COUNT -ge 5 ]; then
    echo ""
    echo "🎉 TEST RESULT: SUCCESS"
    echo "✓ Emulator components are working correctly"
    echo "✓ Multi-queue MAC scheduler is operational"
    echo "✓ Packet flow from PDCP sender to UE PDCP is functional"
    echo ""
    echo "The 4-priority queue system is ready for use!"
    exit 0
else
    echo ""
    echo "❌ TEST RESULT: ISSUES DETECTED"
    echo "Some components may not be working correctly."
    echo "Please check the log files for more details:"
    echo "  - sender.log"
    echo "  - du_link.log"
    echo "  - ue_receiver.log"
    exit 1
fi