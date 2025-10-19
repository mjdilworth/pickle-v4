# GPU Rendering Optimization: Implementation Roadmap

## Summary of Changes Needed

| Priority | Issue | Fix | File | Lines | Effort | Impact |
|----------|-------|-----|------|-------|--------|--------|
| 1 | Redundant GL state management | Remove unbind/rebind cycle | gl_context.c | 402-440 | 10 min | 2-3ms |
| 2 | Texture params set every frame | Move to init | gl_context.c | 441-454 | 5 min | 1-1.5ms |
| 3 | Large YUV uploads (bottleneck) | Implement PBO pipeline | gl_context.c | 300+ | 45 min | 8-12ms |
| 4 | Stride conversion inefficiency | Pre-allocate buffers | gl_context.c | 430-600 | 10 min | 1-2ms |
| 5 | Debug sync overhead | Conditional GL errors | gl_context.c | 607-619 | 2 min | 1-2ms |

---

## Phase 1: Quick Wins (15 minutes, 3-4.5ms improvement)

### Change 1: Remove Redundant State Teardown (gl_context.c lines 402-440)

**Current Code:**
```c
// CRITICAL: Complete buffer unbinding before state restoration
// This is needed to prevent state leakage from previous operations
glBindBuffer(GL_ARRAY_BUFFER, 0);
glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
glUseProgram(0);

// Reset vertex attribute arrays
glDisableVertexAttribArray(0);
glDisableVertexAttribArray(1);

// Now set up the correct state for video rendering
glUseProgram(gl->program);
glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);
```

**New Code (with state caching):**
```c
// Add to gl_context_t struct in gl_context.h:
// static GLuint cached_program = 0;
// static GLuint cached_vbo = 0;

// In gl_render_frame():
// Skip state setup for video rendering - it's always the same
// Only restore if overlays changed GL state (detected by app->keystone->show_*)

// State is set once at init, only restore when needed
if (gl->state_was_corrupted_by_overlays) {
    glUseProgram(gl->program);
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    gl->state_was_corrupted_by_overlays = false;
}
```

**Remove these 8 redundant calls from hot path.**

---

### Change 2: Move Texture Parameters to Initialization

**Current Code (lines 441-454):**
```c
// For every frame - WASTE!
glActiveTexture(GL_TEXTURE0);
glBindTexture(GL_TEXTURE_2D, gl->texture_y);

// Set proper texture parameters (might have been changed by overlays)
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

glActiveTexture(GL_TEXTURE1);
glBindTexture(GL_TEXTURE_2D, gl->texture_u);

// Set proper texture parameters
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

glActiveTexture(GL_TEXTURE2);
glBindTexture(GL_TEXTURE_2D, gl->texture_v);

// Set proper texture parameters
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

// Reset to texture unit 0
glActiveTexture(GL_TEXTURE0);
```

**Find where textures are created** (probably `gl_context.c` early initialization) and add:

```c
// Called ONCE during init (find this function or create it):
static void init_texture_parameters(GLuint texture) {
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// In init code:
init_texture_parameters(gl->texture_y);
init_texture_parameters(gl->texture_u);
init_texture_parameters(gl->texture_v);
```

**Then remove all 12 glTexParameteri calls from gl_render_frame().**

---

## Phase 2: Major Optimization (45 minutes, 8-12ms improvement)

### Change 3: Implement Pixel Buffer Object (PBO) Pipeline

This is the big one. Current approach uploads ~1.9 MB per frame from CPU to GPU.
PBO approach uses GPU-side memory for faster transfers.

**Steps:**

1. **Add PBO objects to gl_context_t (gl_context.h):**
```c
// In gl_context_t struct:
GLuint pbo_y;
GLuint pbo_u;
GLuint pbo_v;
```

2. **Create PBOs in initialization (in gl_context.c, find init function):**
```c
// Add to gl_init or similar:
GLsizeiptr y_size = width * height;
GLsizeiptr uv_size = (width/2) * (height/2);

glGenBuffers(1, &gl->pbo_y);
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, gl->pbo_y);
glBufferData(GL_PIXEL_UNPACK_BUFFER, y_size, NULL, GL_DYNAMIC_DRAW);

glGenBuffers(1, &gl->pbo_u);
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, gl->pbo_u);
glBufferData(GL_PIXEL_UNPACK_BUFFER, uv_size, NULL, GL_DYNAMIC_DRAW);

glGenBuffers(1, &gl->pbo_v);
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, gl->pbo_v);
glBufferData(GL_PIXEL_UNPACK_BUFFER, uv_size, NULL, GL_DYNAMIC_DRAW);

glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);  // Unbind
```

