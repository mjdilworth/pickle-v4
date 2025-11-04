#define _POSIX_C_SOURCE 199309L
                if (is_sel) {
                    // Draw only an opaque border (no fill) for crisp selection
                    glDisable(GL_BLEND);
static inline void copy_with_stride_simd(uint8_t *dst, const uint8_t *src, 
                                         int width, int height, 
                                         int dst_stride, int src_stride) {
    #if HAS_NEON
    // Use NEON 128-bit vectors for 16-byte copies (2 NEON registers per iteration)
    int width_16 = (width / 16) * 16;  // Process 16 bytes at a time
    
    for (int row = 0; row < height; row++) {
        // NEON fast path for 16-byte aligned chunks
        uint8_t *s = (uint8_t *)src + (row * src_stride);
        uint8_t *d = dst + (row * dst_stride);
        
        for (int col = 0; col < width_16; col += 16) {
            uint8x16_t data = vld1q_u8(s + col);
            vst1q_u8(d + col, data);
        }
        
        // Scalar copy for remaining bytes
        memcpy(d + width_16, s + width_16, width - width_16);
    }
    #else
    // Fallback to standard memcpy loop for non-ARM platforms
    for (int row = 0; row < height; row++) {
        memcpy(dst + (row * dst_stride), src + (row * src_stride), width);
    }
    #endif
}

// Forward declarations
static void draw_char_simple(float *vertices, int *vertex_count, char c, float x, float y, float size);
static void draw_text_simple(float *vertices, int *vertex_count, const char *text, float x, float y, float size);

// OpenGL ES 3.0 constants for single-channel textures
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RED  
#define GL_RED 0x1903
#endif

// Pre-allocated YUV temp buffers for stride handling (avoids malloc/free each frame)
typedef struct {
    uint8_t *y_temp_buffer;
    uint8_t *u_temp_buffer;
    uint8_t *v_temp_buffer;
    int allocated_size;  // Size each buffer is allocated for
} yuv_temp_buffers_t;

// Global persistent buffers - allocated once, reused
static yuv_temp_buffers_t g_yuv_buffers = {NULL, NULL, NULL, 0};

static void allocate_yuv_buffers(int needed_size) {
    // Only allocate if needed or size changed
    if (g_yuv_buffers.allocated_size < needed_size) {
        // Free old buffers if they exist
        free(g_yuv_buffers.y_temp_buffer);
        free(g_yuv_buffers.u_temp_buffer);
        free(g_yuv_buffers.v_temp_buffer);
        
        // Allocate new buffers - allocate slightly larger to reduce reallocations
        int alloc_size = needed_size * 1.2;  // 20% headroom
        g_yuv_buffers.y_temp_buffer = malloc(alloc_size);
        g_yuv_buffers.u_temp_buffer = malloc(alloc_size / 4);  // U/V are half size
        g_yuv_buffers.v_temp_buffer = malloc(alloc_size / 4);
        g_yuv_buffers.allocated_size = alloc_size;
        
        if (g_yuv_buffers.y_temp_buffer && g_yuv_buffers.u_temp_buffer && g_yuv_buffers.v_temp_buffer) {
            // printf("DEBUG: Pre-allocated YUV buffers: Y=%p U=%p V=%p (size %d)\n",
            //        g_yuv_buffers.y_temp_buffer, g_yuv_buffers.u_temp_buffer, g_yuv_buffers.v_temp_buffer, alloc_size);
        }
    }
}

static void free_yuv_buffers(void) {
    if (g_yuv_buffers.y_temp_buffer) {
        free(g_yuv_buffers.y_temp_buffer);
        g_yuv_buffers.y_temp_buffer = NULL;
    }
    if (g_yuv_buffers.u_temp_buffer) {
        free(g_yuv_buffers.u_temp_buffer);
        g_yuv_buffers.u_temp_buffer = NULL;
    }
    if (g_yuv_buffers.v_temp_buffer) {
        free(g_yuv_buffers.v_temp_buffer);
        g_yuv_buffers.v_temp_buffer = NULL;
    }
    g_yuv_buffers.allocated_size = 0;
}

// Modern vertex shader for keystone correction and video rendering (OpenGL ES 3.1)
// OPTIMIZATION: Use highp for geometry (precision needed for matrix math)
const char *vertex_shader_source = 
    "#version 310 es\n"
    "precision highp float;\n"
    "\n"
    "// Vertex attributes with explicit locations\n"
    "layout(location = 0) in vec2 a_position;\n"
    "layout(location = 1) in vec2 a_texcoord;\n"
    "\n"
    "// Legacy uniforms for compatibility\n"
    "uniform mat4 u_mvp_matrix;\n"
    "uniform mat4 u_keystone_matrix;\n"
    "\n"
    "// Output to fragment shader\n"
    "out vec2 v_texcoord;\n"
    "\n"
    "void main() {\n"
    "    // Apply keystone correction first, then MVP transformation\n"
    "    vec4 pos = vec4(a_position, 0.0, 1.0);\n"
    "    pos = u_keystone_matrix * pos;\n"
    "    gl_Position = u_mvp_matrix * pos;\n"
    "    \n"
    "    // Pass texture coordinates to fragment shader\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";

// Optimized YUV→RGB conversion fragment shader (OpenGL ES 3.1)
// Keep highp precision for color accuracy (mediump causes color shift)
const char *fragment_shader_source = 
    "#version 310 es\n"
    "precision highp float;\n"  // KEEP highp for color accuracy
    "in vec2 v_texcoord;\n"
    "\n"
    "// Legacy uniform samplers (for compatibility)\n"
    "uniform sampler2D u_texture_y;\n"
    "uniform sampler2D u_texture_u;\n"
    "uniform sampler2D u_texture_v;\n"
    "\n"
    "out vec4 fragColor;\n"
    "\n"
    "void main() {\n"
    "    // Sample YUV values from separate planes\n"
    "    float y = texture(u_texture_y, v_texcoord).r;\n"
    "    float u = texture(u_texture_u, v_texcoord).r;\n"
    "    float v = texture(u_texture_v, v_texcoord).r;\n"
    "    \n"
    "    // BT.709 TV range (16-235 for Y, 16-240 for UV) to RGB conversion\n"
    "    // First expand from TV range to full range\n"
    "    y = (y * 255.0 - 16.0) / 219.0;\n"
    "    u = (u * 255.0 - 16.0) / 224.0;\n"
    "    v = (v * 255.0 - 16.0) / 224.0;\n"
    "    \n"
    "    // BT.709 YUV to RGB conversion matrix\n"
    "    float r = y + 1.5748 * (v - 0.5);\n"
    "    float g = y - 0.1873 * (u - 0.5) - 0.4681 * (v - 0.5);\n"
    "    float b = y + 1.8556 * (u - 0.5);\n"
    "    \n"
    "    // Clamp values to valid range\n"
    "    r = clamp(r, 0.0, 1.0);\n"
    "    g = clamp(g, 0.0, 1.0);\n"
    "    b = clamp(b, 0.0, 1.0);\n"
    "    \n"
    "    fragColor = vec4(r, g, b, 1.0);\n"
    "}\n";

// Corner highlight shaders (OpenGL ES 3.1) - OPTIMIZED
const char *corner_vertex_shader_source = 
    "#version 310 es\n"
    "precision mediump float;\n"  // OPTIMIZED: mediump for overlay geometry
    "layout(location = 0) in vec2 a_position;\n"
    "layout(location = 1) in vec4 a_color;\n"
    "uniform mat4 u_mvp_matrix;\n"
    "out vec4 v_color;\n"
    "void main() {\n"
    "    gl_Position = u_mvp_matrix * vec4(a_position, 0.0, 1.0);\n"
    "    v_color = a_color;\n"
    "}\n";

const char *corner_fragment_shader_source = 
    "#version 310 es\n"
    "precision mediump float;\n"  // OPTIMIZED: mediump for overlay colors
    "in vec4 v_color;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = v_color;\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint length;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        char *log = malloc(length);
        glGetShaderInfoLog(shader, length, NULL, log);
        fprintf(stderr, "Shader compilation failed: %s\n", log);
        free(log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static int create_program(gl_context_t *gl) {
    gl->vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    if (!gl->vertex_shader) return -1;

    gl->fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
    if (!gl->fragment_shader) {
        glDeleteShader(gl->vertex_shader);
        return -1;
    }

    gl->program = glCreateProgram();
    glAttachShader(gl->program, gl->vertex_shader);
    glAttachShader(gl->program, gl->fragment_shader);
    glLinkProgram(gl->program);

    GLint linked;
    glGetProgramiv(gl->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint length;
        glGetProgramiv(gl->program, GL_INFO_LOG_LENGTH, &length);
        char *log = malloc(length);
        glGetProgramInfoLog(gl->program, length, NULL, log);
        fprintf(stderr, "Program linking failed: %s\n", log);
        free(log);
        glDeleteProgram(gl->program);
        glDeleteShader(gl->vertex_shader);
        glDeleteShader(gl->fragment_shader);
        return -1;
    }

    // Get uniform and attribute locations
    gl->u_mvp_matrix = glGetUniformLocation(gl->program, "u_mvp_matrix");
    gl->u_texture_y = glGetUniformLocation(gl->program, "u_texture_y");
    gl->u_texture_u = glGetUniformLocation(gl->program, "u_texture_u");
    gl->u_texture_v = glGetUniformLocation(gl->program, "u_texture_v");
    gl->u_texture_nv12 = glGetUniformLocation(gl->program, "u_texture_nv12");
    gl->u_keystone_matrix = glGetUniformLocation(gl->program, "u_keystone_matrix");
    gl->a_position = glGetAttribLocation(gl->program, "a_position");
    gl->a_texcoord = glGetAttribLocation(gl->program, "a_texcoord");

    return 0;
}

static int create_corner_program(gl_context_t *gl) {
    GLuint corner_vertex_shader = compile_shader(GL_VERTEX_SHADER, corner_vertex_shader_source);
    if (!corner_vertex_shader) return -1;

    GLuint corner_fragment_shader = compile_shader(GL_FRAGMENT_SHADER, corner_fragment_shader_source);
    if (!corner_fragment_shader) {
        glDeleteShader(corner_vertex_shader);
        return -1;
    }

    gl->corner_program = glCreateProgram();
    glAttachShader(gl->corner_program, corner_vertex_shader);
    glAttachShader(gl->corner_program, corner_fragment_shader);
    glLinkProgram(gl->corner_program);

    GLint linked;
    glGetProgramiv(gl->corner_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint length;
        glGetProgramiv(gl->corner_program, GL_INFO_LOG_LENGTH, &length);
        char *log = malloc(length);
        glGetProgramInfoLog(gl->corner_program, length, NULL, log);
        fprintf(stderr, "Corner program linking failed: %s\n", log);
        free(log);
        glDeleteProgram(gl->corner_program);
        glDeleteShader(corner_vertex_shader);
        glDeleteShader(corner_fragment_shader);
        return -1;
    }

    // Get uniform and attribute locations for corner rendering
    gl->corner_u_mvp_matrix = glGetUniformLocation(gl->corner_program, "u_mvp_matrix");
    gl->corner_u_color = glGetUniformLocation(gl->corner_program, "u_color");
    gl->corner_a_position = glGetAttribLocation(gl->corner_program, "a_position");

    // Clean up shaders (they're linked into the program now)
    glDeleteShader(corner_vertex_shader);
    glDeleteShader(corner_fragment_shader);

    return 0;
}

int gl_init(gl_context_t *gl, display_ctx_t *drm) {
    memset(gl, 0, sizeof(*gl));

    // Get EGL display
    gl->egl_display = eglGetDisplay((EGLNativeDisplayType)drm->gbm_device);
    if (gl->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        return -1;
    }

    // Initialize EGL
    EGLint major, minor;
    if (!eglInitialize(gl->egl_display, &major, &minor)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        return -1;
    }

    printf("EGL version: %d.%d\n", major, minor);

    // Bind OpenGL ES API
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "Failed to bind OpenGL ES API\n");
        eglTerminate(gl->egl_display);
        return -1;
    }

    // Choose EGL config
    EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };

    EGLint num_configs;
    if (!eglChooseConfig(gl->egl_display, config_attrs, &gl->egl_config, 1, &num_configs)) {
        fprintf(stderr, "Failed to choose EGL config\n");
        eglTerminate(gl->egl_display);
        return -1;
    }

    // Create EGL context for OpenGL ES 3.1
    EGLint context_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 1,
        EGL_NONE
    };

    gl->egl_context = eglCreateContext(gl->egl_display, gl->egl_config, EGL_NO_CONTEXT, context_attrs);
    if (gl->egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        eglTerminate(gl->egl_display);
        return -1;
    }

    // Create EGL surface
    gl->egl_surface = eglCreateWindowSurface(gl->egl_display, gl->egl_config, 
                                            (EGLNativeWindowType)drm->gbm_surface, NULL);
    if (gl->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "Failed to create EGL surface\n");
        eglDestroyContext(gl->egl_display, gl->egl_context);
        eglTerminate(gl->egl_display);
        return -1;
    }

    // Make current
    if (!eglMakeCurrent(gl->egl_display, gl->egl_surface, gl->egl_surface, gl->egl_context)) {
        fprintf(stderr, "Failed to make EGL context current\n");
        eglDestroySurface(gl->egl_display, gl->egl_surface);
        eglDestroyContext(gl->egl_display, gl->egl_context);
        eglTerminate(gl->egl_display);
        return -1;
    }

    // OPTIMIZATION: Configure VSync for smooth frame pacing
    // Set swap interval to 1 for VSync (reduces buffer swap timing variance)
    if (!eglSwapInterval(gl->egl_display, 1)) {
        printf("Warning: Could not enable VSync (swap interval), may have frame timing issues\n");
    } else {
        printf("VSync enabled for smooth frame pacing\n");
    }

    // Create shaders and program
    if (create_program(gl) != 0) {
        gl_cleanup(gl);
        return -1;
    }

    // Create corner rendering program
    if (create_corner_program(gl) != 0) {
        gl_cleanup(gl);
        return -1;
    }

    // Initialize PBO for async texture uploads (OpenGL ES 3.0+)
    // DISABLED: Direct upload is faster on RPi4 when strides match (5-8ms vs 12-17ms with PBO overhead)
    gl->use_pbo = false;
    glGenBuffers(3, gl->pbo);
    // printf("PBO async texture uploads enabled\n");

    // OpenGL ES initialization complete
    return 0;
}

