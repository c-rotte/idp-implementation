#!/bin/bash

URL=$1
TIMESTAMP=$2
NUM_TRANSACTIONS=$3
DL_DIR=$4
TUN_DEVICE=$5

if [ -n "$TUN_DEVICE" ] ; then
    for i in $(seq 0 $((NUM_TRANSACTIONS - 1))); do
        LOG_FILE="$DL_DIR/transaction_$i.LOGGING"
        python3.11 ./host_scripts/selenium_driver.py "$URL" "$TIMESTAMP" "$TUN_DEVICE" &> "$LOG_FILE" &
    done
else
    for i in $(seq 0 $((NUM_TRANSACTIONS - 1))); do
        TUN_DEVICE="tun_client_$((i * 4))"
        LOG_FILE="$DL_DIR/transaction_$i.LOGGING"
        python3.11 ./host_scripts/selenium_driver.py "$URL" "$TIMESTAMP" "$TUN_DEVICE" &> "$LOG_FILE" &
    done
fi

wait
