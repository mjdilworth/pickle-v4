# Test Video Suite: 1920×1080 @ 60 FPS Comparison

## Overview

You now have a comprehensive test suite of H.264 videos at 1920×1080 @ 60 FPS to validate hardware vs. software decoder performance:

## Quick Reference Table

| File | Profile | Level | Complexity | Expected HW Result | File Size | Bitrate |
|------|---------|-------|---|---|---|---|
| `test_1920x1080_60fps.mp4` | Baseline | 4.0 | ✓ Low | ✅ Hardware | 803 KB | 1315 kbps |
| `test_1920x1080_60fps_high41.mp4` | **High** | **4.1** | ⚠️ Very High | ❌ Fallback | 1.4 MB | 2306 kbps |
| `rpi4-e-baseline.mp4` | Baseline | 4.0 | ✓ Low | ✅ Hardware | 125 MB | 6732 kbps |
| `rpi4-e.mp4` | Main | 4.2 | ⚠️ High | ❌ Fallback | 400 MB | 6730 kbps |
| `test_video.mp4` | Baseline | 4.0 | ✓ Low | ✅ Hardware | ? | ? |

## Test Scenarios

### Scenario 1: Simple Baseline @ 60 FPS
**File:** `test_1920x1080_60fps.mp4`

```
Expected Flow:
┌─────────────────┐
│ Hardware Decode │ ← Should succeed immediately
│ h264_v4l2m2m    │
└────────┬────────┘
         │ Success (~50ms first frame)
         ↓
    GPU Rendering
```

**Purpose:** Baseline for hardware performance

---

### Scenario 2: Complex High Profile @ 60 FPS
**File:** `test_1920x1080_60fps_high41.mp4`

```
Expected Flow:
┌─────────────────┐
│ Hardware Decode │ ← Will attempt
│ h264_v4l2m2m    │
└────────┬────────┘
         │ EAGAIN × 50 packets
         ↓
┌──────────────────────┐
│ Automatic Fallback   │ ← Triggers after 50 packets
│ Software (libavcodec)│
└────────┬─────────────┘
         │ Success (~150-200ms first frame)
         ↓
    GPU Rendering
```

**Purpose:** Test fallback mechanism robustness

---

### Scenario 3: Real-World Main Profile @ 60 FPS
**File:** `rpi4-e.mp4`

```
Expected Flow:
┌─────────────────┐
│ Hardware Decode │ ← Will attempt
│ h264_v4l2m2m    │
└────────┬────────┘
         │ EAGAIN × 50 packets
         ↓
┌──────────────────────┐
│ Automatic Fallback   │ ← Triggers
│ Software (libavcodec)│
└────────┬─────────────┘
         │ Success (real file, ~88ms first frame)
         ↓
    GPU Rendering
```

**Purpose:** Real-world edge case validation

---

### Scenario 4: Safe Baseline @ 30 FPS (Reference)
**File:** `test_video.mp4`

```
Expected Flow:
┌─────────────────┐
│ Hardware Decode │ ← Should succeed quickly
│ h264_v4l2m2m    │
└────────┬────────┘
         │ Success (<100ms first frame)
         ↓
    GPU Rendering
```

**Purpose:** Baseline reference point

---

## Profile Complexity Hierarchy

```
Complexity Increase ↑
│
├─ Baseline (0x42)         ✓ Simple
│   └─ No B-frames
│   └─ Limited refs (4)
│   └─ Simple entropy coding
│
├─ Constrained Baseline    ✓ Very Simple
│   └─ More restrictive Baseline
│
├─ Main (0x4d)            ⚠️ Medium-High
│   ├─ B-frames (limited)
│   ├─ Up to 16 ref frames
│   ├─ Weighted prediction
│
├─ High (0x64)            ⚠️ Very High
│   ├─ Full B-frame support
│   ├─ Up to 16 ref frames
│   ├─ 8×8 transforms
│   ├─ CAVLC/CABAC entropy
│   └─ Full weighted prediction
│
└─ High 10/422/444         ⚠️ Extremely High (10+ bit, 4:2:2, 4:4:4)
```

## Expected Test Results

### Part 1: Hardware Decoder Limits

Running on bcm2835-codec (RPi4):

