# Hardware Decoder Troubleshooting

## Current Issue

Your video playback is experiencing performance problems because the hardware decoder (V4L2 M2M) is failing to produce frames, causing a fallback to software decoding.

### Symptoms
- Hardware decoder processes 9+ packets without producing a frame
- Falls back to software decoding
- Slow decode times: 38-62ms per frame (target is 16.7ms for 60fps)
- Video playback is stuttering

### Log Analysis
```
[h264_v4l2m2m @ 0x55ae2155f0] Using device /dev/video10
[h264_v4l2m2m @ 0x55ae2155f0] driver 'bcm2835-codec' on card 'bcm2835-codec-decode'
...
Warning: Hardware decoder processed 9 packets without getting a frame
Attempting fallback to software decoding...
Successfully switched to software decoding
```

## Possible Causes

1. **Firmware/Driver Issues**
   - Outdated Raspberry Pi firmware
   - V4L2 driver problems with bcm2835-codec

2. **Video Format Compatibility**
   - The video might use H.264 features not well-supported by hardware decoder
   - Profile 77 (High Profile) might have compatibility issues

3. **Buffer/Memory Configuration**
   - Insufficient GPU memory allocation
   - V4L2 buffer configuration issues

## Solutions to Try

### 1. Update Raspberry Pi Firmware
```bash
sudo apt update
sudo apt full-upgrade
sudo reboot
```

### 2. Increase GPU Memory
Edit `/boot/config.txt` (or `/boot/firmware/config.txt` on newer systems):
```bash
sudo nano /boot/config.txt
```

Add or modify:
```
gpu_mem=256
```

Reboot:
```bash
sudo reboot
```

### 3. Check V4L2 Devices
```bash
# List available V4L2 devices
v4l2-ctl --list-devices

# Check capabilities of the decoder
v4l2-ctl -d /dev/video10 --all
```

### 4. Test with Different Video
Try a simpler H.264 video to rule out format issues:
```bash
# Create a test video with baseline profile
ffmpeg -i ../content/rpi4-e.mp4 -c:v libx264 -profile:v baseline -level 3.0 \
       -pix_fmt yuv420p test_baseline.mp4

./pickle --timing test_baseline.mp4
```

### 5. Force Software Decoding (Workaround)
If hardware decoder continues to fail, you can modify the code to skip hardware decoding:

In `video_decoder.c`, line 76, comment out the hardware decoder:
```c
// video->codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
// if (video->codec) {
//     printf("Using V4L2 M2M hardware decoder for H.264\n");
// } else {
    video->codec = avcodec_find_decoder(codec_id);
    printf("Using software decoder for video\n");
// }
```

Rebuild:
```bash
make -j4
```

### 6. Check Kernel Modules
```bash
# Ensure V4L2 M2M is loaded
lsmod | grep bcm2835

# Check kernel messages for errors
dmesg | grep -i v4l2
dmesg | grep -i bcm2835
```

### 7. Verify Video File Integrity
```bash
# Check video stream info
ffprobe -v error -show_streams ../content/rpi4-e.mp4

# Test decode with ffmpeg
ffmpeg -c:v h264_v4l2m2m -i ../content/rpi4-e.mp4 -f null -
```

## Performance Expectations

### With Hardware Decoding (Working)
- Decode time: 0.5-2ms per frame
- CPU usage: 10-20%
- Smooth 60fps playback

### With Software Decoding (Current State)
- Decode time: 30-60ms per frame
- CPU usage: 80-100%
- Stuttering playback, frame skipping

## Current Workarounds

1. **Use CPU Performance Governor**
   ```bash
   sudo sh -c 'echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor'
   ```

2. **Reduce Video Resolution**
   Encode video at lower resolution (e.g., 1280x720):
   ```bash
   ffmpeg -i ../content/rpi4-e.mp4 -s 1280x720 -c:v libx264 \
          -preset fast -crf 23 output_720p.mp4
   ```

3. **Lower Frame Rate**
   Re-encode at 30fps:
   ```bash
   ffmpeg -i ../content/rpi4-e.mp4 -r 30 -c:v libx264 output_30fps.mp4
   ```

## Checking Current Status

To verify if hardware decoding is working:
```bash
./pickle --timing your_video.mp4 2>&1 | grep -E "hardware|software|decode"
```

Look for:
- ✅ "Using V4L2 M2M hardware decoder" - Good
- ❌ "Attempting fallback to software decoding" - Hardware failed
- ✅ "Avg decode: 1-2ms" - Hardware working
- ❌ "Avg decode: 30-60ms" - Software decoding (slow)

## Additional Resources

- [Raspberry Pi V4L2 Documentation](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/v4l2.html)
- [FFmpeg V4L2 M2M Guide](https://trac.ffmpeg.org/wiki/HWAccelIntro)
- Raspberry Pi Forums: Search for "bcm2835-codec" issues
