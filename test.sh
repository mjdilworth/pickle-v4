#!/bin/bash

# Simple test script for the video player

echo "RPi4 Video Player Test Script"
echo "=============================="

# Check if we have a test video file
if [ ! -f "test_video.mp4" ]; then
    echo "No test_video.mp4 found. Creating a simple test pattern..."
    
    # Check if ffmpeg is available to create a test video
    if command -v ffmpeg >/dev/null 2>&1; then
        echo "Creating 10-second test pattern with ffmpeg..."
        ffmpeg -f lavfi -i testsrc2=duration=10:size=1920x1080:rate=30 -c:v libx264 -preset fast test_video.mp4 2>/dev/null
        if [ $? -eq 0 ]; then
            echo "✓ Created test_video.mp4 (10 seconds, 1920x1080, 30fps)"
        else
            echo "✗ Failed to create test video"
            exit 1
        fi
    else
        echo "✗ ffmpeg not found. Please install ffmpeg or provide a test_video.mp4 file"
        echo "  Install: sudo apt install ffmpeg"
        exit 1
    fi
fi

echo ""
echo "Test video ready: test_video.mp4"
echo "File size: $(du -h test_video.mp4 | cut -f1)"
echo ""

# Check if running with proper privileges
if [ "$EUID" -ne 0 ]; then
    echo "⚠️  Not running as root - DRM/KMS access will fail"
    echo ""
    echo "To test the video player with hardware acceleration:"
    echo "  sudo ./pickel test_video.mp4"
    echo ""
    echo "Current user groups: $(groups)"
    echo ""
    echo "To add user to video group (logout/login required):"
    echo "  sudo usermod -a -G video \$USER"
    echo ""
else
    echo "✓ Running as root - hardware access should work"
    echo ""
fi

# Show system info
echo "System Information:"
echo "  Kernel: $(uname -r)"
echo "  Architecture: $(uname -m)"
echo "  GPU Memory Split: $(vcgencmd get_mem gpu 2>/dev/null || echo 'N/A (vcgencmd not found)')"
echo ""

# Check for DRM devices
echo "Available DRM devices:"
ls -la /dev/dri/ 2>/dev/null || echo "  No /dev/dri/ directory found"
echo ""

# Check for input devices
echo "Available input devices:"
ls -la /dev/input/event* 2>/dev/null | head -5 || echo "  No input event devices found"
echo ""

# Run the application
echo "Starting video player..."
echo "Controls: q=quit, 1-4=select corners, arrows=move corner, r=reset"
echo "=========================================================================="
exec ./pickel test_video.mp4