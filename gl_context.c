#include "gl_context.h"
#include "drm_display.h"
#include "keystone.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// OpenGL ES 3.0 constants for single-channel textures
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RED  
#define GL_RED 0x1903
#endif

// Vertex shader for keystone correction and video rendering (OpenGL ES 3.1)
const char *vertex_shader_source = 
    "#version 310 es\n"
    "layout(location = 0) in vec2 a_position;\n"
    "layout(location = 1) in vec2 a_texcoord;\n"
    "uniform mat4 u_mvp_matrix;\n"
    "uniform mat4 u_keystone_matrix;\n"
    "out vec2 v_texcoord;\n"
    "void main() {\n"
    "    vec4 pos = vec4(a_position, 0.0, 1.0);\n"
    "    pos = u_keystone_matrix * pos;\n"
    "    gl_Position = u_mvp_matrix * pos;\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";

// Fragment shader for YUV→RGB conversion (OpenGL ES 3.1)
const char *fragment_shader_source = 
    "#version 310 es\n"
    "precision mediump float;\n"
    "in vec2 v_texcoord;\n"
    "uniform sampler2D u_texture_y;  // Y plane\n"
    "uniform sampler2D u_texture_u;  // U plane\n" 
    "uniform sampler2D u_texture_v;  // V plane\n"
    "out vec4 fragColor;\n"
    "\n"
    "void main() {\n"
    "    // Sample YUV values\n"
    "    float y = texture(u_texture_y, v_texcoord).r;\n"
    "    float u = texture(u_texture_u, v_texcoord).r - 0.5;\n"
    "    float v = texture(u_texture_v, v_texcoord).r - 0.5;\n"
    "    \n"
    "    // YUV to RGB conversion (BT.709 color space)\n"
    "    float r = y + 1.5748 * v;\n"
    "    float g = y - 0.1873 * u - 0.4681 * v;\n"
    "    float b = y + 1.8556 * u;\n"
    "    \n"
    "    fragColor = vec4(r, g, b, 1.0);\n"
    "}\n";

// Corner highlight shaders (OpenGL ES 3.1)
const char *corner_vertex_shader_source = 
    "#version 310 es\n"
    "layout(location = 0) in vec2 a_position;\n"
    "uniform mat4 u_mvp_matrix;\n"
    "void main() {\n"
    "    gl_Position = u_mvp_matrix * vec4(a_position, 0.0, 1.0);\n"
    "}\n";

const char *corner_fragment_shader_source = 
    "#version 310 es\n"
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = u_color;\n"
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

    // OpenGL ES initialization complete
    return 0;
}

