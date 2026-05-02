#!/bin/bash

# Change to the desired directory
cd ../cmake_targets/ran_build/build

# Execute gNB
sudo ./nr-softmodem RFSIMULATOR=server -O ../../../exec_config/gnb.sa.band78.fr1.106PRB.usrpb210.conf --gNBs.[0].min_rxtxtime 6 --rfsim --sa
#sudo ./nr-softmodem RFSIMULATOR=server -O ../../../exec_config/gnb_sim_n41.conf --telnetsrv --parallel-config PARALLEL_SINGLE_THREAD --gNBs.[0].min_rxtxtime 6 --rfsim --sa --rfsimulator.options chanmod