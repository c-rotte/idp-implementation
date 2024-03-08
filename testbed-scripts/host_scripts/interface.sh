#!/bin/bash
ip address add $1 dev $2
ip link set $2 up

# disable tcp segmentation offload, generic segmentation offload and generic receive offload
# ethtool -K $2 tso off gso off gro off
