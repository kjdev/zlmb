#!/bin/env python
#
# Example worker
#
# Usage: exp_worker.py ENDPOINT MESSAGE ...
#
# % python exp_worker.py tcp://127.0.0.1:5560

import sys
import os
import zmq

argv = sys.argv
argc = len(argv)

if argc <= 1:
    print "Usage: python %s ENDPOINT" % os.path.basename(__file__)
    exit(1);

endpoint = argv[1]

context = zmq.Context()
socket = context.socket(zmq.PULL)
socket.connect(endpoint)

interrupt = False
while True:
    msg = []
    more = True
    while more:
        try:
            msg.append(socket.recv())
            more = socket.getsockopt(zmq.RCVMORE)
        except:
            interrupt = True
            break
    if interrupt == True:
        break
    print msg
