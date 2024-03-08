#!/bin/bash
set -x

iface="$1"
oface="$2"

sysctl net.ipv4.ip_forward=1
# sysctl net.ipv6.ip_forward=1
iptables -t nat -A POSTROUTING -o "$oface" -j MASQUERADE
iptables -A FORWARD -i "$iface" -o "$oface" -j ACCEPT
iptables -A FORWARD -i "$iface" -j ACCEPT
iptables -A FORWARD -o "$iface" -j ACCEPT
