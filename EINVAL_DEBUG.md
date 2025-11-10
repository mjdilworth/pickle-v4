# Zero-Copy Debugging Summary

## Critical Discovery: The EINVAL Gateway

The `av_hwframe_ctx_init()` call is **the critical gateway** to zero-copy support. This is where we determine if the kernel and FFmpeg can work together to provide GEM-backed buffers.

### Current Status
```
av_hwframe_ctx_init() returns: AVERROR(EINVAL) - Invalid argument
```

### What This Means
The FFmpeg DRM backend is trying to allocate a buffer pool (via the DRM device context), but V4L2 M2M doesn't support that allocation model. V4L2 M2M manages its own buffers internally and doesn't allow FFmpeg to pre-allocate a pool.

### Why This Blocks Zero-Copy
Without frames context initialization succeeding:
- FFmpeg cannot properly configure the V4L2 driver
- V4L2 M2M defaults to mmap mode (system memory)
- Decoder outputs yuv420p instead of DRM_PRIME
- No DMA FDs are generated
- No zero-copy possible

## Debug Output Example

```
[HWACCEL] Creating and initializing hardware frames context...
[HWACCEL] ✓ HW frames context allocated
[HWACCEL] Frames context config:
[HWACCEL]   format (GPU):  drm_prime (178)
[HWACCEL]   sw_format:    nv12 (23) - RPi V3D preference
[HWACCEL]   dimensions:   1920x1080 (from codec)
[HWACCEL]   pool size:    20 buffers
[HWACCEL] Initializing HW frames context (calling V4L2_MEMORY_DMABUF setup)...
[HWACCEL] ✗ Failed to initialize HW frames context: Invalid argument
[HWACCEL] CRITICAL: This blocks zero-copy. Check dmesg for driver errors:
[HWACCEL]   dmesg | grep -i 'drm\|v4l2\|v3d'
[HWACCEL] Possible causes:
[HWACCEL]   1. Kernel V4L2 M2M driver doesn't support DRM_PRIME output
[HWACCEL]   2. V3D GPU driver version incompatibility
[HWACCEL]   3. DRM backend needs additional configuration
[HWACCEL]   4. Check: uname -r && modinfo bcm2835_codec
```

## Debugging Steps

### 1. Verify Kernel/Driver Versions
```bash
uname -r
modinfo bcm2835_codec | grep version
lsmod | grep v4l2
```

### 2. Check dmesg for Errors
```bash
dmesg | tail -100 | grep -E 'drm|v4l2|v3d|gem|dmabuf' -i
```

### 3. Examine V4L2 Device
```bash
v4l2-ctl -d /dev/video10 --info
v4l2-ctl -d /dev/video10 --list-formats
```

### 4. Check DRM Device
```bash
ls -la /dev/dri/
drm_info
```

### 5. Test FFmpeg Directly
```bash
/usr/local/bin/ffmpeg -h decoder=h264_v4l2m2m 2>&1 | head -50
```

## Solutions to Try

### Solution 1: Rebuild FFmpeg with Different Configuration
Some FFmpeg configurations might not wire up the DRM frames context properly.

```bash
# Rebuild with explicit DRM configuration
cd /home/dilly/Projects/Video/patch/rpi-ffmpeg
./configure \
  --enable-libdrm \
  --enable-v4l2-m2m \
  --enable-shared \
  [other flags]
```

### Solution 2: Patch FFmpeg Source Code
Modify v4l2_m2m_dec.c to bypass frames context requirement:

```c
// In v4l2_m2m_dec.c, modify choose_capture_format() to not fail if 
// frames context allocation fails
// Instead, rely on device context and format callback
```

### Solution 3: Kernel Driver Update
The bcm2835-codec driver may need updating to support DMABUF mode properly:

```bash
# Check current kernel version
uname -r

# Update if needed:
# sudo apt update && sudo apt full-upgrade

# Or rebuild kernel with DRM_PRIME support
```

### Solution 4: Use Device Context Only
Modify init_hw_accel_context() to NOT require frames context success:

```c
// Allow frames context to fail gracefully
// Device context alone might be enough to trigger DMABUF mode
if (ret < 0) {
    printf("[HWACCEL] ⚠ Frames context init failed, continuing with device context only\n");
    // Don't return -1, just unref and continue
}
```

## Next Steps

1. **Gather diagnostic information**:
   ```bash
   echo "=== System Info ===" 
   uname -r
   echo "=== V4L2 M2M Driver ===" 
   modinfo bcm2835_codec
   echo "=== DRM Devices ===" 
   ls -la /dev/dri/
   echo "=== Kernel Messages ===" 
   dmesg | tail -50
   ```

2. **Try FFmpeg rebuild** (most likely solution):
   - Use `/home/dilly/Projects/pickle-v4/build_patched_ffmpeg.sh`
   - This ensures v4l2_m2m_dec.c is properly compiled with all callbacks

3. **If rebuild doesn't work**, check if there's a kernel driver issue:
   - V4L2 M2M may need to be recompiled
   - RPi kernel may need patching for DRM_PRIME support

4. **Last resort**: Patch source code manually
   - Modify FFmpeg to not require frames context
   - Modify kernel driver to output DRM_PRIME buffers

## Key Insight

The frames context initialization is the **critical control point**. When it succeeds, the entire zero-copy pipeline should work because:

1. FFmpeg knows to configure DMABUF
2. V4L2 M2M allocates GEM-backed buffers
3. Kernel provides real DMA FDs
4. Application extracts DMA FDs
5. EGL imports DMA FDs as GPU textures
6. Result: 60fps smooth playback with <1ms texture upload

When it fails (current state), we're stuck with system memory and CPU uploads.
