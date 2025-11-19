# Production Readiness Fixes - Pickle v1.0.0

## Overview
This document details critical production fixes applied to the software decode path (non-`--hw` mode) to ensure stability, safety, and proper resource management.

## Fixes Applied

### 1. Memory Leak Fixes

#### video_decoder.c - BSF Error Path Leaks
**Issue**: Memory leaks when BSF (bitstream filter) allocation/initialization failed
**Fix**: Proper cleanup ordering - free codec_ctx before calling video_cleanup()
- Lines modified: BSF allocation error paths in video_init()
- Impact: Prevents memory leaks on decoder initialization failures

#### video_decoder.c - NULL Safety in Cleanup
**Issue**: Potential double-free and use-after-free bugs in video_cleanup()
**Fix**: Added NULL checks before all free operations and NULL pointer assignments after cleanup
- Lines modified: video_cleanup() function
- Added checks for: codec_ctx, pkt, frame, bsf_ctx, fmt_ctx, sws_ctx
- All pointers explicitly nulled after freeing

### 2. Thread Safety Improvements

#### video_player.c - Pthread Error Handling
**Issue**: No error checking on pthread mutex/condition operations
**Fix**: Added comprehensive error checking with fallback paths
- pthread_mutex_destroy() - checks return value, warns on EBUSY
- pthread_cond_broadcast() - replaced signal() for proper wakeup
- pthread_mutex_lock/unlock - error checking in async decoder

#### video_decoder.c - Mutex Error Checking
**Issue**: Silent failures on mutex operations could cause deadlocks
**Fix**: Added error checking with stderr warnings
- pthread_mutex_destroy() in video_cleanup()
- Provides diagnostic output for EBUSY conditions

#### video_player.c - Async Decoder Timeout
**Issue**: 100ms timeout too short for graceful async decoder shutdown
**Fix**: Increased timeout to 200ms
- Reduces forced thread cancellations
- Gives decoders time to finish current frame

### 3. Signal Handler Safety (CRITICAL)

#### pickel.c - Async-Signal-Safe Signal Handler
**Issue**: Signal handler was calling non-async-signal-safe operations:
- Writing to `g_app->running` (complex struct access)
- Potential for deadlocks if signal arrives during malloc/mutex operations

**Fix**: Converted to fully async-signal-safe implementation:
```c
// Global atomic flag - safe for signal handlers
volatile sig_atomic_t g_quit_requested = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_quit_requested = 1;  // ONLY operation - guaranteed safe
}
```

**Changes**:
- Removed all g_app access from signal handler
- Added g_quit_requested global atomic flag
- Main loop checks: `while (app->running && !g_quit_requested)`
- Emergency handlers added for SIGSEGV/SIGBUS (terminal restoration only)

**Impact**: Eliminates signal-related deadlocks and race conditions

### 4. File Descriptor Leak Fixes

#### drm_display.c - DRM Initialization Error Paths
**Issue**: 7 error paths in drm_init() leaked file descriptors
**Fix**: Added proper cleanup on ALL error paths:
```c
close(drm->drm_fd);
drm->drm_fd = -1;
drm->connector = NULL;
drm->encoder = NULL;
drm->crtc = NULL;
```

**Fixed paths**:
1. No connectors available
2. No encoder for connector
3. drmModeGetEncoder failed
4. No CRTC for encoder
5. drmModeGetCrtc failed
6. No valid display mode found
7. GBM device creation failed

**Impact**: Prevents FD exhaustion on repeated initialization failures

### 5. Code Quality Improvements

#### Compiler Warnings Fixed
- Unused variable `ret` in video_player.c
- Missing return statement in gl_context.c
- All builds now warning-free with `-Wall -Wextra`

## Testing Recommendations

### Signal Handling Test
```bash
# Start pickle, then Ctrl+C - should exit cleanly
./pickle video1.mp4 video2.mp4

# Expected: "Quit signal received" + graceful shutdown
# No deadlocks, no hung threads
```

### FD Leak Test
```bash
# Rapid start/stop cycles
for i in {1..100}; do 
    timeout 0.5s ./pickle video1.mp4 video2.mp4
done

# Check FD usage: should not accumulate
lsof -p $(pgrep pickle) | wc -l
```

