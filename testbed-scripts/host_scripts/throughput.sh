#!/bin/bash
cd $(dirname $0)

node_output_path="$1"

num_files=$(find "$node_output_path" -type f -name "*.LOGGING" | wc -l)
if [ "$num_files" -eq 0 ]; then
  echo "No LOGGING files found. Skipping..."
  sleep 1
  exit 0
fi

python3 throughput.py --dir "$node_output_path" >> "$node_output_path"/result.csv

sleep 1