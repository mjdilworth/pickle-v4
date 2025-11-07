# Pickle Video Player

A high-performance video player using hardware-accelerated decoding and OpenGL ES rendering.

## Performance Optimization: CPU Governor Configuration

For optimal video playback performance, especially to eliminate stuttering and periodic frame drops, it's crucial to configure the CPU governor properly.

### The Problem

By default, many Linux systems use the "ondemand" CPU governor, which dynamically scales CPU frequency based on load. This can cause periodic performance spikes (100-140ms delays) during video playback when the CPU frequency scales down during lighter processing periods and then needs to ramp back up.

### Solution: Performance Mode

Set the CPU governor to "performance" mode to maintain consistent maximum CPU frequency during video playback.

## Quick Setup

### 1. Check Current CPU Governor
```bash
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
```

### 2. Check Available Governors
```bash
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors
```

### 3. Set Performance Mode (Temporary)
```bash
sudo sh -c 'echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor'
```

### 4. Verify the Change
```bash
# Check governor is set
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Check CPU frequencies (should show max frequency)
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq
```

### 5. Run Video Player
```bash
./pickle /path/to/your/video.mp4
```

## Permanent Configuration (Optional)

To make the performance governor persistent across reboots:

### Method 1: Using cpufrequtils
```bash
# Install cpufrequtils
sudo apt update
sudo apt install cpufrequtils

# Edit configuration
sudo nano /etc/default/cpufrequtils

# Add this line:
GOVERNOR="performance"

# Restart service
sudo systemctl restart cpufrequtils
```

### Method 2: Using systemd service
Create a systemd service to set the governor on boot:

```bash
sudo nano /etc/systemd/system/cpu-performance.service
```

Add this content:
```ini
[Unit]
Description=Set CPU Governor to Performance
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

Enable the service:
```bash
sudo systemctl enable cpu-performance.service
sudo systemctl start cpu-performance.service
```

## Performance Results

With the CPU governor set to performance mode, you should see:

- **Consistent frame times**: 3-6ms total frame processing
- **Stable rendering**: GL operations in 2.9-3.3ms range
- **Smooth playback**: No periodic 100-140ms spikes
- **Optimal decode times**: 0.6-1.4ms average decode performance

## Reverting to Default

To restore the default "ondemand" governor:

```bash
sudo sh -c 'echo ondemand | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor'
```

## Build Instructions

```bash
# Compile the video player
make clean
make

# Run with a video file
./pickle /path/to/video.mp4
```

## System Requirements

- Linux with V4L2 M2M hardware decoding support
- OpenGL ES 3.1
- DRM/KMS display support
- FFmpeg libraries (libavformat, libavcodec, libavutil)

## Troubleshooting

### Video Stuttering
1. First, check if CPU governor is set to performance mode
2. Verify CPU frequencies are at maximum
3. Check for thermal throttling: `cat /sys/class/thermal/thermal_zone*/temp`

### Hardware Decoding Issues
The player automatically falls back to software decoding if hardware acceleration fails.

### Display Issues
Ensure you have proper DRM/KMS permissions and are running as root or with appropriate group membership.

---

*Note: This configuration prioritizes performance over power efficiency. Consider reverting to "ondemand" when not using video applications to save battery life on laptops.*




Mike Dilworth is inviting you to a scheduled Zoom meeting.

Topic: Mike Dilworth
Time: Nov 13, 2025 11:45 AM Athens
Join Zoom Meeting
https://us05web.zoom.us/j/2576768572?pwd=zpanyL382meBhZ9aWNJUIcpu5Cjk3F.1&omn=86524305978

Meeting ID: 257 676 8572
Passcode: vdRuF2

mmcblk0p3 vfat   FAT32 SHARED 4464-FE62



# Add cloudflare gpg key
sudo mkdir -p --mode=0755 /usr/share/keyrings
curl -fsSL https://pkg.cloudflare.com/cloudflare-public-v2.gpg | sudo tee /usr/share/keyrings/cloudflare-public-v2.gpg >/dev/null

# Add this repo to your apt repositories
echo 'deb [signed-by=/usr/share/keyrings/cloudflare-public-v2.gpg] https://pkg.cloudflare.com/cloudflared any main' | sudo tee /etc/apt/sources.list.d/cloudflared.list

# install cloudflared
sudo apt-get update && sudo apt-get install cloudflared

