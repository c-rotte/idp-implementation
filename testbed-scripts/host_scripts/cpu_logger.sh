#!/bin/bash

screen_name="$1"
node_output_path="$2"

screen -dmS "$screen_name" bash -c "mpstat -P ALL 1 > ${node_output_path}/uptime.log"

echo "Started cpu logger in screen session $screen_name"
