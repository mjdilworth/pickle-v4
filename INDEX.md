# 📚 Test Video Suite - Complete Index

## 🎯 What Was Created

Two new test videos at **1920×1080 @ 60 FPS**:

1. **`test_1920x1080_60fps.mp4`** (803 KB)
   - Baseline Profile, Level 4.0
   - Purpose: Hardware acceleration baseline
   - Expected: ✅ HARDWARE decode

2. **`test_1920x1080_60fps_high41.mp4`** (1.4 MB)
   - High Profile, Level 4.1
   - Purpose: Stress test & fallback validation
   - Expected: ❌ SOFTWARE fallback

## 📖 Documentation Files

Read these in order:

### 1. **START HERE** → `QUICK_TEST_REFERENCE.md`
- One-page quick reference
- Test commands
- Expected outputs
- Profile support matrix

### 2. **For Details** → `TEST_SUITE_SUMMARY.md`
- Complete comparison of all test files
- Expected behavior for each scenario
- Testing workflow
- Performance benchmarks

### 3. **For Baseline** → `TEST_1920x1080_60fps_INFO.md`
- Detailed specs on Baseline version
- Why it should work with hardware
- Technical breakdown

### 4. **For High Profile** → `TEST_1920x1080_60fps_HIGH41_INFO.md`
- Detailed specs on High Profile version
- Why it needs software fallback
- B-frame explanation
- Complexity analysis

### 5. **Summary** → `TEST_SUITE_CREATED.md`
- Executive overview
- Test coverage matrix
- What you can now validate

## 🧪 Quick Test

```bash
cd /home/dilly/Projects/pickle-v4

# All tests should complete successfully
./pickle test_1920x1080_60fps.mp4           # Should be FAST
./pickle test_1920x1080_60fps_high41.mp4    # Should use FALLBACK
./pickle rpi4-e.mp4                         # Real-world test
./pickle test_video.mp4                     # Reference
```

## 📊 File Inventory

```
NEW FILES (Today):
├─ test_1920x1080_60fps.mp4          803 KB  ✓ Baseline, 60fps
└─ test_1920x1080_60fps_high41.mp4  1.4 MB  ⚠️ High Profile, 60fps

RECODED (Previous):
├─ rpi4-e-baseline.mp4              125 MB  ✓ Baseline, 60fps (recoded)
└─ rpi4-e.mp4                      ~400 MB  ❌ Main, 60fps (original)

REFERENCE:
└─ test_video.mp4                     ?     ✓ Baseline, 30fps

DOCUMENTATION (NEW):
├─ QUICK_TEST_REFERENCE.md
├─ TEST_SUITE_SUMMARY.md
├─ TEST_1920x1080_60fps_INFO.md
├─ TEST_1920x1080_60fps_HIGH41_INFO.md
├─ TEST_SUITE_CREATED.md
└─ This file (index)
```

## 🎯 Test Scenarios

### Scenario A: Hardware Performance
**File:** `test_1920x1080_60fps.mp4`
- Expected: ✅ Hardware decode (<100ms)
- Validates: bcm2835-codec works at 1080p60 Baseline

### Scenario B: Fallback Robustness  
**File:** `test_1920x1080_60fps_high41.mp4`
- Expected: ⚠️ Software fallback (~150-200ms)
- Validates: Graceful degradation works

### Scenario C: Real-World Edge Case
**File:** `rpi4-e.mp4`
- Expected: ⚠️ Software fallback (~88ms)
- Validates: Production content handled correctly

### Scenario D: Baseline Reference
**File:** `test_video.mp4`
- Expected: ✅ Hardware decode (<100ms)
- Validates: 30fps also works

## 🔍 What to Look For

### Hardware Success Output:
```
* Decoder: Hardware
* Frame format: yuv420p (0)
* Frame size: 1920x1080
First frame decoded successfully
GPU rendering started
```

### Fallback Triggered Output:
```
[WARN] Hardware decoder: Sent 50 packets without output, trying software fallback
Successfully switched to software decoding
* Decoder: Software
* Frame format: yuv420p (0)
* Frame size: 1920x1080
First frame decoded successfully
GPU rendering started
```

## 📈 Performance Expected

| File | Profile | Expected Time | Path |
|------|---------|---|---|
| test_1920x1080_60fps.mp4 | Baseline | ~50ms | Hardware |
| test_1920x1080_60fps_high41.mp4 | High | ~150-200ms | Software |
| rpi4-e.mp4 | Main | ~88ms | Software |
| test_video.mp4 | Baseline | ~50ms | Hardware |

## ✅ Validation Checklist

- [ ] All files present in `/home/dilly/Projects/pickle-v4/`
- [ ] Can list: `ls -lh test_1920x1080_60fps*.mp4`
- [ ] Baseline works with hardware
- [ ] High Profile triggers fallback
- [ ] GPU rendering active for both
- [ ] No crashes or hangs
- [ ] Performance as expected

## 🚀 Next Steps

1. **Run tests** using commands in QUICK_TEST_REFERENCE.md
2. **Compare results** with expected outputs
3. **Document findings** in project notes
4. **Validate** performance metrics
5. **Use files** for regression testing

## 📞 File Purposes

| File | Tests | Use When |
|------|---|---|
| test_1920x1080_60fps.mp4 | Hardware performance | Baseline validation |
| test_1920x1080_60fps_high41.mp4 | Fallback mechanism | Edge case testing |
| rpi4-e.mp4 | Real-world content | Production validation |
| rpi4-e-baseline.mp4 | Recoded comparison | Profile comparison |
| test_video.mp4 | 30fps baseline | Reference point |

---

## 📋 Summary

You now have:

✅ **5 test videos** covering multiple profiles/framerates  
✅ **6 documentation files** explaining everything  
✅ **Complete test suite** for validation  
✅ **Fallback coverage** for edge cases  
✅ **Performance benchmarks** for comparison  

All files are in `/home/dilly/Projects/pickle-v4/` ready to use! 🎬

---

**Ready to test?** Start with `QUICK_TEST_REFERENCE.md`
