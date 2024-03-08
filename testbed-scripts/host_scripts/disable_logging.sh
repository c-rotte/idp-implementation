#!/bin/bash

# for some weird reason, the kernel keeps writing this info log to /dev/kmsg:
# https://elixir.bootlin.com/linux/latest/source/drivers/net/tun.c#L1091
# the following lines are to disable it

# update package lists and install required packages
apt update -yqq
apt install -yqq sysctl procps psmisc
# stop and disable rsyslog
systemctl stop rsyslog
systemctl disable rsyslog
apt -yqq remove --purge rsyslog
# backup and disable systemd-journald logging
systemctl stop systemd-journald
mv /var/log/journal /var/log/journal.org || true
systemctl disable systemd-journald
# disable klogd if it exists
if systemctl is-active --quiet klogd; then
  systemctl stop klogd
  systemctl disable klogd
fi
# check for and disable other syslog variants
if systemctl is-active --quiet syslog-ng; then
  systemctl stop syslog-ng
  systemctl disable syslog-ng
fi
if systemctl is-active --quiet rsyslog; then
  systemctl stop rsyslog
  systemctl disable rsyslog
fi
# update kernel logging level
echo 1 > /proc/sys/kernel/printk
sysctl -w kernel.printk="1 1 1 1"
# disable dmesg for regular users
dmesg -n 1
# disable logging for other services that may log kernel messages
for service in service_name1 service_name2; do
  if systemctl is-active --quiet "$service"; then
    systemctl stop "$service"
    systemctl disable "$service"
  fi
done

echo "Kernel logging has been minimized."