void gl_setup_buffers(gl_context_t *gl) {
    // Quad vertices (position + texture coordinates)
    float vertices[] = {
        // Position  // TexCoord
        -1.0f, -1.0f, 0.0f, 1.0f,  // Bottom-left
         1.0f, -1.0f, 1.0f, 1.0f,  // Bottom-right
         1.0f,  1.0f, 1.0f, 0.0f,  // Top-right
        -1.0f,  1.0f, 0.0f, 0.0f   // Top-left
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
    
    // Setup corner highlight VBO - will be updated with actual corner positions
    glGenBuffers(1, &gl->corner_vbo);
    
    // Setup border VBO - will be updated with actual border positions
    glGenBuffers(1, &gl->border_vbo);
    
    // Setup help overlay VBO - will be updated with help overlay geometry
    glGenBuffers(1, &gl->help_vbo);
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
    
    // Set up rendering state every frame (no VAO)
    glUseProgram(gl->program);
    
    // Bind buffers and set up vertex attributes
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);
    
    // Position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    // Texture coordinate attribute  
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Texture binding is now handled in the YUV section below
    
    // Set MVP matrix (identity for now)
    float mvp_matrix[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->u_mvp_matrix, 1, GL_FALSE, mvp_matrix);
    
    // Set keystone matrix from keystone correction system
    const float *keystone_matrix = keystone_get_matrix(keystone);
    glUniformMatrix4fv(gl->u_keystone_matrix, 1, GL_FALSE, keystone_matrix);
    
    // Update YUV textures if video data is provided
    if (y_data && u_data && v_data) {
        // Calculate UV dimensions (usually half resolution for 4:2:0)
        int uv_width = width / 2;
        int uv_height = height / 2;
        
        // Update Y texture (handle stride manually for OpenGL ES)
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gl->texture_y);
        
        if (width != last_width || height != last_height || frame_rendered == 0) {
            if (y_stride == width) {
                // No padding, upload directly
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, y_data);
            } else {
                // Handle stride by uploading row by row
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
                for (int row = 0; row < height; row++) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, width, 1, GL_RED, GL_UNSIGNED_BYTE, 
                                  y_data + row * y_stride);
                }
            }
            printf("YUV strides: Y=%d U=%d V=%d (dimensions: %dx%d, UV: %dx%d)\n", 
                   y_stride, u_stride, v_stride, width, height, uv_width, uv_height);
        } else {
            if (y_stride == width) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, y_data);
            } else {
                for (int row = 0; row < height; row++) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, width, 1, GL_RED, GL_UNSIGNED_BYTE, 
                                  y_data + row * y_stride);
                }
            }
        }
        glUniform1i(gl->u_texture_y, 0);
        
        // Update U texture (handle stride manually for OpenGL ES)
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gl->texture_u);
        
        if (width != last_width || height != last_height || frame_rendered == 0) {
            if (u_stride == uv_width) {
                // No padding, upload directly
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, u_data);
            } else {
                // Handle stride by uploading row by row
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
                for (int row = 0; row < uv_height; row++) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, uv_width, 1, GL_RED, GL_UNSIGNED_BYTE, 
                                  u_data + row * u_stride);
                }
            }
        } else {
            if (u_stride == uv_width) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, u_data);
            } else {
                for (int row = 0; row < uv_height; row++) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, uv_width, 1, GL_RED, GL_UNSIGNED_BYTE, 
                                  u_data + row * u_stride);
                }
            }
        }
        glUniform1i(gl->u_texture_u, 1);
        
        // Update V texture (handle stride manually for OpenGL ES)
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, gl->texture_v);
        
        if (width != last_width || height != last_height || frame_rendered == 0) {
            if (v_stride == uv_width) {
                // No padding, upload directly
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, v_data);
            } else {
                // Handle stride by uploading row by row
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
                for (int row = 0; row < uv_height; row++) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, uv_width, 1, GL_RED, GL_UNSIGNED_BYTE, 
                                  v_data + row * v_stride);
                }
            }
            if (frame_rendered == 0) {
                printf("GPU YUV→RGB rendering started (%dx%d)\n", width, height);
            }
            last_width = width;
            last_height = height;
        } else {
            if (v_stride == uv_width) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, v_data);
            } else {
                for (int row = 0; row < uv_height; row++) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, uv_width, 1, GL_RED, GL_UNSIGNED_BYTE, 
                                  v_data + row * v_stride);
                }
            }
        }
        glUniform1i(gl->u_texture_v, 2);
        
        frame_rendered++;
    } else if (frame_rendered == 0) {
        // Skip test pattern for now - YUV textures need proper initialization
        printf("Waiting for video data...\n");
    }
    
    // Draw quad
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
    // Check for OpenGL errors only occasionally
    if (frame_rendered < 5) {
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            printf("OpenGL error: 0x%x\n", error);
        }
    }
    
    // EGL swap buffers (this makes the rendered frame available)
    EGLBoolean swap_result = eglSwapBuffers(gl->egl_display, gl->egl_surface);
    if (!swap_result && frame_rendered < 5) {
        printf("EGL swap failed: 0x%x\n", eglGetError());
        return;
    }
    
    // Present to display via DRM
    if (drm_swap_buffers(drm) != 0 && frame_rendered < 5) {
        printf("DRM swap failed\n");
    }
}

