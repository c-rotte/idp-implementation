#!/bin/bash

# Check if the script is run as root
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root"
   exit 1
fi

# Check if two arguments are provided
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <internet_interface> <tun_interface>"
    exit 1
fi

INET_IFACE="$1"
TUN_IFACE="$2"

iptables -P INPUT ACCEPT
iptables -P FORWARD ACCEPT
iptables -P OUTPUT ACCEPT

# Enable IP Forwarding
sysctl -w net.ipv4.ip_forward=1

iptables -A FORWARD -i $TUN_IFACE -j ACCEPT
iptables -A FORWARD -o $TUN_IFACE -j ACCEPT

# Setup NAT
iptables -t nat -A POSTROUTING -o $INET_IFACE -j MASQUERADE

echo "NAT setup complete for $TUN_IFACE to access the internet via $INET_IFACE."
