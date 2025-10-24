#!/bin/bash
# Test script for mesh warp functionality
# This script sends keyboard input to toggle mesh mode and adjust points

set -e

VIDEO_FILE="${1:-h-6.mp4}"

if [ ! -f "$VIDEO_FILE" ]; then
    echo "Video file not found: $VIDEO_FILE"
    exit 1
fi

# Start the player in background
echo "Starting pickle with $VIDEO_FILE..."
timeout 20 ./pickle "$VIDEO_FILE" <<EOF &
PLAYER_PID=$!

# Give it time to start
sleep 2

# Press 'm' to toggle mesh warp mode
echo "Toggling mesh mode on..."
m

# Give it time to render
sleep 1

# Press '1' to select a mesh point
echo "Selecting mesh point 1..."
1

# Give it time to select
sleep 0.5

# Press arrow keys to move the point
echo "Moving point with arrow keys..."
Up Up Up Up Up
Right Right Right Right Right

# Wait to see the deformation
sleep 3

# Toggle mesh mode off
echo "Toggling mesh mode off..."
m

sleep 1

# Exit
echo "Exiting..."
q

EOF

# Wait for player to finish
wait $PLAYER_PID 2>/dev/null || true
echo "Test complete"
