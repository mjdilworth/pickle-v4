# 🚀 Getting Started with Test Suite

## Quickest Start (30 seconds)

```bash
cd /home/dilly/Projects/pickle-v4

# Test 1: Hardware (fast)
timeout 3 ./pickle test_1920x1080_60fps.mp4 2>&1 | head -20

# Test 2: Fallback (comprehensive)
timeout 3 ./pickle test_1920x1080_60fps_high41.mp4 2>&1 | head -20
```

## What to Look For

### Test 1 Output (Should show Hardware)
```
* Decoder: Hardware
First frame decoded successfully
Frame 1: YUV pointers...
GPU YUV→RGB rendering started
```

### Test 2 Output (Should show Fallback)
```
[WARN] Hardware decoder: Sent 50 packets without output, trying software fallback
Successfully switched to software decoding
* Decoder: Software
Frame 1: YUV pointers...
GPU YUV→RGB rendering started
```

## Files You Created Today

1. **test_1920x1080_60fps.mp4** (803 KB)
   - Baseline Profile
   - Should be fast

2. **test_1920x1080_60fps_high41.mp4** (1.4 MB)  
   - High Profile
   - Should trigger fallback

## What to Read Next

### For Quick Overview
→ **README_TEST_SUITE.md**

### For Test Commands
→ **QUICK_TEST_REFERENCE.md**

### For Deep Dive
→ **TEST_SUITE_SUMMARY.md**

## Success Criteria

✅ Both files decode without crashing  
✅ First file uses hardware (faster)  
✅ Second file uses software (via fallback)  
✅ Both show GPU rendering  
✅ No error messages  

---

That's it! You've created a comprehensive test suite. 🎬
