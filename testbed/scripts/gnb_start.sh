#!/bin/bash

# Change to the desired directory
cd ../../cmake_targets/ran_build/build

for ((i=0;i<$(nproc);i++)); do sudo cpufreq-set -c $i -r -g performance; done
sudo sysctl -w net.core.rmem_max=33554432
sudo sysctl -w net.core.wmem_max=33554432
sudo sysctl -w net.core.rmem_default=33554432
sudo sysctl -w net.core.wmem_default=33554432

sudo ip addr add 10.53.1.3/24 dev enp58s0

# Execute gNB
#sudo ./nr-softmodem --sa -O ../../../exec_config/gnb.sa.band41.fr1.106PRB.usrpb210.conf --usrp-tx-thread-config 1 --RUs.[0].sdr_addrs "type=b200,serial=${USRP_SERIAL_NR:?Set USRP_SERIAL_NR in your environment}"  --RUs.[0].clock_src "external"  --gNBs.[0].min_rxtxtime 6 --continuous-tx -E  --T_stdout 2
#sudo ./nr-softmodem --sa -O ../../../exec_config/gnb.sa.band78.fr1.106PRB.usrpb210.conf --usrp-tx-thread-config 1 --RUs.[0].sdr_addrs "type=b200,serial=${USRP_SERIAL_NR:?Set USRP_SERIAL_NR in your environment}"  --RUs.[0].clock_src "external" --gNBs.[0].min_rxtxtime 6 --continuous-tx -E 
#sudo ./nr-softmodem -O ../../../exec_config/gnb_dc.conf --usrp-tx-thread-config 1 --RUs.[0].sdr_addrs "type=b200,serial=${USRP_SERIAL_NR:?Set USRP_SERIAL_NR in your environment}" --RUs.[0].clock_src "external" --gNBs.[0].min_rxtxtime 6 --continuous-tx -E 
sudo ./nr-softmodem  -O ../../../ci-scripts/conf_files/gnb.nsa.band78.106prb.usrpb200.conf --usrp-tx-thread-config 1 --RUs.[0].sdr_addrs "type=b200,serial=${USRP_SERIAL_NR:?Set USRP_SERIAL_NR in your environment}" --RUs.[0].clock_src "external" --gNBs.[0].min_rxtxtime 6 --continuous-tx -E  

#sudo ./nr-softmodem --sa -O ../../../exec_config/gnb_open5gs.conf --usrp-tx-thread-config 1 --RUs.[0].sdr_addrs type=x300  --RUs.[0].clock_src "external" --gNBs.[0].min_rxtxtime 2 --continuous-tx -E

#sudo ./nr-softmodem --sa -O ../../../exec_config/gnb.sa.band66.fr1.24PRB.usrpx300.conf --usrp-tx-thread-config 1 --RUs.[0].sdr_addrs "type=b200,serial=${USRP_SERIAL_NR:?Set USRP_SERIAL_NR in your environment}"  --RUs.[0].clock_src "external" --gNBs.[0].min_rxtxtime 0
#--gNBs.[0].min_rxtxtime 1



