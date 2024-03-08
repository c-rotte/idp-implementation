#!/bin/sh

export DEBIAN_FRONTEND=noninteractive
apt update && apt upgrade
apt install -yqq \
    git \
    cmake \
    m4 \
    g++ \
    flex \
    bison \
    libgflags-dev \
    libkrb5-dev \
    libsasl2-dev \
    libnuma-dev \
    pkg-config \
    libssl-dev \
    libcap-dev \
    gperf \
    libevent-dev \
    libtool \
    libjemalloc-dev \
    libsnappy-dev \
    wget \
    unzip \
    libiberty-dev \
    liblz4-dev \
    liblzma-dev \
    make \
    zlib1g-dev \
    binutils-dev \
    libsodium-dev \
    libdouble-conversion-dev \
    libpcap-dev

echo "deb http://deb.debian.org/debian/ experimental main" >> /etc/apt/sources.list
echo "deb http://deb.debian.org/debian/ sid main" >> /etc/apt/sources.list
apt update -yqq
apt -yqq purge libboost-all-dev
yes | apt -yqq -t experimental install -f libboost-all-dev  # boost 1.83
apt install -yqq libgoogle-glog-dev
apt install -yqq python3.9
update-alternatives --install /usr/bin/python python /usr/bin/python3.9 50
update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.9 50