void gl_setup_buffers(gl_context_t *gl) {
    // Quad vertices (position + texture coordinates)  
    float vertices[] = {
        // Position  // TexCoord (standard orientation)
        -1.0f, -1.0f, 0.0f, 0.0f,  // Bottom-left
         1.0f, -1.0f, 1.0f, 0.0f,  // Bottom-right
         1.0f,  1.0f, 1.0f, 1.0f,  // Top-right
        -1.0f,  1.0f, 0.0f, 1.0f   // Top-left
    };

    GLuint indices[] = {
        0, 1, 2,  // First triangle
        2, 3, 0   // Second triangle
    };

    // Generate and bind VBO (no VAO for compatibility)
    glGenBuffers(1, &gl->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // Generate and bind EBO
    glGenBuffers(1, &gl->ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // Store for later use - don't set attributes here since no VAO

    // Generate YUV textures
    glGenTextures(1, &gl->texture_y);
    glGenTextures(1, &gl->texture_u);
    glGenTextures(1, &gl->texture_v);
    glGenTextures(1, &gl->texture_nv12);
    
    // Setup Y texture
    glBindTexture(GL_TEXTURE_2D, gl->texture_y);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Setup U texture
    glBindTexture(GL_TEXTURE_2D, gl->texture_u);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Setup V texture
    glBindTexture(GL_TEXTURE_2D, gl->texture_v);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Setup NV12 texture (combined Y + interleaved UV at 1.5x height)
    glBindTexture(GL_TEXTURE_2D, gl->texture_nv12);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, gl->texture_v);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Setup corner highlight VBO - will be updated with actual corner positions
    glGenBuffers(1, &gl->corner_vbo);
    
    // Setup border VBO - will be updated with actual border positions
    glGenBuffers(1, &gl->border_vbo);
    
    // Setup help overlay VBO - will be updated with help overlay geometry
    glGenBuffers(1, &gl->help_vbo);
    
    // Modern ES 3.1 UBO setup (disabled for compatibility)
    // setup_uniform_buffers(gl);
}

// Helper function to upload NV12 data to GPU
void gl_render_nv12(gl_context_t *gl, uint8_t *nv12_data, int width, int height, int stride,
                    display_ctx_t *drm, keystone_context_t *keystone) {
    (void)stride; // Unused parameter - kept for API consistency
    static int frame_rendered = 0;
    static int last_width = 0, last_height = 0;
    static bool gl_state_set = false;
    
    // Set black clear color for video background
    if (frame_rendered == 0) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    }
    
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Set up OpenGL state only once or when needed
    if (!gl_state_set) {
        glViewport(0, 0, drm->width, drm->height);
        gl_state_set = true;
    }
    
    // CRITICAL: Complete buffer unbinding before state restoration
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glUseProgram(0);
    
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    
    // Now set up the correct state for video rendering
    glUseProgram(gl->program);
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);
    
    // Position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    // Texture coordinate attribute  
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Set MVP matrix
    float mvp_matrix[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->u_mvp_matrix, 1, GL_FALSE, mvp_matrix);
    
    // Explicitly disable blending and depth test
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    
    // Set keystone matrix
    const float *keystone_matrix = keystone_get_matrix(keystone);
    glUniformMatrix4fv(gl->u_keystone_matrix, 1, GL_FALSE, keystone_matrix);
    
    // Bind NV12 texture to texture unit 0
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl->texture_nv12);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Disable other texture units so shader uses only NV12
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    
    // Set sampler uniforms
    glUniform1i(gl->u_texture_nv12, 0);  // NV12 on unit 0
    glUniform1i(gl->u_texture_y, 1);     // Unused (should help shader detect NV12 mode)
    glUniform1i(gl->u_texture_u, 2);     // Unused
    glUniform1i(gl->u_texture_v, 3);     // Unused
    
    // Upload NV12 data
    if (nv12_data) {
        bool size_changed = (width != last_width || height != last_height || frame_rendered == 0);
        
        // NV12 is Y plane + interleaved UV plane = 1.5x height
        int nv12_height = (height * 3) / 2;
        
        if (size_changed) {
            // Need full glTexImage2D for size change
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, nv12_height, 0, GL_RED, GL_UNSIGNED_BYTE, nv12_data);
            printf("NV12 texture uploaded: %dx%d (NV12 height %d)\n", width, height, nv12_height);
        } else {
            // Use glTexSubImage2D for faster updates
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, nv12_height, GL_RED, GL_UNSIGNED_BYTE, nv12_data);
        }
        
        last_width = width;
        last_height = height;
        frame_rendered++;
    }
    
    // Check for OpenGL errors
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        printf("OpenGL error (NV12): 0x%x\n", error);
    }
    
    // Draw quad
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
    error = glGetError();
    if (error != GL_NO_ERROR) {
        printf("OpenGL error after NV12 draw: 0x%x\n", error);
    }
}

