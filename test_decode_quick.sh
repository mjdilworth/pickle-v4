#!/bin/bash
# Quick frame decode test without interactive input
export SDL_VIDEODRIVER=dummy
timeout 3 ./pickle ../content/rpi4-e.mp4 < /dev/null 2>&1 | grep -E "Successfully decoded|Frame.*YUV|Video pointers|Error|Failed"
