#!/bin/bash

node_output_path="$1"

num_files=$(find "$node_output_path" -type f -name "*.PERF" | wc -l)
if [ "$num_files" -eq 0 ]; then
  echo "No PERF files found. Skipping..."
  sleep 1
  exit 0
fi

# convert to flamegraph format
find "$node_output_path" -type f -name "*.PERF" -exec bash -c '{
  path="$1"
  perf script -i "$path" | ./host_scripts/stackcollapse-perf.pl > "$path".folded
  ./host_scripts/flamegraph.pl "$path".folded > "$path".svg
}' _ {} \;

sleep 1