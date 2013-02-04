#!/bin/env ruby
#
# Example worker
#
# Usage: exp_worker.rb ENDPOINT MESSAGE ...
#
# % ruby exp_worker.rb tcp://127.0.0.1:5560

require 'zmq'

if ARGV.length <= 0 then
  print "Usage: ruby ", File.basename(__FILE__), " ENDPOINT\n"
  exit
end

endpoint = ARGV[0]

context = ZMQ::Context.new(1)
socket = context.socket(ZMQ::PULL)
socket.connect(endpoint)

interrupt = FALSE
while TRUE
  msg = []
  loop do
    begin
      msg.push(socket.recv())
      more = socket.getsockopt(ZMQ::RCVMORE)
      break unless more
    rescue Exception
      interrupt = TRUE
      break
    end
  end
  if interrupt == TRUE then
    break
  end
  p msg
end

socket.close()
context.close()