int gl_create_shaders(gl_context_t *gl) {
    return create_program(gl);
}

void gl_render_corners(gl_context_t *gl, keystone_context_t *keystone) {
    if (!keystone || !keystone->show_corners) {
        return; // Don't render corners if not visible
    }
    
    // Create corner positions (small squares at each corner)
    float corner_size = 0.03f; // 3% of screen size
    
    // Corner vertices for 4 small squares 
    float corner_vertices[32]; // 4 corners * 4 vertices * 2 coords = 32 floats
    
    // Get corner positions from keystone context
    for (int i = 0; i < 4; i++) {
        float cx = keystone->corners[i].x;
        float cy = keystone->corners[i].y;
        
        // Create a small square centered at corner position
        int base = i * 8; // 8 floats per corner (4 vertices * 2 coords)
        corner_vertices[base + 0] = cx - corner_size; // Bottom-left
        corner_vertices[base + 1] = cy - corner_size;
        corner_vertices[base + 2] = cx + corner_size; // Bottom-right
        corner_vertices[base + 3] = cy - corner_size;
        corner_vertices[base + 4] = cx + corner_size; // Top-right
        corner_vertices[base + 5] = cy + corner_size;
        corner_vertices[base + 6] = cx - corner_size; // Top-left
        corner_vertices[base + 7] = cy + corner_size;
    }
    
    // Update corner VBO with new positions
    glBindBuffer(GL_ARRAY_BUFFER, gl->corner_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(corner_vertices), corner_vertices, GL_DYNAMIC_DRAW);
    
    // Use corner shader program
    glUseProgram(gl->corner_program);
    
    // Set up vertex attributes
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(gl->corner_a_position);
    
    // Set identity matrix for MVP (corners in normalized coordinates)
    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->corner_u_mvp_matrix, 1, GL_FALSE, identity);
    
    // Render each corner with different colors
    for (int i = 0; i < 4; i++) {
        // Set color based on corner (and selection state)
        float color[4];
        if (keystone->selected_corner == i) {
            // Highlight selected corner in bright green
            color[0] = 0.0f; color[1] = 1.0f; color[2] = 0.0f; color[3] = 1.0f;
        } else {
            // Regular corners in white with some transparency
            color[0] = 1.0f; color[1] = 1.0f; color[2] = 1.0f; color[3] = 0.8f;
        }
        glUniform4fv(gl->corner_u_color, 1, color);
        
        // Draw corner as triangle fan (4 vertices forming a square)
        glDrawArrays(GL_TRIANGLE_FAN, i * 4, 4);
    }
    
    glDisableVertexAttribArray(gl->corner_a_position);
}

