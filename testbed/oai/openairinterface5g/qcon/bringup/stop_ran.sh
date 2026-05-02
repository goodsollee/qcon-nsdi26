#!/bin/bash
# Stop eNB + gNB cleanly
sudo pkill -INT lte-softmodem 2>/dev/null
sudo pkill -INT nr-softmodem 2>/dev/null
sleep 2
sudo pkill -KILL lte-softmodem 2>/dev/null
sudo pkill -KILL nr-softmodem 2>/dev/null
echo "stopped"
