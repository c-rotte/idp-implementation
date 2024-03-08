#!/bin/sh

openssl req -newkey rsa:4096 -x509 \
            -sha256 \
            -days 3650 \
            -nodes \
            -out /tmp/cert.crt \
            -keyout /tmp/cert.key \
            -batch
ls /tmp/cert.key
ls /tmp/cert.crt
