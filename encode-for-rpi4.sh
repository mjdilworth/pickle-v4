#!/bin/bash
# Encode video for RPi4 hardware decode
# Profile: High 4.2, 60fps, 1920x1080
# No B-frames, no AUD packets, 11 Mbps bitrate

INPUT="${1:-HalloweenPickle2.mp4}"
OUTPUT="${INPUT%.*}-rpi4.mp4"

echo "Encoding $INPUT -> $OUTPUT"
echo "Settings: High Profile 4.2, 60fps, 1920x1080, 11 Mbps, no B-frames, no AUD"
echo ""

ffmpeg -i "$INPUT" \
  -c:v libx264 \
  -profile:v high \
  -level 4.2 \
  -b_strategy 0 \
  -bf 0 \
  -refs 4 \
  -preset slow \
  -x264-params "vui=0:aud=0" \
  -b:v 11M \
  -r 60 \
  -pix_fmt yuv420p \
  -c:a aac \
  -b:a 128k \
  "$OUTPUT"

echo ""
echo "✓ Encoding complete: $OUTPUT"
echo ""
echo "Video info:"
ffprobe -select_streams v:0 -show_entries "stream=profile,level,width,height,r_frame_rate,bit_rate" -of compact=nokey=1:noprint_wrappers=1 "$OUTPUT" 2>/dev/null
