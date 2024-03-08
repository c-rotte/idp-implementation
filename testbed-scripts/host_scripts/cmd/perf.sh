#!/bin/bash

cmd="$1"
path="$2"

eval "perf record -F 99 -g -o $path -- $cmd"
