#!/bin/bash
# Helper script to run pickle with proper permissions

VIDEO="$1"

if [ -z "$VIDEO" ]; then
    echo "Usage: $0 <video_file>"
    echo "Example: $0 video.mp4"
    exit 1
fi

# Check if we're root
if [ "$EUID" -eq 0 ]; then
    echo "Running as root..."
    exec ./pickle "$VIDEO"
fi

# Check if user is in video/render groups
IN_VIDEO=$(groups | grep -c video)
IN_RENDER=$(groups | grep -c render)

if [ "$IN_VIDEO" -eq 0 ] || [ "$IN_RENDER" -eq 0 ]; then
    echo "⚠️  User not in video/render groups"
    echo "Adding you to groups (requires password):"
    sudo usermod -a -G video,render $USER
    echo "✓ Groups added. Please logout and login for changes to take effect."
    echo ""
    echo "For now, running with sudo:"
    exec sudo ./pickle "$VIDEO"
fi

# Check if something is holding DRM
DRM_USERS=$(sudo lsof /dev/dri/card* 2>/dev/null | grep -v COMMAND | wc -l)

if [ "$DRM_USERS" -gt 0 ]; then
    echo "⚠️  Warning: Another process is using the display"
    sudo lsof /dev/dri/card* 2>/dev/null | head -10
    echo ""
    echo "Try stopping desktop manager:"
    echo "  sudo systemctl stop lightdm"
    echo ""
    read -p "Run with sudo anyway? (y/n) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        exec sudo ./pickle "$VIDEO"
    else
        exit 1
    fi
fi

# Try running normally
./pickle "$VIDEO"
EXIT_CODE=$?

# If it failed, try with sudo
if [ $EXIT_CODE -ne 0 ]; then
    echo ""
    echo "Failed to run. Trying with sudo..."
    exec sudo ./pickle "$VIDEO"
fi