void gl_render_frame(gl_context_t *gl, uint8_t *y_data, uint8_t *u_data, uint8_t *v_data, 
                    int width, int height, int y_stride, int u_stride, int v_stride,
                    display_ctx_t *drm, keystone_context_t *keystone) {
    static int frame_rendered = 0;
    static int last_width = 0, last_height = 0;
    static bool gl_state_set = false;
    
    // Handle stride for YUV data with potential padding
    
    // Set black clear color for video background
    if (frame_rendered == 0) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // Black background
    }
    
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Set up OpenGL state only once or when needed
    if (!gl_state_set) {
        glViewport(0, 0, drm->width, drm->height);
        gl_state_set = true;
    }
    
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
    
    // Position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    // Texture coordinate attribute  
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Set MVP matrix (identity - only once)
    float mvp_matrix[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->u_mvp_matrix, 1, GL_FALSE, mvp_matrix);
    
    // Explicitly disable blending - overlays might have enabled it
    glDisable(GL_BLEND);
    
    // Disable depth test - make sure video is always visible
    glDisable(GL_DEPTH_TEST);
    
    // Set keystone matrix (may change dynamically)
    const float *keystone_matrix = keystone_get_matrix(keystone);
    glUniformMatrix4fv(gl->u_keystone_matrix, 1, GL_FALSE, keystone_matrix);
    
    // Restore texture units and samplers (critical for video rendering)
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
    
    // Set texture sampler uniforms
    glUniform1i(gl->u_texture_y, 0);
    glUniform1i(gl->u_texture_u, 1);
    glUniform1i(gl->u_texture_v, 2);
    
    // Update YUV textures if video data is provided
    if (y_data && u_data && v_data) {
        // Calculate UV dimensions (usually half resolution for 4:2:0)
        int uv_width = width / 2;
        int uv_height = height / 2;
        
        // OPTIMIZED: Check if we can use direct upload for all planes
        bool y_direct = (y_stride == width);
        bool u_direct = (u_stride == uv_width);
        bool v_direct = (v_stride == uv_width);
        bool size_changed = (width != last_width || height != last_height || frame_rendered == 0);
        
        if (size_changed) {
            printf("YUV strides: Y=%d U=%d V=%d (dimensions: %dx%d, UV: %dx%d)\n", 
                   y_stride, u_stride, v_stride, width, height, uv_width, uv_height);
            printf("Direct upload: Y=%s U=%s V=%s\n", y_direct?"YES":"NO", u_direct?"YES":"NO", v_direct?"YES":"NO");
        }
        
        // Y texture - OPTIMIZED: Use PBO for async DMA transfer or direct upload
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gl->texture_y);
        
        if (gl->use_pbo && y_direct) {
            // PBO async upload path (fastest for direct data)
            int y_size = width * height;
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, gl->pbo[0]);
            
            if (size_changed) {
                glBufferData(GL_PIXEL_UNPACK_BUFFER, y_size, NULL, GL_STREAM_DRAW);
            }
            
            // Map buffer and copy data
            void *pbo_mem = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, y_size, 
                                             GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
            if (pbo_mem) {
                memcpy(pbo_mem, y_data, y_size);
                glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                
                // Upload from PBO (async DMA)
                if (size_changed) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, 0);
                } else {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, 0);
                }
            }
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        } else if (y_direct) {
            // Direct upload (no PBO)
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, y_data);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, y_data);
            }
        } else {
            // Stride copy path (no PBO for non-direct data)
            int needed_size = width * height;
            allocate_yuv_buffers(needed_size);
            
            copy_with_stride_simd(g_yuv_buffers.y_temp_buffer, y_data, width, height, width, y_stride);
            
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, g_yuv_buffers.y_temp_buffer);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, g_yuv_buffers.y_temp_buffer);
            }
        }
        // U texture - OPTIMIZED: Use PBO for async DMA transfer or direct upload
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gl->texture_u);
        
        if (gl->use_pbo && u_direct) {
            // PBO async upload path
            int u_size = uv_width * uv_height;
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, gl->pbo[1]);
            
            if (size_changed) {
                glBufferData(GL_PIXEL_UNPACK_BUFFER, u_size, NULL, GL_STREAM_DRAW);
            }
            
            void *pbo_mem = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, u_size,
                                             GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
            if (pbo_mem) {
                memcpy(pbo_mem, u_data, u_size);
                glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                
                if (size_changed) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, 0);
                } else {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, 0);
                }
            }
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        } else if (u_direct) {
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, u_data);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, u_data);
            }
        } else {
            // Use pre-allocated buffers for U plane
            // OPTIMIZED: Use SIMD-accelerated stride copy (NEON on ARM)
            int needed_size = uv_width * uv_height;
            allocate_yuv_buffers(needed_size);
            
            copy_with_stride_simd(g_yuv_buffers.u_temp_buffer, u_data, uv_width, uv_height, uv_width, u_stride);
            
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, g_yuv_buffers.u_temp_buffer);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, g_yuv_buffers.u_temp_buffer);
            }
        }
        
        // V texture - OPTIMIZED: Use PBO for async DMA transfer or direct upload
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, gl->texture_v);
        
        if (gl->use_pbo && v_direct) {
            // PBO async upload path
            int v_size = uv_width * uv_height;
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, gl->pbo[2]);
            
            if (size_changed) {
                glBufferData(GL_PIXEL_UNPACK_BUFFER, v_size, NULL, GL_STREAM_DRAW);
            }
            
            void *pbo_mem = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, v_size,
                                             GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
            if (pbo_mem) {
                memcpy(pbo_mem, v_data, v_size);
                glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                
                if (size_changed) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, 0);
                } else {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, 0);
                }
            }
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        } else if (v_direct) {
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, v_data);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, v_data);
            }
        } else {
            // Use pre-allocated buffers for V plane
            // OPTIMIZED: Use SIMD-accelerated stride copy (NEON on ARM)
            int needed_size = uv_width * uv_height;
            allocate_yuv_buffers(needed_size);
            
            copy_with_stride_simd(g_yuv_buffers.v_temp_buffer, v_data, uv_width, uv_height, uv_width, v_stride);
            
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, g_yuv_buffers.v_temp_buffer);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, g_yuv_buffers.v_temp_buffer);
            }
        }
        
        if (frame_rendered == 0) {
            printf("GPU YUV→RGB rendering started (%dx%d)\n", width, height);
        }
        last_width = width;
        last_height = height;
        
        frame_rendered++;
    } else if (frame_rendered == 0) {
        // Skip test pattern for now - YUV textures need proper initialization
        printf("Waiting for video data...\n");
    }
    
    // Check for OpenGL errors before drawing
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        printf("OpenGL error before draw: 0x%x\n", error);
    }
    
    // Draw quad
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
    // Check for OpenGL errors after drawing
    error = glGetError();
    if (error != GL_NO_ERROR) {
        printf("OpenGL error after draw: 0x%x\n", error);
    }
}
void gl_swap_buffers(gl_context_t *gl, struct display_ctx *drm) {
    static int swap_count = 0;
    static struct timespec last_swap_time = {0, 0};
    struct timespec t1, t2;
    
    // Measure swap time for diagnostics
    clock_gettime(CLOCK_MONOTONIC, &t1);
    
    // EGL swap buffers (this makes the rendered frame available and blocks on VSync)
    EGLBoolean swap_result = eglSwapBuffers(gl->egl_display, gl->egl_surface);
    if (!swap_result && swap_count < 5) {
        printf("EGL swap failed: 0x%x\n", eglGetError());
        clock_gettime(CLOCK_MONOTONIC, &last_swap_time);
        return;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &t2);
    
    // Calculate swap time (includes VSync wait)
    double swap_ms = (t2.tv_sec - t1.tv_sec) * 1000.0 + 
                     (t2.tv_nsec - t1.tv_nsec) / 1000000.0;
    
    // Warn on long swaps (indicates late arrival to VBlank)
    if (swap_ms > 20.0 && swap_count > 10) {
        printf("PERF: Long swap: %.1fms (late frame, missed VBlank window)\n", swap_ms);
    }
    
    // Present to display via DRM
    if (drm_swap_buffers(drm) != 0 && swap_count < 5) {
        printf("DRM swap failed\n");
    }
    
    last_swap_time = t2;
    swap_count++;
}

