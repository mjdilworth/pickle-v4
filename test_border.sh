#!/bin/bash
# Test border toggle functionality
echo "Starting video player and testing border toggle..."
cd /home/dilly/Projects/pickle-v4
echo "Press 'b' to toggle border after 3 seconds"
(
  sleep 3
  echo "b" > /dev/tty
  echo "Toggled border"
  sleep 5
  echo "q" > /dev/tty
) &
./pickle /home/dilly/Projects/content/vid-a.mp4