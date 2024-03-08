#!/bin/bash

target_time=$1

current_time=$(date +%s)
sleep_seconds=$(echo "$target_time - $current_time" | bc)

if (( $(echo "$sleep_seconds < 0" | bc -l) )); then
    echo "The specified target time is in the past."
    exit 0
fi

sleep $sleep_seconds