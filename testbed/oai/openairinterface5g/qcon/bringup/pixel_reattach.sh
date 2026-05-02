#!/bin/bash
# Trigger Pixel re-attach via airplane mode toggle.
# Used after eNB/gNB launch to force registration.
ADB_SERIAL="${ADB_SERIAL:-2C121JEHN10216}"
adb -s "$ADB_SERIAL" shell settings put global airplane_mode_on 1
sleep 3
adb -s "$ADB_SERIAL" shell settings put global airplane_mode_on 0
sleep 2
echo "  airplane: $(adb -s "$ADB_SERIAL" shell settings get global airplane_mode_on)"
echo "  operator: $(adb -s "$ADB_SERIAL" shell getprop gsm.operator.numeric)"
echo "  sim state: $(adb -s "$ADB_SERIAL" shell getprop gsm.sim.state)"
echo "  data state: $(adb -s "$ADB_SERIAL" shell getprop gsm.data.network.type)"
