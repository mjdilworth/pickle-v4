#!/bin/bash
# Test script to verify border toggle functionality
echo "Testing border toggle functionality..."
echo "This will start the video player, wait for video to stabilize, then toggle border"

cd /home/dilly/Projects/pickle-v4

# Start video player in background
(sleep 8; echo "b"; sleep 2; echo "b"; sleep 2; echo "q") | ./pickle /home/dilly/Projects/content/vid-a.mp4