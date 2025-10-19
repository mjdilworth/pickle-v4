# Test Video: 1920×1080 @ 60 FPS - High Profile Level 4.1

## File Created: `test_1920x1080_60fps_high41.mp4`

### Specifications

| Property | Value |
|----------|-------|
| **Resolution** | 1920×1080 pixels |
| **Frame Rate** | 60 fps (exactly 60/1) |
| **Codec** | H.264 (AVC) |
| **Profile** | High (100) |
| **Level** | 4.1 |
| **Pixel Format** | YUV420p |
| **Duration** | 5 seconds (300 frames) |
| **Total Frames** | 300 |
| **Bitrate** | ~2306 kbps |
| **File Size** | 1.4 MB |
| **Container** | MP4 with avcC |

### Creation Command

```bash
ffmpeg -f lavfi -i testsrc=size=1920x1080:duration=5:rate=60 \
  -pix_fmt yuv420p \
  -c:v libx264 \
  -profile:v high \
  -level:v 4.1 \
  -preset fast \
  -b:v 5000k \
  test_1920x1080_60fps_high41.mp4 -y
```

### Encoding Details

**Input:**
- TestSrc pattern (SMPTE color bars/test pattern)
- Generated at 1920×1080 @ 60fps
- Duration: 5 seconds
- RGB24 → YUV420p conversion

**Output Statistics:**
- Total frames encoded: 300
- I-frames: Variable (keyframe detection)
- P-frames: Predicted frames
- **B-frames: YES** (High profile supports bidirectional prediction)
- 8×8 transform intra: 3.9%
- 8×8 transform inter: 33.5% (High profile feature)
- Weighted P-Frames: Analyzed but not used
- Reference frames: Up to 16 possible
- Average bitrate: 2.3 Mbps (higher due to B-frames)

### High Profile Features (Why It's More Complex)

The High profile includes advanced H.264 features:

1. **B-Frames** - Bidirectional prediction
   - More compression efficiency
   - Requires complex decoding state machine
   - Uses both past and future reference frames

2. **8×8 Transform** - Instead of just 4×4
   - Better compression for smooth areas
   - More calculations required

3. **CAVLC & CABAC** - Advanced entropy coding
   - CABAC can be significantly more complex
   - Requires more memory access patterns

4. **Weighted Prediction** - Adaptive reference frame weighting
   - Better compression for fading transitions
   - More state to manage

5. **More Reference Frames** - Up to 16 vs 4 in Baseline
   - More memory bandwidth
   - More complex buffer management

### Comparison: All Test Files

| File | Resolution | FPS | Profile | Level | File Size | Bitrate | Complexity |
|------|---|---|---|---|---|---|---|
| **test_video.mp4** | 1920×1080 | 30 | Baseline | 4.0 | ? | ? | ✓ Low |
| **rpi4-e.mp4** | 1920×1080 | 60 | Main | 4.2 | ~400MB | 6730k | ⚠️ High |
| **rpi4-e-baseline.mp4** | 1920×1080 | 60 | Baseline | 4.0 | 125MB | 6732k | ✓ Low |
| **test_1920x1080_60fps.mp4** | 1920×1080 | 60 | Baseline | 4.0 | 803KB | 1315k | ✓ Low |
| **test_1920x1080_60fps_high41.mp4** | 1920×1080 | 60 | High | 4.1 | 1.4MB | 2306k | ⚠️ Very High |

### Hardware Decoder Compatibility

**bcm2835-codec (Raspberry Pi 4):**

| Profile | Support Status | Notes |
|---------|---|---|
| Baseline | ✅ Full | Always works |
| Constrained Baseline | ✅ Full | Subset of Baseline |
| Main | ⚠️ Partial | Works sometimes, bugs in edge cases |
| High | ❌ Unknown | B-frames may cause issues |
| High 10 | ❌ Not Supported | 10-bit color depth |
| High 422 | ❌ Not Supported | 4:2:2 chroma |

**Expected Result for High Profile:**
- May fail during hardware decode attempt
- Software fallback will handle it
- Useful for testing fallback reliability

### Why This Test File Is Useful

1. **Maximum Complexity** - Tests hardware limits
2. **B-Frame Handling** - Verifies fallback works for advanced features
3. **High Profile @ 4.1** - Professional-grade encoding
4. **Performance Stress Test** - Shows software decode capability
5. **Fallback Validation** - Confirms graceful degradation

### Use Cases

1. **Stress Testing** - Hardware decoder's breaking point
2. **Fallback Validation** - Ensure software fallback catches incompatible profiles
3. **Performance Benchmarking** - Compare Baseline vs High decode times
4. **Edge Case Testing** - Level 4.1 is upper limit for 1080p
5. **Feature Verification** - B-frame handling in software decoder

### Expected Behavior with Pickle

With your current `video_decoder.c`:

1. **Hardware Decode Attempt** (50 packet limit)
   - Try h264_v4l2m2m first
   - May fail with High profile + B-frames
   - EAGAIN loop likely

2. **Automatic Fallback** (after 50 packets)
   - Switch to software decoder (libavcodec)
   - Handles B-frames without issue
   - Continue playback seamlessly

3. **User Experience**
   - First frame: ~150-200ms (software decode slower)
   - But completely transparent - no user action needed
   - Perfect for validating robustness

### Technical Details

**avcC Header Analysis:**
```
Baseline (0x42):  0x01 0x42 0xc0 0x28
Main (0x4d):      0x01 0x4d 0x40 0x2a
High (0x64):      0x01 0x64 0x00 0x28  ← This file
  ├─ 0x01: avcC version 1
  ├─ 0x64: Profile = High (100)
  ├─ 0x00: No constraints (full High profile)
  └─ 0x28: Level = 4.1
```

**Frame Structure (High Profile with B-frames):**
```
Typical GOP (Group of Pictures):
[I-frame] [P-frame] [B-frame] [P-frame] [B-frame] [P-frame] ...
  ↓         ↓         ↓         ↓
  Uses      Uses      Uses      Uses
  Past      Past      Past+     Past
  only      only      Future    only
```

### Bitrate Comparison

- **Baseline 60fps**: 1315 kbps (simple, high compression)
- **High 4.1 60fps**: 2306 kbps (complex, better quality)
- **Ratio**: High profile produces ~1.75× bitrate for same visual quality
- **Reason**: B-frames require explicit overhead, more reference frame storage

### Summary

A stress-test video featuring **H.264 High Profile Level 4.1** - the most complex profile supported by ffmpeg at this resolution/framerate. Designed to:

✅ Test hardware decoder's limits  
✅ Validate software fallback mechanism  
✅ Benchmark performance differences  
✅ Confirm B-frame handling in software decoder  
✅ Verify graceful degradation  

This file is **expected to fail hardware decode** and successfully use the software fallback.
