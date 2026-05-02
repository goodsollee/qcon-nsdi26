#!/bin/bash

# Change to the desired directory
cd ../../cmake_targets/ran_build/build

sudo ip addr add 192.168.61.1/24 dev enp58s0

# Execute eNB
#sudo ./lte-softmodem -O ../../../exec_config/enb_dc.conf --usrp-tx-thread-config 1 --RUs.[0].sdr_addrs "type=b200,serial=${USRP_SERIAL_LTE:?Set USRP_SERIAL_LTE in your environment}"  --RUs.[0].clock_src "external"  --continuous-tx  -E #--gNBs.[0].min_rxtxtime 2 --continuous-tx 

sudo ./lte-softmodem -O ../../../ci-scripts/conf_files/enb.nsa.band7.25prb.usrpb200.conf --usrp-tx-thread-config 1 --RUs.[0].sdr_addrs "type=b200,serial=${USRP_SERIAL_LTE:?Set USRP_SERIAL_LTE in your environment}"  --RUs.[0].clock_src "external"  --continuous-tx  -E #--T_stdout 2 #--gNBs.[0].min_rxtxtime 2 --continuous-tx 

# Execute gNB
#sudo ./nr-softmodem --sa -O ../../../exec_config/gnb.sa.band41.fr1.106PRB.usrpb210.conf --usrp-tx-thread-config 1 --RUs.[0].sdr_addrs type=x300  --RUs.[0].clock_src "external"  --gNBs.[0].min_rxtxtime 2 --continuous-tx 
#sudo ./nr-softmodem --sa -O ../../../exec_config/gnb_usrp_n78.conf --usrp-tx-thread-config 1 --RUs.[0].sdr_addrs type=b200  --RUs.[0].clock_src "extenral" --gNBs.[0].min_rxtxtime 2 --continuous-tx -E
#sudo ./nr-softmodem --sa -O ../../../exec_config/gnb_open5gs.conf --usrp-tx-thread-config 1 --RUs.[0].sdr_addrs type=x300  --RUs.[0].clock_src "external" --gNBs.[0].min_rxtxtime 2 --continuous-tx -E
#sudo ./nr-softmodem --sa -O ../../../exec_config/gnb.sa.band66.fr1.24PRB.usrpx300.conf --usrp-tx-thread-config 1 --RUs.[0].sdr_addrs type=x300  --RUs.[0].clock_src "internal" --gNBs.[0].min_rxtxtime 0

#--gNBs.[0].min_rxtxtime 1