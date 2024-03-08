#!/bin/bash

port="$1"
cc="$2"
packet_size="$3"
threads="$4"
qlog_dir="$5" # may be empty

if [ -z "$qlog_dir" ]; then
  ./binaries_proxygen/hq-server \
      -mode server \
      -cert ./resources/cert.crt \
      -key ./resources/cert.key \
      -host 0.0.0.0 \
      -port $port \
      -httpversion "3" \
      -max_receive_packet_size "$packet_size" \
      -max_send_packet_size "$packet_size" \
      -outdir /tmp/www \
      -logdir /tmp/logs \
      -log_response true \
      -congestion "$cc" \
      -threads $threads
else
  ./binaries_proxygen/hq-server \
      -mode server \
      -cert ./resources/cert.crt \
      -key ./resources/cert.key \
      -host 0.0.0.0 \
      -port $port \
      -httpversion "3" \
      -max_receive_packet_size "$packet_size" \
      -max_send_packet_size "$packet_size" \
      -outdir /tmp/www \
      -logdir /tmp/logs \
      -log_response true \
      -qlogger_path "$qlog_dir" \
      -pretty_json false \
      -threads $threads \
      -congestion "$cc" 2>&1 | tee /tmp/http_server_qlog.log
fi
