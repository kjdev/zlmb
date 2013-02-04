#!/bin/env python
#
# Example worker exec
#
# Usage: exp_worker_exec.py
#
# zlmb-worker -e tcp://127.0.0.1:5560 -c exp_worker_exec.py

import sys
import os
import datetime

# Init zlmb
zlmb_frame = ''
zlmb_frame_length = ''
zlmb_length = ''
zlmb_buffer = ''

# Read STDIN (Get zlmb buffer)
if sys.stdin.isatty() == False:
    zlmb_buffer = sys.stdin.read()

# Get zlmb environ
zlmb_frame = int(os.environ.get('ZLMB_FRAME', 0))
zlmb_frame_length = os.environ.get('ZLMB_FRAME_LENGTH')
zlmb_length = int(os.environ.get('ZLMB_LENGTH', 0))

# Output
print os.path.basename(__file__), ":", datetime.datetime.today().strftime("%Y-%m-%d %H:%M:%S")
print "ZLMB_FRAME:%s" % zlmb_frame
print "ZLMB_FRAME_LENGTH:%s" % zlmb_frame_length
print "ZLMB_LENGTH:%s" % zlmb_length
print "ZLMB_BUFFER:%s" % zlmb_buffer

# Parse zlmb frame buffer
zlmb_frames = []
if zlmb_frame_length > 0:
    i = 0
    j = 0
    lines = str(zlmb_frame_length).split(':')
    for val in lines:
        j += int(val)
        buf = zlmb_buffer[i:j]
        i = j
        zlmb_frames.append(buf)

print zlmb_frames

print "----------"