int gl_create_shaders(gl_context_t *gl) {
    return create_program(gl);
}

void gl_render_corners(gl_context_t *gl, keystone_context_t *keystone) {
    if (!keystone || !keystone->show_corners) {
        return; // Don't render corners if not visible
    }
    
    // Pre-allocated static buffer for corners (initialized once)
    static float corner_vertices[10000];  // Enough for position+color data
    static GLuint corner_vbo = 0;
    static bool vbo_initialized = false;
    static int cached_selected_corner = -2;  // Track which corner was selected
    
    // Initialize VBO once on first call
    if (!vbo_initialized) {
        glGenBuffers(1, &corner_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, corner_vbo);
        // OPTIMIZED: Pre-allocate with GL_DYNAMIC_DRAW on first init only
        glBufferData(GL_ARRAY_BUFFER, sizeof(corner_vertices), NULL, GL_DYNAMIC_DRAW);
        vbo_initialized = true;
    }
    
    // OPTIMIZATION: Only update VBO if corners moved or selection changed
    bool needs_update = keystone->corners_dirty || 
                        (cached_selected_corner != keystone->selected_corner);
    
    // Always prepare corner colors (outside needs_update so available for rendering)
    float corner_colors[4][4];
    for (int i = 0; i < 4; i++) {
        if (keystone->selected_corner == i) {
            // Green for selected
            corner_colors[i][0] = 0.0f;
            corner_colors[i][1] = 1.0f;
            corner_colors[i][2] = 0.0f;
            corner_colors[i][3] = 1.0f;
        } else {
            // White for unselected
            corner_colors[i][0] = 1.0f;
            corner_colors[i][1] = 1.0f;
            corner_colors[i][2] = 1.0f;
            corner_colors[i][3] = 1.0f;
        }
    }
    
    if (needs_update) {
        cached_selected_corner = keystone->selected_corner;
        keystone->corners_dirty = false;  // Clear dirty flag
        
        // Create corner positions (small squares)
        float corner_size = 0.015f; // 1.5% of screen size (smaller)
        
        // Corner vertices with colors: [x, y, r, g, b, a] per vertex
        int vertex_count = 0;
        
        // Render corner indicators at the keystone corner positions
        for (int i = 0; i < 4; i++) {
            // Get the keystone corner position
            float tx = keystone->corners[i].x;
            float ty = keystone->corners[i].y;
            float *color = corner_colors[i];
            
            // Create a small square with per-vertex colors (6 floats per vertex: x, y, r, g, b, a)
            if (vertex_count + 4 <= 1600) { // Leave room for text
                // Bottom-left
                corner_vertices[vertex_count*6 + 0] = tx - corner_size;
                corner_vertices[vertex_count*6 + 1] = ty - corner_size;
                corner_vertices[vertex_count*6 + 2] = color[0];
                corner_vertices[vertex_count*6 + 3] = color[1];
                corner_vertices[vertex_count*6 + 4] = color[2];
                corner_vertices[vertex_count*6 + 5] = color[3];
                vertex_count++;
                
                // Bottom-right
                corner_vertices[vertex_count*6 + 0] = tx + corner_size;
                corner_vertices[vertex_count*6 + 1] = ty - corner_size;
                corner_vertices[vertex_count*6 + 2] = color[0];
                corner_vertices[vertex_count*6 + 3] = color[1];
                corner_vertices[vertex_count*6 + 4] = color[2];
                corner_vertices[vertex_count*6 + 5] = color[3];
                vertex_count++;
                
                // Top-right
                corner_vertices[vertex_count*6 + 0] = tx + corner_size;
                corner_vertices[vertex_count*6 + 1] = ty + corner_size;
                corner_vertices[vertex_count*6 + 2] = color[0];
                corner_vertices[vertex_count*6 + 3] = color[1];
                corner_vertices[vertex_count*6 + 4] = color[2];
                corner_vertices[vertex_count*6 + 5] = color[3];
                vertex_count++;
                
                // Top-left
                corner_vertices[vertex_count*6 + 0] = tx - corner_size;
                corner_vertices[vertex_count*6 + 1] = ty + corner_size;
                corner_vertices[vertex_count*6 + 2] = color[0];
                corner_vertices[vertex_count*6 + 3] = color[1];
                corner_vertices[vertex_count*6 + 4] = color[2];
                corner_vertices[vertex_count*6 + 5] = color[3];
                vertex_count++;
            }
        }
        
        // OPTIMIZED: Use glBufferSubData instead of glBufferData
        // glBufferSubData only updates the data, much faster than reallocating
        glBindBuffer(GL_ARRAY_BUFFER, corner_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, vertex_count * 6 * sizeof(float), corner_vertices);
    }  // End needs_update block
    
    // Always bind and render (even if not updated)
    glBindBuffer(GL_ARRAY_BUFFER, corner_vbo);
    
    // Enable blending for transparent overlays
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Disable depth testing to ensure overlays are always visible
    glDisable(GL_DEPTH_TEST);
    
    // Use corner shader program
    glUseProgram(gl->corner_program);
    
    // Set up vertex attributes - interleaved position (2) + color (4)
    int stride = 6 * sizeof(float);
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(gl->corner_a_position);
    
    // Enable color attribute (location 1)
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Set identity matrix for MVP (corners in normalized coordinates)
    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->corner_u_mvp_matrix, 1, GL_FALSE, identity);
    
    // Render all 4 corner squares - colors are now in vertex data
    static int last_selected = -999;
    
    // Debug when selection changes
    if (keystone->selected_corner != last_selected) {
        printf("[RENDER] Selected corner changed: %d -> %d\n", last_selected, keystone->selected_corner);
        last_selected = keystone->selected_corner;
    }
    
    // Draw all corner squares in one call - each corner is 4 vertices
    for (int i = 0; i < 4; i++) {
        glDrawArrays(GL_TRIANGLE_FAN, i * 4, 4);
    }
    
    glDisableVertexAttribArray(gl->corner_a_position);
    glDisableVertexAttribArray(1); // Disable color attribute
    
    // Disable blending and restore depth testing
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    
    // CRITICAL: Restore GL state - unbind VBO to prevent interference
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void gl_render_border(gl_context_t *gl, keystone_context_t *keystone) {
    if (!keystone || !keystone->show_border) {
        return; // Don't render border if not visible
    }
    
    // Enable blending for transparent overlays
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Disable depth testing to ensure overlays are always visible
    glDisable(GL_DEPTH_TEST);
    
    // Create border line vertices with color (4 lines connecting the corners)
    // Each vertex: x, y, r, g, b, a (6 floats per vertex)
    float border_vertices[48]; // 8 vertices * 6 floats = 48 floats
    
    // Yellow color for all border vertices
    float r = 1.0f, g = 1.0f, b = 0.0f, a = 1.0f;
    
    // Show the control point positions (where user has positioned corners)
    point_t *corners = keystone->corners;
    
    // Line 1: Top-left to top-right (use direct Y coordinates)
    border_vertices[0] = corners[CORNER_TOP_LEFT].x;
    border_vertices[1] = corners[CORNER_TOP_LEFT].y;
    border_vertices[2] = r; border_vertices[3] = g; border_vertices[4] = b; border_vertices[5] = a;
    border_vertices[6] = corners[CORNER_TOP_RIGHT].x;
    border_vertices[7] = corners[CORNER_TOP_RIGHT].y;
    border_vertices[8] = r; border_vertices[9] = g; border_vertices[10] = b; border_vertices[11] = a;
    
    // Line 2: Top-right to bottom-right
    border_vertices[12] = corners[CORNER_TOP_RIGHT].x;
    border_vertices[13] = corners[CORNER_TOP_RIGHT].y;
    border_vertices[14] = r; border_vertices[15] = g; border_vertices[16] = b; border_vertices[17] = a;
    border_vertices[18] = corners[CORNER_BOTTOM_RIGHT].x;
    border_vertices[19] = corners[CORNER_BOTTOM_RIGHT].y;
    border_vertices[20] = r; border_vertices[21] = g; border_vertices[22] = b; border_vertices[23] = a;
    
    // Line 3: Bottom-right to bottom-left
    border_vertices[24] = corners[CORNER_BOTTOM_RIGHT].x;
    border_vertices[25] = corners[CORNER_BOTTOM_RIGHT].y;
    border_vertices[26] = r; border_vertices[27] = g; border_vertices[28] = b; border_vertices[29] = a;
    border_vertices[30] = corners[CORNER_BOTTOM_LEFT].x;
    border_vertices[31] = corners[CORNER_BOTTOM_LEFT].y;
    border_vertices[32] = r; border_vertices[33] = g; border_vertices[34] = b; border_vertices[35] = a;
    
    // Line 4: Bottom-left to top-left
    border_vertices[36] = corners[CORNER_BOTTOM_LEFT].x;
    border_vertices[37] = corners[CORNER_BOTTOM_LEFT].y;
    border_vertices[38] = r; border_vertices[39] = g; border_vertices[40] = b; border_vertices[41] = a;
    border_vertices[42] = corners[CORNER_TOP_LEFT].x;
    border_vertices[43] = corners[CORNER_TOP_LEFT].y;
    border_vertices[44] = r; border_vertices[45] = g; border_vertices[46] = b; border_vertices[47] = a;
    
    // Update border VBO with new positions
    glBindBuffer(GL_ARRAY_BUFFER, gl->border_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(border_vertices), border_vertices, GL_DYNAMIC_DRAW);
    
    // Use corner shader program (same as corners, just different geometry)
    glUseProgram(gl->corner_program);
    
    // Set up vertex attributes (interleaved position + color)
    int stride = 6 * sizeof(float); // x, y, r, g, b, a
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(gl->corner_a_position);
    
    // Color attribute at location 1
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Set identity matrix for MVP (border in normalized coordinates)
    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->corner_u_mvp_matrix, 1, GL_FALSE, identity);
    
    // Set line width for better visibility
    glLineWidth(3.0f);
    
    // Draw border as 4 separate lines (8 vertices with colors from vertex data)
    glDrawArrays(GL_LINES, 0, 8); // 8 vertices (4 lines * 2 vertices each)
    
    // Reset line width
    glLineWidth(1.0f);
    
    glDisableVertexAttribArray(gl->corner_a_position);
    glDisableVertexAttribArray(1);
    
    // Disable blending and restore depth testing
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    
    // NOTE: Don't unbind buffers here - video_player.c handles complete state restoration
}