void gl_render_border(gl_context_t *gl, keystone_context_t *keystone) {
    if (!keystone || !keystone->show_border) {
        return; // Don't render border if not visible
    }
    
    // Create border line vertices (4 lines connecting the corners)
    float border_vertices[16]; // 4 lines * 2 vertices * 2 coords = 16 floats
    
    // Get corner positions from keystone context
    point_t *corners = keystone->corners;
    
    // Line 1: Top-left to top-right
    border_vertices[0] = corners[CORNER_TOP_LEFT].x;
    border_vertices[1] = corners[CORNER_TOP_LEFT].y;
    border_vertices[2] = corners[CORNER_TOP_RIGHT].x;
    border_vertices[3] = corners[CORNER_TOP_RIGHT].y;
    
    // Line 2: Top-right to bottom-right
    border_vertices[4] = corners[CORNER_TOP_RIGHT].x;
    border_vertices[5] = corners[CORNER_TOP_RIGHT].y;
    border_vertices[6] = corners[CORNER_BOTTOM_RIGHT].x;
    border_vertices[7] = corners[CORNER_BOTTOM_RIGHT].y;
    
    // Line 3: Bottom-right to bottom-left
    border_vertices[8] = corners[CORNER_BOTTOM_RIGHT].x;
    border_vertices[9] = corners[CORNER_BOTTOM_RIGHT].y;
    border_vertices[10] = corners[CORNER_BOTTOM_LEFT].x;
    border_vertices[11] = corners[CORNER_BOTTOM_LEFT].y;
    
    // Line 4: Bottom-left to top-left
    border_vertices[12] = corners[CORNER_BOTTOM_LEFT].x;
    border_vertices[13] = corners[CORNER_BOTTOM_LEFT].y;
    border_vertices[14] = corners[CORNER_TOP_LEFT].x;
    border_vertices[15] = corners[CORNER_TOP_LEFT].y;
    
    // Update border VBO with new positions
    glBindBuffer(GL_ARRAY_BUFFER, gl->border_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(border_vertices), border_vertices, GL_DYNAMIC_DRAW);
    
    // Use corner shader program (same as corners, just different geometry)
    glUseProgram(gl->corner_program);
    
    // Set up vertex attributes
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(gl->corner_a_position);
    
    // Set identity matrix for MVP (border in normalized coordinates)
    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->corner_u_mvp_matrix, 1, GL_FALSE, identity);
    
    // Set border color (bright cyan for visibility)
    float border_color[4] = {0.0f, 1.0f, 1.0f, 1.0f}; // Cyan
    glUniform4fv(gl->corner_u_color, 1, border_color);
    
    // Set line width for better visibility
    glLineWidth(3.0f);
    
    // Draw border as 4 separate lines
    glDrawArrays(GL_LINES, 0, 8); // 8 vertices (4 lines * 2 vertices each)
    
    // Reset line width
    glLineWidth(1.0f);
    
    glDisableVertexAttribArray(gl->corner_a_position);
}

