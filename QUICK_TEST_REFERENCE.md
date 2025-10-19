# Quick Reference: Test Videos @ 1920×1080 @ 60 FPS

## Created Files

### 1️⃣ `test_1920x1080_60fps.mp4`
- **Profile**: Baseline (0x42) ✓ Simple
- **Level**: 4.0
- **Size**: 803 KB
- **Bitrate**: 1315 kbps
- **Expected HW**: ✅ **HARDWARE** (fast)
- **Purpose**: Baseline performance reference

### 2️⃣ `test_1920x1080_60fps_high41.mp4` 
- **Profile**: High (0x64) ⚠️ Complex
- **Level**: 4.1
- **Size**: 1.4 MB
- **Bitrate**: 2306 kbps
- **Expected HW**: ❌ **FALLBACK** (software)
- **Purpose**: Stress test & fallback validation

## Existing Files

### 3️⃣ `rpi4-e-baseline.mp4`
- **Profile**: Baseline (0x42) ✓ Simple
- **Level**: 4.0
- **Size**: 125 MB
- **Bitrate**: 6732 kbps
- **Expected HW**: ✅ **HARDWARE** (fast)
- **Purpose**: Recoded version of problematic file

### 4️⃣ `rpi4-e.mp4`
- **Profile**: Main (0x4d) ⚠️ Medium-High
- **Level**: 4.2
- **Size**: ~400 MB
- **Bitrate**: 6730 kbps
- **Expected HW**: ❌ **FALLBACK** (software)
- **Purpose**: Real-world edge case

### 5️⃣ `test_video.mp4`
- **Profile**: Baseline (0x42) ✓ Simple
- **Level**: 4.0
- **Size**: ?
- **Bitrate**: ?
- **Expected HW**: ✅ **HARDWARE** (fast)
- **Purpose**: Reference baseline

## One-Line Tester

```bash
for f in test_1920x1080_60fps.mp4 test_1920x1080_60fps_high41.mp4 rpi4-e.mp4 test_video.mp4; do echo "=== $f ==="; timeout 3 ./pickle "$f" 2>&1 | grep -E "Decoder:|fallback|Successfully" | head -2; done
```

## Profile Support Matrix

```
bcm2835-codec (Hardware):
  ✅ Baseline       → FAST
  ✅ Baseline (CB)  → FAST
  ⚠️  Main          → Sometimes (edge cases fail)
  ❌ High           → Not supported
  ❌ High 10/422/444 → Not supported

libavcodec (Software Fallback):
  ✅ Baseline       → Works (slower)
  ✅ Main           → Works (slower)
  ✅ High           → Works (slower)
  ✅ High 10/422/444 → Works (slower)
```

## Expected Output

### Hardware Success (Baseline files)
```
* Decoder: Hardware
* First frame: <100ms
GPU rendering activated
```

### Fallback Triggered (High/Main files)
```
[WARN] Hardware decoder: Sent 50 packets without output, trying software fallback
Successfully switched to software decoding
* Decoder: Software
* First frame: 150-200ms
GPU rendering activated
```

## Test Commands

### Test All Files
```bash
cd /home/dilly/Projects/pickle-v4

echo "1. Baseline 60fps (should be FAST):"
timeout 3 ./pickle test_1920x1080_60fps.mp4 2>&1 | grep -i "decoder:\|successfully"

echo -e "\n2. High Profile 60fps (should use FALLBACK):"
timeout 3 ./pickle test_1920x1080_60fps_high41.mp4 2>&1 | grep -i "decoder:\|fallback\|successfully"

echo -e "\n3. Real Main Profile 60fps (should use FALLBACK):"
timeout 3 ./pickle rpi4-e.mp4 2>&1 | grep -i "decoder:\|fallback\|successfully"

echo -e "\n4. Reference Baseline 30fps (should be FAST):"
timeout 3 ./pickle test_video.mp4 2>&1 | grep -i "decoder:\|successfully"
```

## Key Takeaways

| Aspect | Hardware | Software Fallback |
|--------|----------|---|
| **Speed** | ✓ Fast (<100ms) | Slower (150-200ms) |
| **CPU** | ✓ Low | Higher |
| **Profiles** | Baseline only | All profiles |
| **Reliability** | Profile-dependent | 100% reliable |
| **User Notice** | Transparent | Transparent |

## Files Breakdown

### Size Comparison
```
test_1920x1080_60fps.mp4       803 KB  (test pattern, highly compressible)
test_1920x1080_60fps_high41.mp4 1.4 MB (High profile overhead)
rpi4-e-baseline.mp4           125 MB  (real content, 5 min video)
rpi4-e.mp4                   ~400 MB  (real content, full length)
```

### What Each Tests

| File | Tests What |
|------|---|
| test_1920x1080_60fps.mp4 | Hardware performance at 1080p60 |
| test_1920x1080_60fps_high41.mp4 | Fallback mechanism under stress |
| rpi4-e.mp4 | Real-world Main profile handling |
| rpi4-e-baseline.mp4 | Hardware reliability with real content |
| test_video.mp4 | Baseline performance reference |

---

**All files ready for testing!** 🎬
