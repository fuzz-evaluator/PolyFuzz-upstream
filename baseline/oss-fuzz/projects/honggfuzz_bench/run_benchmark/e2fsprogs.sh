#!/bin/sh

cd /root/honggfuzz_bench/cyclonedds/

nohup sh run.sh > full.log 2>&1 &

python3 extract_log.py 1800 $0