3. **In gl_render_frame() (lines 430-600), replace texture upload section:**

```c
// OLD: Direct CPU→GPU transfer
glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, y_data);

// NEW: PBO path
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, gl->pbo_y);
void* ptr = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, width * height,
                              GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
if (ptr) {
    if (y_direct) {
        memcpy(ptr, y_data, width * height);
    } else {
        // Stride conversion into GPU memory
        uint8_t *src = y_data, *dst = (uint8_t*)ptr;
        for (int row = 0; row < height; row++) {
            memcpy(dst, src, width);
            src += y_stride;
            dst += width;
        }
    }
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
}
glBindTexture(GL_TEXTURE_2D, gl->texture_y);
glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, 0);
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

// Repeat for U and V planes
```

4. **Cleanup in destructor:**
```c
glDeleteBuffers(1, &gl->pbo_y);
glDeleteBuffers(1, &gl->pbo_u);
glDeleteBuffers(1, &gl->pbo_v);
```

---

## Phase 3: Polish (12 minutes, 2-4ms improvement)

### Change 4: Pre-allocate Stride Buffers

**Location:** gl_context.c lines 430-600 (stride conversion section)

```c
// Add to gl_context_t:
uint8_t* stride_buffer_y;
uint8_t* stride_buffer_u;
uint8_t* stride_buffer_v;
size_t stride_alloc_size_y;
size_t stride_alloc_size_u;
size_t stride_alloc_size_v;

// In init:
gl->stride_buffer_y = NULL;
gl->stride_buffer_u = NULL;
gl->stride_buffer_v = NULL;
gl->stride_alloc_size_y = 0;
gl->stride_alloc_size_u = 0;
gl->stride_alloc_size_v = 0;

// Per frame:
int y_needed = width * height;
if (gl->stride_alloc_size_y < y_needed) {
    free(gl->stride_buffer_y);
    gl->stride_buffer_y = malloc(y_needed + 1024);  // Aligned buffer
    gl->stride_alloc_size_y = y_needed;
}
// Use gl->stride_buffer_y instead of allocating new
```

---

### Change 5: Conditional GL Error Checking

**Location:** gl_context.c lines 607-619

```c
// Before:
GLenum error = glGetError();
if (error != GL_NO_ERROR) {
    printf("OpenGL error before draw: 0x%x\n", error);
}

// After:
#ifdef GL_DEBUG
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "OpenGL error: 0x%x\n", error);
    }
#endif
```

---

## Testing Checklist

After each phase:

- [ ] Code compiles without errors
- [ ] No new warnings
- [ ] Visual output unchanged
- [ ] First frame renders correctly
- [ ] Sustained playback works (10+ seconds)
- [ ] YUV data valid (pointers non-NULL)
- [ ] GL render time improved
- [ ] No screen artifacts or corruption
- [ ] Test with multiple video files
- [ ] Performance measured with --timing flag

---

## Expected Progression

```
Baseline:        20-30ms render (45fps)
After Phase 1:   17-26ms render (47fps) +++ Quick wins
After Phase 2:   8-14ms render (60+fps)  +++ GOAL ACHIEVED
After Phase 3:   7-13ms render (65+fps)  +++ Over target
```

---

## Rollback Strategy

Each change is independent. If something breaks:

1. Revert the last change
2. Run `make clean && make`
3. Test baseline video
4. Verify render times back to expected

---

## Key Insights

**Why Render is Slow:**
- 1.9 MB YUV data uploaded every frame = bottleneck
- Raspberry Pi GPU memory bandwidth shared with CPU
- 114 MB/s at 60fps is 20% of available bandwidth

**Why PBO Helps:**
- GPU-side buffer = no CPU→GPU transfer
- Data already in GPU memory = fast GPU operations
- Bandwidth: 114 MB/s → 20 MB/s (metadata only)

**Why State Management Matters:**
- Redundant GL calls add up on small GPUs
- VideoCore IV less efficient than desktop GPUs
- State caching prevents wasteful rebinds

**Why This Works on Raspberry Pi:**
- V4L2 M2M hardware decode already working (1-3ms)
- Bottleneck is GPU rendering (what we're fixing)
- PBO approach is well-supported on modern OpenGL ES

