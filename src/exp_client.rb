#!/bin/env ruby
#
# Example client
#
# Usage: exp_client.rb ENDPOINT MESSAGE ...
#
# % ruby exp_client.rb tcp://127.0.0.1:5557 message
# % ruby exp_client.rb tcp://127.0.0.1:5557 message1 message2 message3

require 'zmq'

if ARGV.length <= 1 then
  print "Usage: ruby ", File.basename(__FILE__), " ENDPOINT MESSAGE ...\n"
  exit
end

endpoint = ARGV[0]

context = ZMQ::Context.new(1)
socket = context.socket(ZMQ::PUSH)
socket.connect(endpoint)

last = ARGV.length - 1
i = 1

while i != last
  socket.send(ARGV[i].to_s, ZMQ::SNDMORE)
  i = i + 1
end

socket.send(ARGV[last].to_s)

socket.close()
context.close()