void gl_render_help_overlay(gl_context_t *gl, keystone_context_t *keystone) {
    if (!keystone || !keystone->show_help) {
        return; // Don't render help overlay if not visible
    }
    
    // Create help overlay background (semi-transparent dark rectangle)
    float help_vertices[] = {
        // Help overlay background (covers most of the screen)
        -0.9f, -0.9f,  // Bottom-left
         0.9f, -0.9f,  // Bottom-right
         0.9f,  0.9f,  // Top-right
        -0.9f,  0.9f,  // Top-left
        
        // Key indicators (small rectangles representing keys)
        // 'Q' key indicator (top-left area)
        -0.85f,  0.7f,
        -0.75f,  0.7f,
        -0.75f,  0.8f,
        -0.85f,  0.8f,
        
        // 'H' key indicator (help)
        -0.65f,  0.7f,
        -0.55f,  0.7f,
        -0.55f,  0.8f,
        -0.65f,  0.8f,
        
        // 'C' key indicator (corners)
        -0.45f,  0.7f,
        -0.35f,  0.7f,
        -0.35f,  0.8f,
        -0.45f,  0.8f,
        
        // 'B' key indicator (border)
        -0.25f,  0.7f,
        -0.15f,  0.7f,
        -0.15f,  0.8f,
        -0.25f,  0.8f,
        
        // Number keys area (1-4)
        -0.85f,  0.5f,
        -0.75f,  0.5f,
        -0.75f,  0.6f,
        -0.85f,  0.6f,
        
        -0.65f,  0.5f,
        -0.55f,  0.5f,
        -0.55f,  0.6f,
        -0.65f,  0.6f,
        
        -0.45f,  0.5f,
        -0.35f,  0.5f,
        -0.35f,  0.6f,
        -0.45f,  0.6f,
        
        -0.25f,  0.5f,
        -0.15f,  0.5f,
        -0.15f,  0.6f,
        -0.25f,  0.6f,
        
        // Arrow keys representation (cross pattern)
        -0.1f,   0.3f,   // Up arrow
         0.0f,   0.3f,
         0.0f,   0.4f,
        -0.1f,   0.4f,
        
        -0.2f,   0.2f,   // Left arrow
        -0.1f,   0.2f,
        -0.1f,   0.3f,
        -0.2f,   0.3f,
        
         0.0f,   0.2f,   // Right arrow
         0.1f,   0.2f,
         0.1f,   0.3f,
         0.0f,   0.3f,
        
        -0.1f,   0.1f,   // Down arrow
         0.0f,   0.1f,
         0.0f,   0.2f,
        -0.1f,   0.2f,
        
        // 'R' key indicator (reset)
        -0.85f,  0.3f,
        -0.75f,  0.3f,
        -0.75f,  0.4f,
        -0.85f,  0.4f,
    };
    
    // Update help VBO with overlay geometry
    glBindBuffer(GL_ARRAY_BUFFER, gl->help_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(help_vertices), help_vertices, GL_DYNAMIC_DRAW);
    
    // Use corner shader program for help overlay rendering
    glUseProgram(gl->corner_program);
    
    // Set up vertex attributes
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(gl->corner_a_position);
    
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
    
    // Draw help overlay background (semi-transparent black)
    float bg_color[4] = {0.0f, 0.0f, 0.0f, 0.7f}; // Dark with 70% opacity
    glUniform4fv(gl->corner_u_color, 1, bg_color);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4); // First 4 vertices form the background
    
    // Draw key indicators in different colors
    float key_colors[][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f}, // Red for Q (quit)
        {0.0f, 1.0f, 0.0f, 1.0f}, // Green for H (help)
        {1.0f, 1.0f, 0.0f, 1.0f}, // Yellow for C (corners)
        {0.0f, 1.0f, 1.0f, 1.0f}, // Cyan for B (border)
        {1.0f, 0.5f, 0.0f, 1.0f}, // Orange for 1
        {1.0f, 0.5f, 0.0f, 1.0f}, // Orange for 2
        {1.0f, 0.5f, 0.0f, 1.0f}, // Orange for 3
        {1.0f, 0.5f, 0.0f, 1.0f}, // Orange for 4
        {1.0f, 1.0f, 1.0f, 1.0f}, // White for arrows
        {1.0f, 1.0f, 1.0f, 1.0f}, // White for arrows
        {1.0f, 1.0f, 1.0f, 1.0f}, // White for arrows
        {1.0f, 1.0f, 1.0f, 1.0f}, // White for arrows
        {0.5f, 0.5f, 1.0f, 1.0f}, // Light blue for R (reset)
    };
    
    // Draw each key indicator (4 vertices each, starting from vertex 4)
    for (int i = 0; i < 13; i++) {
        glUniform4fv(gl->corner_u_color, 1, key_colors[i]);
        glDrawArrays(GL_TRIANGLE_FAN, 4 + i * 4, 4);
    }
    
    // Disable blending
    glDisable(GL_BLEND);
    
    glDisableVertexAttribArray(gl->corner_a_position);
}

void gl_cleanup(gl_context_t *gl) {
    // Clean up YUV textures
    if (gl->texture_y) glDeleteTextures(1, &gl->texture_y);
    if (gl->texture_u) glDeleteTextures(1, &gl->texture_u);
    if (gl->texture_v) glDeleteTextures(1, &gl->texture_v);
    if (gl->vbo) glDeleteBuffers(1, &gl->vbo);
    if (gl->ebo) glDeleteBuffers(1, &gl->ebo);
    if (gl->corner_vbo) glDeleteBuffers(1, &gl->corner_vbo);
    if (gl->border_vbo) glDeleteBuffers(1, &gl->border_vbo);
    if (gl->help_vbo) glDeleteBuffers(1, &gl->help_vbo);
    if (gl->program) glDeleteProgram(gl->program);
    if (gl->corner_program) glDeleteProgram(gl->corner_program);
    if (gl->vertex_shader) glDeleteShader(gl->vertex_shader);
    if (gl->fragment_shader) glDeleteShader(gl->fragment_shader);
    
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