// Complete 5x7 bitmap font data - each byte represents one row, 5 bits used
static const unsigned char font_5x7[128][7] = {
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['!'] = {0x20, 0x20, 0x20, 0x20, 0x00, 0x20, 0x00},
    ['/'] = {0x08, 0x08, 0x10, 0x20, 0x40, 0x40, 0x00},
    [':'] = {0x00, 0x20, 0x00, 0x00, 0x20, 0x00, 0x00},
    ['-'] = {0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00},
    ['('] = {0x10, 0x20, 0x20, 0x20, 0x20, 0x10, 0x00},
    [')'] = {0x20, 0x10, 0x10, 0x10, 0x10, 0x20, 0x00},
    ['0'] = {0x70, 0x88, 0x98, 0xA8, 0xC8, 0x70, 0x00},
    ['1'] = {0x20, 0x60, 0x20, 0x20, 0x20, 0x70, 0x00},
    ['2'] = {0x70, 0x88, 0x08, 0x30, 0x40, 0xF8, 0x00},
    ['3'] = {0x70, 0x88, 0x30, 0x08, 0x88, 0x70, 0x00},
    ['4'] = {0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x00},
    ['5'] = {0xF8, 0x80, 0xF0, 0x08, 0x88, 0x70, 0x00},
    ['6'] = {0x30, 0x40, 0x80, 0xF0, 0x88, 0x70, 0x00},
    ['7'] = {0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x00},
    ['8'] = {0x70, 0x88, 0x70, 0x88, 0x88, 0x70, 0x00},
    ['9'] = {0x70, 0x88, 0x78, 0x08, 0x10, 0x60, 0x00},
    ['A'] = {0x20, 0x50, 0x88, 0x88, 0xF8, 0x88, 0x00},
    ['B'] = {0xF0, 0x88, 0xF0, 0x88, 0x88, 0xF0, 0x00},
    ['C'] = {0x70, 0x88, 0x80, 0x80, 0x88, 0x70, 0x00},
    ['D'] = {0xF0, 0x88, 0x88, 0x88, 0x88, 0xF0, 0x00},
    ['E'] = {0xF8, 0x80, 0xF0, 0x80, 0x80, 0xF8, 0x00},
    ['F'] = {0xF8, 0x80, 0xF0, 0x80, 0x80, 0x80, 0x00},
    ['G'] = {0x70, 0x88, 0x80, 0xB8, 0x88, 0x78, 0x00},
    ['H'] = {0x88, 0x88, 0xF8, 0x88, 0x88, 0x88, 0x00},
    ['I'] = {0x70, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00},
    ['J'] = {0x38, 0x10, 0x10, 0x10, 0x90, 0x60, 0x00},
    ['K'] = {0x88, 0x90, 0xA0, 0xC0, 0xA0, 0x90, 0x00},
    ['L'] = {0x80, 0x80, 0x80, 0x80, 0x80, 0xF8, 0x00},
    ['M'] = {0x88, 0xD8, 0xA8, 0xA8, 0x88, 0x88, 0x00},
    ['N'] = {0x88, 0xC8, 0xA8, 0x98, 0x88, 0x88, 0x00},
    ['O'] = {0x70, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00},
    ['P'] = {0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80, 0x00},
    ['Q'] = {0x70, 0x88, 0x88, 0xA8, 0x90, 0x68, 0x00},
    ['R'] = {0xF0, 0x88, 0x88, 0xF0, 0xA0, 0x90, 0x00},
    ['S'] = {0x70, 0x88, 0x60, 0x10, 0x88, 0x70, 0x00},
    ['T'] = {0xF8, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00},
    ['U'] = {0x88, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00},
    ['V'] = {0x88, 0x88, 0x88, 0x50, 0x50, 0x20, 0x00},
    ['W'] = {0x88, 0x88, 0xA8, 0xA8, 0xD8, 0x88, 0x00},
    ['X'] = {0x88, 0x50, 0x20, 0x20, 0x50, 0x88, 0x00},
    ['Y'] = {0x88, 0x88, 0x50, 0x20, 0x20, 0x20, 0x00},
    ['Z'] = {0xF8, 0x08, 0x10, 0x20, 0x40, 0xF8, 0x00},
    ['a'] = {0x00, 0x00, 0x70, 0x08, 0x78, 0x88, 0x78},
    ['b'] = {0x80, 0x80, 0xF0, 0x88, 0x88, 0x88, 0xF0},
    ['c'] = {0x00, 0x00, 0x70, 0x88, 0x80, 0x88, 0x70},
    ['d'] = {0x08, 0x08, 0x78, 0x88, 0x88, 0x88, 0x78},
    ['e'] = {0x00, 0x00, 0x70, 0x88, 0xF8, 0x80, 0x70},
    ['f'] = {0x30, 0x48, 0x40, 0xF0, 0x40, 0x40, 0x40},
    ['g'] = {0x00, 0x78, 0x88, 0x88, 0x78, 0x08, 0x70},
    ['h'] = {0x80, 0x80, 0xF0, 0x88, 0x88, 0x88, 0x88},
    ['i'] = {0x20, 0x00, 0x60, 0x20, 0x20, 0x20, 0x70},
    ['j'] = {0x10, 0x00, 0x30, 0x10, 0x10, 0x90, 0x60},
    ['k'] = {0x80, 0x80, 0x90, 0xA0, 0xC0, 0xA0, 0x90},
    ['l'] = {0x60, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70},
    ['m'] = {0x00, 0x00, 0xD0, 0xA8, 0xA8, 0xA8, 0xA8},
    ['n'] = {0x00, 0x00, 0xF0, 0x88, 0x88, 0x88, 0x88},
    ['o'] = {0x00, 0x00, 0x70, 0x88, 0x88, 0x88, 0x70},
    ['p'] = {0x00, 0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80},
    ['q'] = {0x00, 0x78, 0x88, 0x88, 0x78, 0x08, 0x08},
    ['r'] = {0x00, 0x00, 0xB0, 0xC8, 0x80, 0x80, 0x80},
    ['s'] = {0x00, 0x00, 0x78, 0x80, 0x70, 0x08, 0xF0},
    ['t'] = {0x40, 0x40, 0xF0, 0x40, 0x40, 0x48, 0x30},
    ['u'] = {0x00, 0x00, 0x88, 0x88, 0x88, 0x88, 0x78},
    ['v'] = {0x00, 0x00, 0x88, 0x88, 0x88, 0x50, 0x20},
    ['w'] = {0x00, 0x00, 0x88, 0xA8, 0xA8, 0xA8, 0x50},
    ['x'] = {0x00, 0x00, 0x88, 0x50, 0x20, 0x50, 0x88},
    ['y'] = {0x00, 0x88, 0x88, 0x88, 0x78, 0x08, 0x70},
    ['z'] = {0x00, 0x00, 0xF8, 0x10, 0x20, 0x40, 0xF8},
};