### Memory Leak Test
```bash
# Run under valgrind
valgrind --leak-check=full --show-leak-kinds=all \
    timeout 5s ./pickle video1.mp4 video2.mp4

# Expected: No definite leaks in decoder/player code
# (FFmpeg internals may show some one-time allocations)
```

### Thread Safety Test
```bash
# Use thread sanitizer
gcc -fsanitize=thread -g [sources...] -o pickle_tsan
./pickle_tsan video1.mp4 video2.mp4

# Expected: No data races in app code
```

## Production Deployment Checklist

- [x] Memory leaks fixed in error paths
- [x] Thread safety - proper mutex error handling
- [x] Signal handlers are async-signal-safe
- [x] File descriptors properly closed on errors
- [x] Compiler warnings eliminated
- [ ] Load tested with real video files
- [ ] Tested rapid start/stop cycles (FD leaks)
- [ ] Tested signal handling (Ctrl+C graceful shutdown)
- [ ] Tested under valgrind (memory leaks)
- [ ] Tested with thread sanitizer (data races)

## Architecture Notes

### Signal Flow
1. OS delivers signal → `signal_handler()` sets `g_quit_requested = 1`
2. Main loop checks `app->running && !g_quit_requested`
3. Graceful shutdown: async decoders → video contexts → DRM cleanup
4. Terminal restoration via atexit handler (always runs)

### Cleanup Order (Critical)
1. Set app->running = false (stops frame processing)
2. Destroy async decoders (200ms timeout per thread)
3. Free video decoder contexts (FFmpeg cleanup)
4. Destroy GL textures/contexts
5. Close DRM device
6. Terminal restoration (atexit)

### Resource Limits (production_config.h)
- MAX_VIDEO_FILE_SIZE: 10GB
- MAX_DECODED_FRAMES: 100,000
- Async decode timeout: 200ms
- Frame queue depth: Double-buffered

## Files Modified

1. **video_decoder.c**
   - NULL checks in video_cleanup()
   - BSF error path fixes
   - Mutex error checking

2. **video_player.c**
   - Async decoder timeout increase
   - Pthread error checking
   - Main loop signal flag check
   - Added extern declaration for g_quit_requested

3. **pickel.c**
   - Async-signal-safe signal handler
   - Global g_quit_requested flag
   - Emergency SIGSEGV/SIGBUS handlers

4. **drm_display.c**
   - FD leak fixes (7 error paths)
   - Proper pointer nulling on cleanup

## Performance Impact

All fixes have **negligible performance impact**:
- NULL checks: Branch predictor handles these efficiently
- Mutex error checking: Only on create/destroy (not hot path)
- Signal flag check: Single atomic read per frame (~60Hz)
- FD cleanup: Only on error paths (not normal operation)

## Compliance

These fixes ensure compliance with:
- POSIX signal handling requirements (async-signal-safe)
- pthread best practices (error checking, proper timeouts)
- Linux resource management (FD cleanup)
- C99 memory safety (NULL checks, no double-free)

## Additional Notes

### Why Async-Signal-Safe Matters
Non-safe operations in signal handlers can cause:
- **Deadlocks**: Signal during malloc → handler calls malloc → deadlock
- **Corruption**: Signal during mutex lock → handler locks → undefined behavior
- **Crashes**: Interrupted heap operations → corrupted allocator state

Our fix uses ONLY `sig_atomic_t` assignment - guaranteed safe by POSIX.

### Why FD Leaks Matter
Each leaked DRM FD holds:
- Kernel DRM master lock
- Display mode resources
- Memory-mapped buffers
- Event queue structures

Accumulated leaks → system cannot open displays → requires reboot.

### Why Thread Timeouts Matter
100ms insufficient for:
- YUV420P frame uploads (large resolutions)
- Network-sourced streams (I/O delays)
- CPU throttled systems (thermal)

200ms provides margin without noticeable delay on Ctrl+C.

## Future Improvements

1. **Monitoring**: Add metrics for FD count, thread timeouts, signal handling
2. **Logging**: Structured logging with severity levels (DEBUG/INFO/WARN/ERROR)
3. **Graceful degradation**: Retry logic for transient DRM failures
4. **Health checks**: Periodic self-test of async decoders, watchdog timers
5. **Rate limiting**: Throttle error messages to prevent log spam

---

**Version**: 1.0.0-production  
**Date**: 2024  
**Validated**: Clean compile, no warnings, no static analysis errors  
**Status**: ✅ Ready for production testing
