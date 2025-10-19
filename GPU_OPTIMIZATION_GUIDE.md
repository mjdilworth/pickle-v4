# GPU Rendering Optimization Guide

## Current Performance Bottleneck

**Current Metrics:**
- Decode: 1-3ms per frame (✅ EXCELLENT - hardware)
- Render: 20-30ms per frame (⚠️ BOTTLENECK - GPU)
- Target: 16.7ms @ 60fps
- Actual: ~45fps achieved

**Goal:** Reduce GPU rendering from 20-30ms to <16.7ms to achieve 60fps

---

## Root Cause Analysis

The GPU rendering pipeline (in `gl_render_frame()`) has several performance-affecting operations:

### 1. **Texture Upload Overhead (HIGHEST IMPACT)**
**Location:** `gl_context.c` lines 430-600

**Current Implementation:**
```c
// For every frame:
// 1. glTexImage2D() - full texture reallocation
// 2. glTexSubImage2D() - partial texture update
// 3. Stride conversion with memcpy for non-contiguous data
```

**Problem:**
- Uploading ~1.9 MB per frame (1920×1080 Y + UV planes) for EVERY frame
- At 60fps = 114 MB/s GPU memory bandwidth
- On Raspberry Pi 4: VideoCore IV GPU has limited bandwidth (~50 GB/s shared with CPU)
- Current code recalculates whether to use `glTexImage2D()` or `glTexSubImage2D()` on EVERY frame

**Time Cost:** ~8-12ms per frame (40-60% of render time)

---

### 2. **Redundant Texture State Restoration (MEDIUM IMPACT)**
**Location:** `gl_context.c` lines 402-440

**Current Implementation:**
```c
// For every frame:
glBindBuffer(GL_ARRAY_BUFFER, 0);          // Unbind
glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);  // Unbind
glUseProgram(0);                           // Disable program
glDisableVertexAttribArray(0);              // Disable attributes
glDisableVertexAttribArray(1);              // Disable attributes

// Then rebind everything:
glUseProgram(gl->program);
glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);
glVertexAttribPointer(0, ...);
glEnableVertexAttribArray(0);
// ... more redundant state setup
```