static void draw_char_simple(float *vertices, int *vertex_count, char c, float x, float y, float size) {
    if (*vertex_count >= 15000) return; // Safety limit - increased for longer help text
    
    // Check bounds and handle unknown characters
    unsigned char uc = (unsigned char)c;
    if (uc > 127) return;
    
    const unsigned char *char_data = font_5x7[uc];
    
    // Check if character has any data (handle sparse array)
    bool has_data = false;
    for (int i = 0; i < 7; i++) {
        if (char_data[i] != 0) {
            has_data = true;
            break;
        }
    }
    if (!has_data && c != ' ') return; // Skip undefined chars except space
    
    float pixel_size = size / 7.0f;  // 5x7 font, divide by 7 for height
    
    for (int row = 0; row < 7; row++) {
        unsigned char row_data = char_data[row];
        for (int col = 0; col < 8; col++) {  // Check all 8 bits
            if ((row_data >> (7-col)) & 1) { // Check if pixel is set (MSB first)
                if (*vertex_count + 4 <= 15000) {
                    float px = x + col * pixel_size;
                    float py = y - (row * pixel_size); // Top to bottom
                    
                    // Add rectangle for this pixel
                    vertices[(*vertex_count)*2] = px; 
                    vertices[(*vertex_count)*2+1] = py;
                    (*vertex_count)++;
                    vertices[(*vertex_count)*2] = px + pixel_size; 
                    vertices[(*vertex_count)*2+1] = py;
                    (*vertex_count)++;
                    vertices[(*vertex_count)*2] = px + pixel_size; 
                    vertices[(*vertex_count)*2+1] = py - pixel_size;
                    (*vertex_count)++;
                    vertices[(*vertex_count)*2] = px; 
                    vertices[(*vertex_count)*2+1] = py - pixel_size;
                    (*vertex_count)++;
                }
            }
        }
    }
}

static void draw_text_simple(float *vertices, int *vertex_count, const char *text, float x, float y, float size) {
    float char_width = size * 1.2f;    // Character width including spacing
    float line_height = size * 1.3f;   // Tighter line spacing to fit more text
    float current_x = x;
    float current_y = y;
    
    while (*text && *vertex_count < 15000) {
        if (*text == '\n') {
            current_x = x;
            current_y -= line_height;
        } else {
            draw_char_simple(vertices, vertex_count, *text, current_x, current_y, size);
            current_x += char_width;
        }
        text++;
    }
}

