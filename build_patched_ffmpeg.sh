#!/bin/bash
# Build Patched FFmpeg for RPi4 Zero-Copy GPU Rendering
# This enables DRM_PRIME output from V4L2 M2M decoder

set -e

echo "=================================================="
echo "Building Patched FFmpeg for RPi4 Zero-Copy"
echo "=================================================="

# Source directory
FFMPEG_DIR="/home/dilly/Projects/Video/patch/rpi-ffmpeg"
INSTALL_PREFIX="/usr/local"

if [ ! -d "$FFMPEG_DIR" ]; then
    echo "ERROR: FFmpeg source not found at $FFMPEG_DIR"
    exit 1
fi

cd "$FFMPEG_DIR"

echo ""
echo "Step 1: Clean previous build..."
make distclean 2>/dev/null || true

echo ""
echo "Step 2: Configure FFmpeg for RPi4 with DRM_PRIME support..."
./configure \
    --prefix=$INSTALL_PREFIX \
    --extra-cflags="-I$INSTALL_PREFIX/include" \
    --extra-ldflags="-L$INSTALL_PREFIX/lib" \
    --extra-libs='-lpthread -lm -latomic' \
    --arch=aarch64 \
    --cpu=cortex-a72 \
    --target-os=linux \
    --enable-pic \
    --enable-gpl \
    --enable-nonfree \
    --enable-version3 \
    --enable-pthreads \
    --enable-libdrm \
    --enable-v4l2-m2m \
    --disable-mmal \
    --enable-libx264 \
    --enable-libx265 \
    --enable-shared \
    --enable-libopus \
    --enable-libvorbis \
    --disable-ffplay \
    --disable-ffprobe \
    --enable-debug=no \
    --enable-optimizations

echo ""
echo "Step 3: Build FFmpeg (this may take 10-30 minutes)..."
make -j4

echo ""
echo "Step 4: Install to $INSTALL_PREFIX..."
sudo make install

echo ""
echo "Step 5: Verify installation..."
$INSTALL_PREFIX/bin/ffmpeg -decoders 2>/dev/null | grep v4l2m2m
echo ""
$INSTALL_PREFIX/bin/ffmpeg -version 2>&1 | head -2

echo ""
echo "=================================================="
echo "✅ FFmpeg build complete!"
echo "=================================================="
echo ""
echo "To use the new FFmpeg:"
echo "  export LD_LIBRARY_PATH=/usr/local/lib:\$LD_LIBRARY_PATH"
echo "  cd /home/dilly/Projects/pickle-v4"
echo "  make clean && make"
echo "  ./pickle test_video.mp4"
echo ""
echo "Look for:"
echo "  [FORMAT CALLBACK INVOKED]"
echo "  [ZERO-COPY] ✓✓✓ DRM PRIME frame detected!"
echo "  [RENDER_DETAIL] Video0 - TexUpload: <1ms"
echo ""
