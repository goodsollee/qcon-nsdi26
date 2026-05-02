#!/bin/bash

# Get input
read -p "Enter the UE index: " number

if ! [[ "$number" =~ ^[0-9]+$ ]]; then
    echo "Please enter a valid integer number."
    exit 1
fi

# Construct the commands

netns_command="sudo ./multi-ue.sh -c${number} -e"
ue_command="sudo -E ./nr-uesoftmodem  -r 106 --numerology 1 --band 78 -C 3619200000 --rfsim --sa -O ../../../exec_config/ue.conf --telnetsrv  --parallel-config PARALLEL_SINGLE_THREAD --rfsimulator.serveraddr 10.20${number}.1.100 --uicc0.imsi 00101000000010${number}"
#ue_command="sudo -E ./nr-uesoftmodem  -r 106 --numerology 1 --band 78 -C 3619200000 --rfsim --sa -O ../../../exec_config/ue.conf --telnetsrv  --parallel-config PARALLEL_SINGLE_THREAD --rfsimulator.serveraddr 10.20${number}.1.100 --uicc0.imsi 001010000000001"



cd ../exec_dir
$netns_command<< EOL
cd ../cmake_targets/ran_build/build
$ue_command
exit
EOL