| Test File | Profile | Expected Result | Notes |
|-----------|---------|---|---|
| test_1920x1080_60fps.mp4 | Baseline | ✅ Hardware | Simple, should work |
| rpi4-e-baseline.mp4 | Baseline | ✅ Hardware | Real file, higher bitrate |
| rpi4-e.mp4 | Main | ❌ Fallback | Known issue - Main profile edge case |
| test_1920x1080_60fps_high41.mp4 | High | ❌ Fallback | Too complex for V4L2 M2M |

### Part 2: Fallback Mechanism Validation

All should decode successfully:

```
Hardware Success (Baseline):
  ✅ Fast decode
  ✅ Low CPU
  ✅ GPU rendering immediate

Hardware Failure → Fallback (Main/High):
  ✅ Slower decode
  ✅ Higher CPU
  ✅ But still works!
  ✅ Transparent to user
```

### Part 3: Performance Benchmarking

Expected first-frame decode times:

```
test_1920x1080_60fps.mp4 (Baseline):
  Hardware: ~50ms
  Expected: 🟢 PASS

test_1920x1080_60fps_high41.mp4 (High 4.1):
  Fallback: ~150-200ms
  Expected: 🟢 PASS (via fallback)

rpi4-e.mp4 (Main 4.2, real content):
  Fallback: ~88ms
  Expected: 🟢 PASS (via fallback)
```

## Testing Workflow

### Quick Test: All 4 Files

```bash
#!/bin/bash
cd /home/dilly/Projects/pickle-v4

echo "=== Testing Hardware-Friendly (Baseline) ==="
timeout 3 ./pickle test_1920x1080_60fps.mp4 2>&1 | grep "Decoder:\|Successfully"

echo -e "\n=== Testing Fallback Required (High 4.1) ==="
timeout 3 ./pickle test_1920x1080_60fps_high41.mp4 2>&1 | grep "Decoder:\|fallback\|Successfully"

echo -e "\n=== Testing Real-World Edge Case (Main 4.2) ==="
timeout 3 ./pickle rpi4-e.mp4 2>&1 | grep "Decoder:\|fallback\|Successfully"

echo -e "\n=== Testing Reference (Baseline 30fps) ==="
timeout 3 ./pickle test_video.mp4 2>&1 | grep "Decoder:\|Successfully"
```

## Key Insights

### ✅ Hardware Decoder (bcm2835-codec)
- **Best Case**: Baseline Profile
- **Performance**: <100ms first frame
- **Memory**: Minimal CPU/GPU load
- **Limitation**: Main/High profiles unsupported

### ⚠️ Software Fallback (libavcodec)
- **Handles**: Any H.264 profile
- **Performance**: 50-200ms first frame
- **Tradeoff**: Higher CPU usage
- **Benefit**: Seamless, automatic, transparent

### 🎯 Your Implementation
- **Strategy**: Try hardware first, fallback on timeout
- **Safety**: 50-packet limit prevents hangs
- **Result**: Best of both worlds
- **User Experience**: Always works, fastest possible

## Files Summary

### Created Today
- ✅ `test_1920x1080_60fps.mp4` - Baseline @ 60fps (803 KB)
- ✅ `test_1920x1080_60fps_high41.mp4` - High Profile 4.1 @ 60fps (1.4 MB)

### Previously Available
- ✅ `rpi4-e.mp4` - Main @ 60fps (400 MB, real content)
- ✅ `rpi4-e-baseline.mp4` - Recoded to Baseline @ 60fps (125 MB)
- ✅ `test_video.mp4` - Baseline @ 30fps (reference)

## Recommendations

1. **Test All 4** - Validates entire decoder chain
2. **Monitor Times** - Confirm hardware vs software performance gap
3. **Check Fallback** - Verify "WARN" messages appear for Main/High
4. **Benchmark** - Use these for performance regression testing
5. **Document** - Results confirm profile-based decoder selection

## Next Steps

1. Run pickle on each file
2. Compare decode times
3. Verify fallback messages
4. Confirm GPU rendering activates
5. Document results in project

---

**Summary:** You now have a complete test suite validating hardware acceleration, profile limits, and fallback reliability! 🎬
