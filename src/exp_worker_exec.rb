#!/bin/env ruby
#
# Example worker exec
#
# Usage: exp_worker_exec.rb
#
# zlmb-worker -e tcp://127.0.0.1:5560 -c exp_worker_exec.rb

# Init zlmb
zlmb_frame = ''
zlmb_frame_length = ''
zlmb_length = ''
zlmb_buffer = ''

# Read STDIN (Get zlmb buffer)
if ! STDIN.tty?
  zlmb_buffer = STDIN.read
end

# Get zlmb environ
zlmb_frame = ENV['ZLMB_FRAME']
zlmb_frame_length = ENV['ZLMB_FRAME_LENGTH']
zlmb_length = ENV['ZLMB_LENGTH']

# Output
print File.basename(__FILE__), ":", Time.now.strftime("%Y-%m-%d %H:%M:%S"), "\n"
print "ZLMB_FRAME:", zlmb_frame.to_s, "\n"
print "ZLMB_FRAME_LENGTH:", zlmb_frame_length.to_s, "\n"
print "ZLMB_LENGTH:", zlmb_length.to_s, "\n"
print "ZLMB_BUFFER:", zlmb_buffer.to_s, "\n"

# Parse zlmb frame buffer
zlmb_frames = []
if zlmb_frame_length.to_i > 0
  i = 0
  lines = zlmb_frame_length.to_s.split(':')
  for val in lines
    buf = zlmb_buffer.to_s[i, val.to_i]
    i += val.to_i
    zlmb_frames.push(buf)
  end
end

p zlmb_frames

print "----------\n"
