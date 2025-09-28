#!/bin/bash
# Reset keystone settings to default values
echo "Resetting keystone settings to default..."

cd /home/dilly/Projects/pickle-v4

# Remove corrupted keystone file
rm -f pickle_keystone.conf

echo "Keystone settings reset. The video player will use default settings on next run."
echo "Default corners: Top-Left(-1,-1), Top-Right(1,-1), Bottom-Right(1,1), Bottom-Left(-1,1)"