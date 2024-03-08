#!/bin/bash

kill_process() {
  local process_name=$1
  local signal=${2:-TERM}
  killall -$signal "$process_name"
  while pidof "$process_name" >/dev/null; do
    sleep 1
  done
}


honor_qlog="$1"
if [ "$honor_qlog" = "true" ]; then
  qlog_processes=("masque-server" "masque-http-client")
  for process in "${qlog_processes[@]}"; do
    echo "Killing process using SIGTERM: $process"
    kill_process "$process" 15
  done
  # Wait for hq-server to flush qlog
  if pidof "hq-server" >/dev/null; then
    echo "Waiting for hq-server to flush qlog..."
    while true; do
      if grep -q "Flushed qlog to /tmp/output/http_server" /tmp/output/http_server.LOGGING_QLOG; then
        echo "hq-server has flushed qlog. Killing it now."
        kill_process "hq-server" 9
        break
      elif grep -q "Peer closed with error" /tmp/output/http_server.LOGGING_QLOG; then
        echo "hq-server has closed with error. Killing it now."
        kill_process "hq-server" 15
        break
      fi
      sleep 1
    done
  fi
else
  echo "Skipping qlog handling."
fi

# murder the remaining
processes=("masque-http-client" "hq-server" "python3.11" "masque-client" "masque-server" "iperf3" "quiche-server")
for process in "${processes[@]}"; do
    echo "Killing process using SIGKILL: $process"
    kill_process "$process" 9
done

# remove the temporary directories
rm -rf /tmp/www
rm -rf /tmp/tun_devices_ids

processes=("chrome" "chromedriver" "google-chrome")
for process in "${processes[@]}"; do
    echo "Killing process using SIGKILL: $process"
    kill_process "$process" 9
done
# remove chrome cache
rm -rf ~/.cache/google-chrome
rm -rf ~/.config/google-chrome