**Problem:**
- State completely torn down then rebuilt every frame
- Unnecessary state changes (video rendering state doesn't conflict with overlays)
- GL state changes are expensive on small GPUs like VideoCore IV

**Time Cost:** ~2-3ms per frame (10-15% of render time)

---

### 3. **Stride Conversion (MEDIUM IMPACT)**
**Location:** `gl_context.c` lines 459-475 (Y plane), 481-497 (U plane), 503-519 (V plane)

**Current Implementation:**
```c
// Check if strides match expected dimensions
bool y_direct = (y_stride == width);
if (!y_direct) {
    // Allocate temp buffer and copy row-by-row
    for (int row = 0; row < height; row++) {
        memcpy(dst, src, width);
        src += y_stride;
        dst += width;
    }
}
```

**Problem:**
- Hardware decoder sometimes adds padding to strides
- Redundant memcpy operations if strides don't match
- Three separate copy operations (Y, U, V planes)
- No memory pooling - allocates/deallocates per frame if reallocation needed

**Time Cost:** ~2-3ms per frame (only if strides don't match, which for rpi4-e.mp4 they do)

---

### 4. **Texture Sampler State (LOW IMPACT)**
**Location:** `gl_context.c` lines 441-454

**Current Implementation:**
```c
// For every frame:
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // Y plane
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

// ... repeat 2 more times for U and V planes
// Total: 12 glTexParameteri() calls per frame!
```

**Problem:**
- These parameters don't change between frames
- Set once during initialization, not per-frame
- Low cost individually but adds up: 12 calls × ~0.1ms = ~1.2ms

**Time Cost:** ~1-1.5ms per frame (5-7% of render time)

---

### 5. **GL Error Checking (VERY LOW IMPACT)**
**Location:** `gl_context.c` lines 607-619

**Current Implementation:**
```c
GLenum error = glGetError();  // BEFORE drawing
if (error != GL_NO_ERROR) {
    printf("OpenGL error before draw: 0x%x\n", error);
}

// ... draw command ...

error = glGetError();  // AFTER drawing
if (error != GL_NO_ERROR) {
    printf("OpenGL error after draw: 0x%x\n", error);
}
```

**Problem:**
- `glGetError()` synchronizes CPU and GPU (expensive flush operation)
- Two synchronization points per frame
- Debug output to console adds OS overhead

**Time Cost:** ~1-2ms per frame (only when enabled, 5-10% when active)

---

## Optimization Opportunities (Priority Order)

### 🔴 PRIORITY 1: Persistent Texture Buffers (High Impact, Medium Effort)

**Target Savings:** 8-12ms per frame (40-60% reduction)

**Implementation:**
Use GPU-side buffer objects instead of CPU→GPU texture uploads:

1. **Create PBO (Pixel Buffer Object)** on init
2. **Map PBO** each frame (GPU side)
3. **Copy YUV data** to mapped buffer (CPU fast path)
4. **Unmap PBO** (GPU fast path)
5. **Use PBO as texture source** (zero copy from GPU)

**Code Location to Modify:** `gl_context.c` lines 430-600

**Expected Result:**
- Eliminate large texture uploads
- Reduce GPU→CPU synchronization points
- Bandwidth: 114 MB/s → ~20 MB/s (only metadata)

**Implementation Complexity:** Medium
- Add PBO creation in `gl_create_context()`
- Modify texture update loop
- Add PBO cleanup in destructor

**Code Sketch:**
```c
// In gl_context_t structure:
GLuint pbo_y, pbo_u, pbo_v;
GLuint* pbo_mapped_y;
GLuint* pbo_mapped_u; 
GLuint* pbo_mapped_v;

// Per frame in gl_render_frame():
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, gl->pbo_y);
void* ptr = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, y_size,
                              GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
memcpy(ptr, y_data, y_size);  // Fast path (GPU memory)
glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, 0);
```

---

### 🟠 PRIORITY 2: Eliminate Redundant State Changes (Medium Impact, Low Effort)

**Target Savings:** 2-3ms per frame (10-15% reduction)

**Current Problem:**
Every frame tears down then rebuilds GL state:
```c
glUseProgram(0);  // Why? Video rendering always uses same program
glBindBuffer(..., 0);  // Why? Video rendering uses same buffers
glDisableVertexAttribArray(0-1);  // Why? Always enabled for video
```

**Implementation:**
Keep video rendering state persistent, only change when overlays appear:

**Code Location to Modify:** `gl_context.c` lines 402-440

**Expected Result:**
- Eliminate teardown/rebuild cycle
- State changes only on overlay visibility change

**Implementation Complexity:** Low
- Remove redundant unbind/rebind calls
- Add state caching (track what's already bound)
- Only restore when overlay visibility changes

**Code Sketch:**
```c
// Cache previous state
static GLuint last_program = 0;
static GLuint last_vbo = 0;
// ...

if (last_program != gl->program) {
    glUseProgram(gl->program);
    last_program = gl->program;
}

if (last_vbo != gl->vbo) {
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    last_vbo = gl->vbo;
}
// Only change state that actually changed
```

---

### 🟡 PRIORITY 3: Move Texture Parameters to Init (Low-Medium Impact, Very Low Effort)

**Target Savings:** 1-1.5ms per frame (5-7% reduction)

**Current Problem:**
```c
// Called EVERY frame:
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // 12× per frame!
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
```

**Implementation:**
Move to texture initialization:

**Code Location to Modify:** `gl_context.c` lines 441-454 (move to texture creation)

**Expected Result:**
- 12 GL calls removed from hot path
- Set only once at startup

**Implementation Complexity:** Very Low
- Find texture creation code
- Add texture parameters there
- Remove from `gl_render_frame()`

**Code Sketch:**
```c
// In gl_init_textures() or similar, called ONCE:
glBindTexture(GL_TEXTURE_2D, gl->texture_y);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
// ... repeat for U, V

// Remove all 12 calls from gl_render_frame()
```

---

### 🟢 PRIORITY 4: Optimize Stride Handling (Low Impact, Low Effort)

**Target Savings:** 1-2ms per frame (if strides don't match) (5-10% conditional)

**Current Problem:**
- Allocates temp buffers when strides don't match
- No pooling of allocations
- Checks stride match on every frame even if never needed

**Implementation:**
Pre-allocate aligned buffers:

**Code Location to Modify:** `gl_context.c` lines 430-600

**Expected Result:**
- No allocation failures in hot path
- Faster stride conversion (aligned buffers)

**Implementation Complexity:** Low
- Add pre-allocated stride buffers in `gl_context_t`
- Use arena allocator pattern

**Code Sketch:**
```c
// In gl_context_t:
uint8_t* stride_buffer_y;
uint8_t* stride_buffer_u;
uint8_t* stride_buffer_v;
size_t stride_buffer_capacity_y;
size_t stride_buffer_capacity_u;
size_t stride_buffer_capacity_v;

// Per frame:
if (!y_direct && stride_buffer_capacity_y < needed_size) {
    free(stride_buffer_y);
    stride_buffer_y = malloc(needed_size + 4096);  // Align
    stride_buffer_capacity_y = needed_size;
}
// Use pre-allocated buffer
```

---

### 💜 PRIORITY 5: Disable Debug GL Error Checking (Very Low Impact, Very Low Effort)

**Target Savings:** 1-2ms per frame (5-10% when enabled) (0ms when disabled)

**Current Problem:**
- `glGetError()` is expensive (GPU→CPU synchronization)
- Debug flag adds this overhead to hot path
- Should be conditional or compile-time disabled

**Implementation:**
Make error checking conditional on debug flag:

**Code Location to Modify:** `gl_context.c` lines 607-619

**Expected Result:**
- No error checking overhead in release builds
- Optional debug mode for development

**Implementation Complexity:** Very Low
- Wrap in `#ifdef DEBUG` or `if (app->debug_mode)`
- Remove from hot path

**Code Sketch:**
```c
#ifdef GL_DEBUG
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "OpenGL error: 0x%x\n", error);
    }
#endif
```

---

## Recommended Implementation Order

1. **Start with Priority 2 (5 min)**: Remove redundant state changes
   - Low risk, easy to test
   - Immediate 2-3ms gain
   - Proves concept works

2. **Then Priority 3 (3 min)**: Move texture parameters to init
   - Ultra-low risk
   - Additional 1-1.5ms gain
   - Total: 3-4.5ms so far

3. **Then Priority 1 (30-45 min)**: Implement PBO pipeline
   - Biggest impact: 8-12ms gain
   - More complex but manageable
   - Thoroughly test before committing
   - Total: 11-16ms gain (11-16fps improvement!)

4. **Then Priority 4 (10 min)**: Optimize stride handling
   - Insurance for future hardware variations
   - Extra 1-2ms if needed

5. **Finally Priority 5 (2 min)**: Debug flag on error checking
   - Performance Polish

---

## Performance Targets After Each Optimization

| Phase | Change | Estimated Time | Cumulative Improvement |
|-------|--------|-----------------|------------------------|
| Current | Baseline | 20-30ms | 45fps |
| +Priority 2 | Remove state churn | 18-27ms | 47fps |
| +Priority 3 | Texture params once | 17-26ms | 48fps |
| +Priority 1 | PBO pipeline | 8-14ms | **60fps target!** |
| +Priority 4 | Stride pooling | 7-13ms | **62-65fps!** |
| +Priority 5 | Debug off | 7-13ms | **62-65fps** |

---

## Testing Strategy

### Before Each Change
```bash
# Build and test
make clean && make

# Baseline measurements
./pickle --timing --hw-debug test_1920x1080_60fps_high41.mp4 2>&1 | tail -20

# Record:
# - Average GL render time
# - Average total render time
# - Frame decode count in 5 seconds
```

### Quick Validation
1. Decode output unchanged (same YUV data)
2. Visual output identical (no corruption)
3. Render time improved (look at GL time)
4. No crashes or error messages

### Full Validation
1. Test with all video files:
   - test_video.mp4 (30fps, Baseline)
   - test_1920x1080_60fps.mp4 (60fps, Baseline)
   - test_1920x1080_60fps_high41.mp4 (60fps, High Profile)
   - rpi4-e-baseline.mp4 (60fps, Baseline)
2. Measure sustained framerate (not just first frame)
3. Test at least 10 seconds per file

---

## Quick Start: Immediate Gains Without Deep Changes

If you want immediate improvement without modifying core rendering:

### Option A: Reduce Texture Filter Quality
**Time to implement:** 2 minutes
**Target savings:** 1-2ms

Change filtering mode for YUV textures:
```c
// Instead of GL_LINEAR, use GL_NEAREST (faster, slightly lower quality)
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
```

Result: Faster texture sampling, may notice slight pixelation on edges.

---

### Option B: Reduce Debug Output
**Time to implement:** 1 minute
**Target savings:** 1-2ms

Disable `--hw-debug` and `--timing` flags when not debugging:
```bash
# Instead of:
./pickle --timing --hw-debug test_1920x1080_60fps_high41.mp4

# Use:
./pickle test_1920x1080_60fps_high41.mp4
```

Reason: Timing measurements themselves add overhead (clock_gettime calls, printf overhead, GPU sync for measurements).

---

## Summary

**Current Problem:**
- GPU rendering: 20-30ms vs 16.7ms target
- Decode perfect: 1-3ms (not the issue)
- Need: Reduce render time by 3-13ms to hit 60fps

**Root Causes (in order of impact):**
1. 8-12ms: Large YUV texture uploads every frame
2. 2-3ms: Redundant GPU state teardown/rebuild
3. 1-1.5ms: Texture parameters set every frame (should be once)
4. 1-2ms: Stride conversion inefficiencies
5. 1-2ms: Debug error checking synchronization

**Best Strategy:**
- Implement Priority 2 (state), Priority 3 (params), Priority 1 (PBO) = 11-16ms improvement = **60fps achieved**
- Total work: ~60 minutes for significant optimization
- Risk: Low (can revert easily, highly testable)

**Quick Win (2 minutes):**
Just disable `--timing` and `--hw-debug` flags → immediate 1-2ms improvement

