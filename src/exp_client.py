#!/bin/env python
#
# Example client
#
# Usage: exp_client.py ENDPOINT MESSAGE ...
#
# % python exp_client.py tcp://127.0.0.1:5557 message
# % python exp_client.py tcp://127.0.0.1:5557 message1 message2 message3

import sys
import os
import zmq

argv = sys.argv
argc = len(argv)

if argc <= 2:
    print "Usage: python %s ENDPOINT MESSAGE ..." % os.path.basename(__file__)
    exit(1)

endpoint = argv[1]

context = zmq.Context()
socket = context.socket(zmq.PUSH)
socket.connect(endpoint)

last = argc - 1
i = 2

while i != last:
    socket.send(str(argv[i]), zmq.SNDMORE)
    i = i + 1

socket.send(str(argv[last]))
