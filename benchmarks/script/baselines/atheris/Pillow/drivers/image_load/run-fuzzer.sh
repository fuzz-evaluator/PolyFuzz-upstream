#!/bin/sh


python $BENCH/script/popen_log.py "python image_load.py ./tests -rss_limit_mb=4096" &


