#!/bin/sh
#
# Example worker exec
#
# Usage: exp_worker_exec.sh
#
# zlmb-worker -e tcp://127.0.0.1:5560 -c exp_worker_exec.sh

# Read STDIN (Get zlmb buffer)
if [ ! -t 0 ]; then
    ZLMB_BUFFER=`cat -`
fi

DATE=`date '+%Y-%m-%d %H:%M:%S'`

# Output
echo "${0##*/}: ${DATE}"
echo "ZLMB_FRAME:${ZLMB_FRAME}"
echo "ZLMB_FRAME_LENGTH:${ZLMB_FRAME_LENGTH}"
echo "ZLMB_LENGTH:${ZLMB_LENGTH}"
echo "ZLMB_BUFFER:${ZLMB_BUFFER}"
echo "----------"
