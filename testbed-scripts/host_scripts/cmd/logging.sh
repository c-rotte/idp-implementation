#!/bin/bash

cmd="$1"
path="$2"

rm -f "$path"
eval "$cmd" > "$path" 2>&1