void gl_render_help_overlay(gl_context_t *gl, keystone_context_t *keystone) {
    if (!keystone || !keystone->show_help) {
        return; // Don't render help overlay if not visible
    }
    
    // Create help overlay with text - using vertex colors (x, y, r, g, b, a)
    float help_vertices[60000]; // Large buffer for text vertices with color
    int vertex_count = 0;
    
    // Large centered background with semi-transparent black color
    // Bottom-left
    help_vertices[0] = -0.9f; help_vertices[1] = -0.7f;
    help_vertices[2] = 0.0f; help_vertices[3] = 0.0f; help_vertices[4] = 0.0f; help_vertices[5] = 0.8f;
    // Bottom-right
    help_vertices[6] = 0.9f; help_vertices[7] = -0.7f;
    help_vertices[8] = 0.0f; help_vertices[9] = 0.0f; help_vertices[10] = 0.0f; help_vertices[11] = 0.8f;
    // Top-right
    help_vertices[12] = 0.9f; help_vertices[13] = 0.7f;
    help_vertices[14] = 0.0f; help_vertices[15] = 0.0f; help_vertices[16] = 0.0f; help_vertices[17] = 0.8f;
    // Top-left
    help_vertices[18] = -0.9f; help_vertices[19] = 0.7f;
    help_vertices[20] = 0.0f; help_vertices[21] = 0.0f; help_vertices[22] = 0.0f; help_vertices[23] = 0.8f;
    vertex_count = 4;
    
    // Compact help text - keyboard and gamepad controls
    const char* help_text = 
        "PICKLE KEYSTONE\n"
        "\n"
        "KEYBOARD\n"
        "1234   Corner Select\n"
        "Arrows Move Corner\n"
        "S      Save\n"
        "R      Reset\n"
        "C      Toggle Corners\n"
        "B      Toggle Border\n"
        "W      WiFi Connect\n"
        "H      Toggle Help\n"
        "Q      Quit\n"
        "\n"
        "GAMEPAD\n"
        "X      Cycle Corner\n"
        "DPAD   Move Corner\n"
        "A      Show Corners\n"
        "B      Show Border\n"
        "Y      Show Help\n"
        "L1     Step Down\n"
        "R1     Step Up\n"
        "START  Save\n"
        "SELECT Reset";
    
    // Update help VBO with background geometry
    glBindBuffer(GL_ARRAY_BUFFER, gl->help_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertex_count * 6 * sizeof(float), help_vertices, GL_DYNAMIC_DRAW);
    
    // Use corner shader program for help overlay rendering
    glUseProgram(gl->corner_program);
    
    // Set up vertex attributes (interleaved position + color)
    int stride = 6 * sizeof(float); // x, y, r, g, b, a
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(gl->corner_a_position);
    
    // Color attribute at location 1
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Set identity matrix for MVP
    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->corner_u_mvp_matrix, 1, GL_FALSE, identity);
    
    // Enable blending for transparency
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Disable depth testing to ensure overlays are always visible
    glDisable(GL_DEPTH_TEST);
    
    // Draw help overlay background (semi-transparent black from vertex colors)
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4); // Background rectangle
    
    // Now render text with better formatting
    float text_vertices[30000]; // Increased buffer size for longer help text
    int text_vertex_count = 0;
    draw_text_simple(text_vertices, &text_vertex_count, help_text, -0.85f, 0.62f, 0.022f);
    
    // Upload text geometry with 2-float format
    glBufferData(GL_ARRAY_BUFFER, text_vertex_count * 2 * sizeof(float), text_vertices, GL_DYNAMIC_DRAW);
    
    // Update vertex attribute for 2-float format
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDisableVertexAttribArray(1); // Disable color attribute
    
    // Create cyan/white color vertices for text (manually set color for each vertex)
    float colored_vertices[180000]; // Large buffer with colors (30000 * 6)
    for (int i = 0; i < text_vertex_count && i * 6 < 180000; i++) {
        colored_vertices[i * 6 + 0] = text_vertices[i * 2 + 0]; // x
        colored_vertices[i * 6 + 1] = text_vertices[i * 2 + 1]; // y
        colored_vertices[i * 6 + 2] = 0.0f; // r - cyan color
        colored_vertices[i * 6 + 3] = 1.0f; // g
        colored_vertices[i * 6 + 4] = 1.0f; // b
        colored_vertices[i * 6 + 5] = 1.0f; // a
    }
    
    // Upload text with colors
    glBufferData(GL_ARRAY_BUFFER, text_vertex_count * 6 * sizeof(float), colored_vertices, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Draw all text pixels (each character pixel is a 4-vertex rectangle)
    int text_pixels = text_vertex_count / 4; // Number of character pixels
    for (int i = 0; i < text_pixels; i++) {
        glDrawArrays(GL_TRIANGLE_FAN, i * 4, 4);
    }
    
    // Disable blending and restore depth testing
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    
    glDisableVertexAttribArray(gl->corner_a_position);
    glDisableVertexAttribArray(1);
    
    // CRITICAL: Restore GL state - unbind VBO to prevent interference
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void gl_render_wifi_overlay(gl_context_t *gl, wifi_manager_t *mgr) {
    if (!gl || !mgr) return;
    
    // DEBUG: Log what mgr pointer value we received
    #ifndef NDEBUG
    static int render_call_count = 0;
    if (render_call_count++ % 60 == 0) {
        printf("[WiFi Render CALLED] mgr pointer=%p, selected_index=%d\n", (void*)mgr, mgr->selected_index);
        fflush(stdout);
    }
    #endif
    
    // Print WiFi menu to console every 60 frames (about once per second at 60fps)
    static int print_counter = 0;
    if (print_counter++ % 60 == 0) {
        printf("\n=== WiFi Networks ===\n");
        for (int i = 0; i < mgr->network_count && i < 5; i++) {
            printf("  [%d] %s - Signal: %d%% %s\n", 
                   i, mgr->networks[i].ssid, (int)mgr->networks[i].signal_strength,
                   i == mgr->selected_index ? "← SELECTED" : "");
        }
        printf("Use D-pad UP/DOWN to select, SELECT button to confirm\n");
        printf("======================\n");
        fflush(stdout);
    }
    
    // Enable blending for semi-transparent overlay
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    
    glUseProgram(gl->corner_program);
    
    // Set MVP matrix
    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->corner_u_mvp_matrix, 1, GL_FALSE, identity);
    
    // Render full-screen semi-transparent black background
    float bg_vertices[24] = {
        -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.9f,
         1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.9f,
         1.0f,  1.0f, 0.0f, 0.0f, 0.0f, 0.9f,
        -1.0f,  1.0f, 0.0f, 0.0f, 0.0f, 0.9f
    };
    
    glBindBuffer(GL_ARRAY_BUFFER, gl->corner_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(bg_vertices), bg_vertices, GL_DYNAMIC_DRAW);
    
    int stride = 6 * sizeof(float);
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(gl->corner_a_position);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    
    // Draw a highlight bar behind the currently selected network line (only in list mode)
    // This makes selection visible even if the '>' glyph isn't present in the font
    if (mgr->state == WIFI_STATE_NETWORK_LIST) {
        // Text layout parameters must match draw_text_simple()
        const float base_y = 0.5f;
        const float text_size = 0.025f;
        const float line_height = text_size * 1.3f;
        // Header (WiFi Networks) + blank line before networks start
        int selected_line_index = 2 + mgr->selected_index; // 0-based from top
        float line_y = base_y - selected_line_index * line_height;
        
        // Highlight rectangle extents
        float rect_left = -0.9f;
        float rect_right = 0.5f;
        float rect_top = line_y + (line_height * 0.55f);
        float rect_bottom = line_y - (line_height * 0.55f);
        
        // Semi-transparent green highlight
        float hl_vertices[24] = {
            rect_left,  rect_bottom, 0.0f, 0.6f, 0.2f, 0.45f,
            rect_right, rect_bottom, 0.0f, 0.6f, 0.2f, 0.45f,
            rect_right, rect_top,    0.0f, 0.6f, 0.2f, 0.45f,
            rect_left,  rect_top,    0.0f, 0.6f, 0.2f, 0.45f
        };
        
        glBindBuffer(GL_ARRAY_BUFFER, gl->corner_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(hl_vertices), hl_vertices, GL_DYNAMIC_DRAW);
        int hl_stride = 6 * sizeof(float);
        glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, hl_stride, (void*)0);
        glEnableVertexAttribArray(gl->corner_a_position);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, hl_stride, (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    }
    
    // Build WiFi overlay text depending on state
    char wifi_text[1024];
    int offset = 0;
    if (mgr->state == WIFI_STATE_NETWORK_LIST) {
        offset = snprintf(wifi_text, sizeof(wifi_text), "WiFi Networks\n\n");
        for (int i = 0; i < mgr->network_count && i < 5; i++) {
            if (i == mgr->selected_index) {
                offset += snprintf(wifi_text + offset, sizeof(wifi_text) - offset,
                                 "> %s (%d%%)\n", mgr->networks[i].ssid, 
                                 (int)mgr->networks[i].signal_strength);
            } else {
                offset += snprintf(wifi_text + offset, sizeof(wifi_text) - offset,
                                 "  %s (%d%%)\n", mgr->networks[i].ssid,
                                 (int)mgr->networks[i].signal_strength);
            }
        }
        snprintf(wifi_text + offset, sizeof(wifi_text) - offset,
                 "\nArrows: Select\nEnter/SELECT: Choose");
    } else if (mgr->state == WIFI_STATE_PASSWORD_ENTRY) {
        // Password display (masked or visible)
        char shown[256];
        if (mgr->show_password) {
            snprintf(shown, sizeof(shown), "%s", mgr->password);
        } else {
            int len = (mgr->password_length >= 0) ? mgr->password_length : 0;
            if (len > (int)sizeof(shown) - 1) len = (int)sizeof(shown) - 1;
            memset(shown, '*', len);
            shown[len] = '\0';
        }

        offset = snprintf(wifi_text, sizeof(wifi_text),
                          "WiFi Password\n\nSSID: %s\nPassword: %s (%s)\n\n",
                          mgr->networks[mgr->selected_index].ssid,
                          shown,
                          mgr->show_password ? "visible" : "hidden");
        offset += snprintf(wifi_text + offset, sizeof(wifi_text) - offset,
                           "Arrows: Move  Enter/SELECT: Press Key  Backspace: Delete\n");
        if (mgr->status[0] != '\0') {
            // Append status/error message
            offset += snprintf(wifi_text + offset, sizeof(wifi_text) - offset,
                               "\n%s\n",
                               mgr->status);
        }
        
        // After the text, draw a simple on-screen keyboard grid
        // Layout is in mgr->keyboard_layout[4] with varying row lengths
    const float key_text_size = 0.028f;
    const float start_y = 0.12f; // slightly lower to reduce overlap
    const float row_gap = key_text_size * 2.6f;
    const float key_w = 0.11f;   // slightly smaller keys
    const float key_h = row_gap * 0.45f; // a bit shorter for clearer rows
    const float key_gap = 0.02f; // spacing between keys
        
    // Draw each key; render selection background before its label to avoid tinting

        for (int r = 0; r < 4; ++r) {
            int row_len = (int)strlen(mgr->keyboard_layout[r]);
            // Center the row based on total width including gaps
            float row_total_w = row_len * key_w + (row_len - 1) * key_gap;
            float row_offset_x = -0.5f * row_total_w;
            float y = start_y - r * row_gap;
            for (int c = 0; c < row_len; ++c) {
                float x = row_offset_x + c * (key_w + key_gap);
                // Highlight if selected
                int cursor_row = mgr->keyboard_cursor / 12;
                int cursor_col = mgr->keyboard_cursor % 12;
                bool is_sel = (cursor_row == r && cursor_col == c);
                
                // Base key background (dark grey)
                float base_r = 0.05f, base_g = 0.05f, base_b = 0.05f, base_a = 0.55f;
                
                // Draw key rectangle
                float rect[24] = {
                    x,           y - key_h, 0.0f, base_r, base_g, base_b, base_a,
                    x + key_w,   y - key_h, 0.0f, base_r, base_g, base_b, base_a,
                    x + key_w,   y + key_h, 0.0f, base_r, base_g, base_b, base_a,
                    x,           y + key_h, 0.0f, base_r, base_g, base_b, base_a
                };
                glBindBuffer(GL_ARRAY_BUFFER, gl->corner_vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(rect), rect, GL_DYNAMIC_DRAW);
                glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
                glEnableVertexAttribArray(gl->corner_a_position);
                glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
                glEnableVertexAttribArray(1);
                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
                if (is_sel) {
                    // To avoid any translucent artifacts, draw selection with blending temporarily disabled
                    glDisable(GL_BLEND);
                    float sel_rcol = 0.20f, sel_gcol = 0.85f, sel_bcol = 0.30f, sel_acol = 1.0f;
                    float srect[24] = {
                        x,         y - key_h, 0.0f, sel_rcol, sel_gcol, sel_bcol, sel_acol,
                        x + key_w, y - key_h, 0.0f, sel_rcol, sel_gcol, sel_bcol, sel_acol,
                        x + key_w, y + key_h, 0.0f, sel_rcol, sel_gcol, sel_bcol, sel_acol,
                        x,         y + key_h, 0.0f, sel_rcol, sel_gcol, sel_bcol, sel_acol
                    };
                    glBindBuffer(GL_ARRAY_BUFFER, gl->corner_vbo);
                    glBufferData(GL_ARRAY_BUFFER, sizeof(srect), srect, GL_DYNAMIC_DRAW);
                    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
                    glEnableVertexAttribArray(gl->corner_a_position);
                    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
                    glEnableVertexAttribArray(1);
                    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

                    // Border
                    float bt = (key_w < key_h ? key_w : key_h) * 0.10f;
                    float br = 1.0f, bg = 1.0f, bb = 0.2f, ba = 1.0f;
                    float b_top[24] = {
                        x - bt,      y + key_h + bt, 0.0f, br, bg, bb, ba,
                        x + key_w + bt, y + key_h + bt, 0.0f, br, bg, bb, ba,
                        x + key_w + bt, y + key_h,    0.0f, br, bg, bb, ba,
                        x - bt,      y + key_h,      0.0f, br, bg, bb, ba
                    };
                    glBufferData(GL_ARRAY_BUFFER, sizeof(b_top), b_top, GL_DYNAMIC_DRAW);
                    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
                    float b_bot[24] = {
                        x - bt,      y - key_h,      0.0f, br, bg, bb, ba,
                        x + key_w + bt, y - key_h,   0.0f, br, bg, bb, ba,
                        x + key_w + bt, y - key_h - bt, 0.0f, br, bg, bb, ba,
                        x - bt,      y - key_h - bt, 0.0f, br, bg, bb, ba
                    };
                    glBufferData(GL_ARRAY_BUFFER, sizeof(b_bot), b_bot, GL_DYNAMIC_DRAW);
                    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
                    float b_left[24] = {
                        x - bt, y - key_h, 0.0f, br, bg, bb, ba,
                        x,      y - key_h, 0.0f, br, bg, bb, ba,
                        x,      y + key_h, 0.0f, br, bg, bb, ba,
                        x - bt, y + key_h, 0.0f, br, bg, bb, ba
                    };
                    glBufferData(GL_ARRAY_BUFFER, sizeof(b_left), b_left, GL_DYNAMIC_DRAW);
                    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
                    float b_right[24] = {
                        x + key_w, y - key_h, 0.0f, br, bg, bb, ba,
                        x + key_w + bt, y - key_h, 0.0f, br, bg, bb, ba,
                        x + key_w + bt, y + key_h, 0.0f, br, bg, bb, ba,
                        x + key_w, y + key_h, 0.0f, br, bg, bb, ba
                    };
                    glBufferData(GL_ARRAY_BUFFER, sizeof(b_right), b_right, GL_DYNAMIC_DRAW);
                    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
                    glEnable(GL_BLEND);
                }

                // Draw label
                char label[8] = {0};
                char k = mgr->keyboard_layout[r][c];
                if (k == '_') { label[0] = 'S'; label[1] = 'P'; }
                else if (k == '<') { label[0] = 'B'; label[1] = 'K'; label[2] = 'S'; label[3] = 'P'; }
                else if (k == '>') { label[0] = 'O'; label[1] = 'K'; }
                else if (k == '!') {
                    if (mgr->show_password) { label[0] = 'H'; label[1] = 'I'; label[2] = 'D'; label[3] = 'E'; }
                    else { label[0] = 'S'; label[1] = 'H'; label[2] = 'O'; label[3] = 'W'; }
                }
                else label[0] = k;
                int label_len = (int)strlen(label);
                float max_fit_size = (key_w * 0.80f) / (1.2f * (label_len > 0 ? label_len : 1));
                float label_size = max_fit_size;
                if (label_size > 0.040f) label_size = 0.040f;
                if (label_size < 0.022f) label_size = 0.022f;
                float total_w = (label_len ? label_len : 1) * label_size * 1.2f;
                float tx = x + (key_w - total_w) * 0.5f;
                float ty = y + label_size * 0.2f; // better vertical centering for our font

                float text_buf[512];
                int vcount = 0;
                // Slightly increase label size when selected
                float eff_label_size = is_sel ? (label_size * 1.1f) : label_size;
                draw_text_simple(text_buf, &vcount, label, tx, ty, eff_label_size);
                if (vcount > 0) {
                    float colored[4096];
                    int cc = 0;
                    for (int i = 0; i < vcount && (cc + 6) < 4096; ++i) {
                        colored[cc++] = text_buf[i * 2 + 0];
                        colored[cc++] = text_buf[i * 2 + 1];
                        // Invert label color when selected (black on bright key)
                        float tr = is_sel ? 0.0f : 1.0f;
                        float tg = is_sel ? 0.0f : 1.0f;
                        float tb = is_sel ? 0.0f : 1.0f;
                        colored[cc++] = tr;
                        colored[cc++] = tg;
                        colored[cc++] = tb;
                        colored[cc++] = 1.0f;
                    }
                    glBindBuffer(GL_ARRAY_BUFFER, gl->corner_vbo);
                    glBufferData(GL_ARRAY_BUFFER, cc * sizeof(float), colored, GL_DYNAMIC_DRAW);
                    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
                    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
                    glEnableVertexAttribArray(gl->corner_a_position);
                    glEnableVertexAttribArray(1);
                    int pixels = vcount / 4;
                    for (int p = 0; p < pixels; ++p) glDrawArrays(GL_TRIANGLE_FAN, p * 4, 4);
                }
            }
        }

        // (Selection already drawn per-key before labels)
    } else if (mgr->state == WIFI_STATE_SCANNING) {
        snprintf(wifi_text, sizeof(wifi_text),
                 "WiFi\n\nScanning for networks...\n\nPlease wait");
    } else {
        // Fallback: show nothing if in other states
        snprintf(wifi_text, sizeof(wifi_text), "WiFi\n");
    }
    
    // DEBUG: Log the text being rendered
    #ifndef NDEBUG
    static int wifi_text_render_count = 0;
    if (wifi_text_render_count++ % 120 == 0) {
        printf("[WiFi Text Rendered] selected_index=%d, First line: %.50s\n", 
               mgr->selected_index, wifi_text);
    }
    #endif
    
    // Render text using same approach as help overlay
    float text_vertices[30000];
    int text_vertex_count = 0;
    draw_text_simple(text_vertices, &text_vertex_count, wifi_text, -0.8f, 0.5f, 0.025f);
    
    #ifndef NDEBUG
    static int detailed_log_count = 0;
    bool should_log_detailed = (detailed_log_count++ % 120 == 0);
    #else
    bool should_log_detailed = false;
    #endif
    
    if (should_log_detailed) {
        printf("[WiFi Render] idx=%d, count=%d\n", mgr->selected_index, text_vertex_count);
        // Print line by line to see the markers clearly
        printf("=== Full WiFi Text ===\n%s\n", wifi_text);
        printf("====== End Text ======\n");
        
    // Also show text length
    printf("[DEBUG] wifi_text strlen=%zu\n", strlen(wifi_text));
    fflush(stdout);
    }
    
    if (text_vertex_count > 0) {
        // Create colored vertices for text - YELLOW to be clearly visible
        float colored_vertices[180000];
        int colored_vertex_count = 0;
        for (int i = 0; i < text_vertex_count && i * 6 < 180000; i++) {
            colored_vertices[colored_vertex_count++] = text_vertices[i * 2 + 0]; // x
            colored_vertices[colored_vertex_count++] = text_vertices[i * 2 + 1]; // y
            colored_vertices[colored_vertex_count++] = 1.0f; // r - YELLOW
            colored_vertices[colored_vertex_count++] = 1.0f; // g - YELLOW
            colored_vertices[colored_vertex_count++] = 0.0f; // b - no blue
            colored_vertices[colored_vertex_count++] = 1.0f; // a
        }
        
        // Upload text with colors
        glBindBuffer(GL_ARRAY_BUFFER, gl->corner_vbo);
        glBufferData(GL_ARRAY_BUFFER, colored_vertex_count * sizeof(float), colored_vertices, GL_DYNAMIC_DRAW);
        glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(gl->corner_a_position);
        glEnableVertexAttribArray(1);
        
        int text_pixels = text_vertex_count / 4;
        
        if (should_log_detailed) {
            printf("  Rendering %d text pixels\n", text_pixels);
            printf("  text_vertex_count=%d, colored_vertex_count=%d\n", text_vertex_count, colored_vertex_count);
        }
        
        // Draw all text pixels (each character pixel is a 4-vertex rectangle)
        int draw_calls = 0;
        for (int i = 0; i < text_pixels; i++) {
            glDrawArrays(GL_TRIANGLE_FAN, i * 4, 4);
            draw_calls++;
        }
        
        if (should_log_detailed) {
            printf("  Completed %d draw calls\n", draw_calls);
            fflush(stdout);
        }
    }
    
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDisableVertexAttribArray(gl->corner_a_position);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void gl_cleanup(gl_context_t *gl) {
    // Clean up YUV textures
    if (gl->texture_y) glDeleteTextures(1, &gl->texture_y);
    if (gl->texture_u) glDeleteTextures(1, &gl->texture_u);
    if (gl->texture_v) glDeleteTextures(1, &gl->texture_v);
    if (gl->texture_nv12) glDeleteTextures(1, &gl->texture_nv12);
    
    // Clean up PBOs
    if (gl->use_pbo) {
        glDeleteBuffers(3, gl->pbo);
    }
    
    if (gl->vbo) glDeleteBuffers(1, &gl->vbo);
    if (gl->ebo) glDeleteBuffers(1, &gl->ebo);
    if (gl->corner_vbo) glDeleteBuffers(1, &gl->corner_vbo);
    if (gl->border_vbo) glDeleteBuffers(1, &gl->border_vbo);
    if (gl->help_vbo) glDeleteBuffers(1, &gl->help_vbo);
    if (gl->program) glDeleteProgram(gl->program);
    if (gl->corner_program) glDeleteProgram(gl->corner_program);
    if (gl->vertex_shader) glDeleteShader(gl->vertex_shader);
    if (gl->fragment_shader) glDeleteShader(gl->fragment_shader);
    
    // Clean up pre-allocated YUV buffers
    free_yuv_buffers();
    
    if (gl->egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(gl->egl_display, gl->egl_surface);
    }
    if (gl->egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(gl->egl_display, gl->egl_context);
    }
    if (gl->egl_display != EGL_NO_DISPLAY) {
        eglTerminate(gl->egl_display);
    }
}