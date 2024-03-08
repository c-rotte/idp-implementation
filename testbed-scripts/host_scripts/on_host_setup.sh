#!/bin/bash


busy_sleep() {
    local duration=$1
    local end_time=$(( $(date +%s) + duration ))
    while [ $(date +%s) -lt $end_time ]; do
        sleep 1
    done
}

busy_timeout() {
    local duration=$1
    local cmd=("${@:2}")
    local end_time=$(( $(date +%s) + duration ))
    # start the command in the background
    "${cmd[@]}" &
    local cmd_pid=$!
    while [ $(date +%s) -lt $end_time ] && kill -0 $cmd_pid 2>/dev/null; do
        sleep 1
    done
    # if the command is still running, kill it
    kill -0 $cmd_pid 2>/dev/null && kill -9 $cmd_pid
}


number=$1

# check if directory iperf-3.12 exists
if [ -d "iperf-3.12" ]; then
	echo Setup already done. Skipping...
	exit 0
fi

export DEBIAN_FRONTEND=noninteractive
apt update -yqq && apt upgrade -yqq
yes | apt install -yqq --force-yes iptables dnsutils wireguard wireguard-tools unzip libsodium-dev libdouble-conversion-dev net-tools linux-perf linux-cpupower ethtool tshark python3 python3-pip screen sysstat
echo "deb http://deb.debian.org/debian/ sid main" >> /etc/apt/sources.list
apt update -yqq
yes | apt -yqq -t unstable install liburing2
echo "deb http://deb.debian.org/debian/ experimental main" >> /etc/apt/sources.list
apt update -yqq
yes | apt -yqq -t experimental install libboost-all-dev  # boost 1.83
apt install -yqq libgoogle-glog-dev
apt install -yqq python3.9
update-alternatives --install /usr/bin/python python /usr/bin/python3.9 50
update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.9 50

# performance monitoring tools
yes | apt install -yqq atop linux-headers-"$(uname -r)" zlib1g-dev iotop nethogs iproute2 lsof
wget https://www.atoptool.nl/download/netatop-3.1.tar.gz
tar xvf netatop-3.1.tar.gz
rm netatop-3.1.tar.gz
cd netatop-3.1
make && make install
cd ..
modprobe -v netatop

# download a more recent iperf3 version allowing to bind to a network device
wget https://github.com/esnet/iperf/archive/refs/tags/3.12.tar.gz
tar xvf 3.12.tar.gz
rm 3.12.tar.gz
cd iperf-3.12
./configure && make && make install
cd ..

# install selenium + bind
mkdir -p ~/.bind
python3.11 -m pip install selenium
python3.11 -m pip install selenium
git clone https://github.com/JsBergbau/BindToInterface.git
gcc -nostartfiles -fpic -shared BindToInterface/bindToInterface.c -o ~/.bind/bind.so -ldl -D_GNU_SOURCE

# install chrome
wget "https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb"
apt install -yqq ./google-chrome-stable_current_amd64.deb

# install chromedriver
chromedriver_download_url="https://edgedl.me.gvt1.com/edgedl/chrome/chrome-for-testing/120.0.6099.109/linux64/chromedriver-linux64.zip"
wget "$chromedriver_download_url" -O ~/.bind/chromedriver.zip
unzip ~/.bind/chromedriver.zip -d ~/.bind/.  # chromedriver will be under ~/.bind/chromedriver-linux64/chromedriver
rm ~/.bind/chromedriver.zip

# move the www.tum.de clone to /var/www
cp -r ./resources/www.tum.de /var/www
# install static curl with http3 support (for testing)
wget https://github.com/stunnel/static-curl/releases/download/8.4.0/curl-static-amd64-8.4.0.tar.xz
tar xf curl-static-amd64-8.4.0.tar.xz
rm curl-static-amd64-8.4.0.tar.xz
chmod +x ./curl
# ./curl --http3-only -k https://127.0.0.1:1337/index.html

# incoming interface: eno3
# outgoing interface: eno4
function setup_interface() {
  name=$1
  ip=$2
  ip a add $ip/24 dev $name
  ip link set $name up
  ethtool -K $name tso on gso on gro on
}
eno3_num=$((number - 1))
eno4_num=$number
setup_interface eno3 4.0.$eno3_num.3
setup_interface eno4 4.0.$eno4_num.4
setup_interface eno5 5.0.0.$number
# setup tun device masquerading
iptables -t nat -A POSTROUTING -o eno4 -j MASQUERADE

# disable some security stuff for testing
sysctl net.ipv4.conf.all.accept_local=1
sysctl net.ipv4.ip_forward=1
apt install -yqq libiperf0

## settings for the hosts
# enable performance mode
cpupower frequency-set -g performance
# lower swappiness
sysctl vm.swappiness=10
# increase UDP receive buffer size
recv_original=212992
recv_size=$((recv_original * 64))
sysctl -w net.core.rmem_max=$recv_size
sysctl -w net.core.rmem_default=$recv_size
# increase UDP send buffer size
send_original=212992
send_size=$((send_original * 64))
sysctl -w net.core.wmem_max=$send_size
sysctl -w net.core.wmem_default=$send_size

# make sure all scripts are executable
find ./host_scripts/ -type f -name "*.sh" -exec chmod +x {} \;

# sync time

ptp_slave="-s"
#  check if eno5 has the ip address 5.0.0.1 assigned
#  --> if yes, then this is the master
if [[ $(ip -4 a s eno5 | grep inet) == *"5.0.0.1"* ]]; then
  ptp_slave=""
fi
ptp4l -i eno5 $ptp_slave &
phc2sys -s eno5 -w &  # keep phc2sys running in background

busy_sleep 30  # wait for ptp4l to sync

# wtf: sometimes, the first quic requests are not passed through the iface,
# except if we listen on the iface with tcpdump first
busy_timeout 30 tcpdump -i eno5 > /dev/null 2>&1
busy_timeout 30 tcpdump -i eno3 > /dev/null 2>&1
busy_timeout 30 tcpdump -i eno4 > /dev/null 2>&1