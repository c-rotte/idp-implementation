#!/bin/bash

SERVER_ADDRESS=$1
TIMESTAMP=$2
STARTING_PORT=$3
NUM_TRANSACTIONS=$4
DL_DIR=$5

for i in $(seq 0 $((NUM_TRANSACTIONS - 1))); do
    TUN_DEVICE="tun_client_$((i * 4))"
    LOG_FILE="$DL_DIR/transaction_$i.LOGGING"
    { sleep $((TIMESTAMP - $(date +%s))); iperf3 --bind-dev $TUN_DEVICE -t 20 -c $SERVER_ADDRESS -p $((STARTING_PORT + i)) -f Kbits &> "$LOG_FILE"; } &
done

wait
