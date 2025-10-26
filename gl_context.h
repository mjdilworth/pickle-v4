#ifndef GL_CONTEXT_H
#define GL_CONTEXT_H

#include <EGL/egl.h>
#include <GLES3/gl31.h>  // OpenGL ES 3.1 for modern features
#include <gbm.h>
#include <stdbool.h>
#include "drm_display.h"
#include "keystone.h"
#include "wifi_manager.h"

// Forward declarations
struct display_ctx;

typedef struct {
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLConfig egl_config;
    
    GLuint program;
    GLuint vertex_shader;
    GLuint fragment_shader;
    
    // YUV textures for hardware decoded video
    GLuint texture_y;     // Y plane texture
    GLuint texture_u;     // U plane texture  
    GLuint texture_v;     // V plane texture
    GLuint texture_nv12;  // NV12 packed format texture
    
    // Pixel Buffer Objects for async DMA transfers
    GLuint pbo[3];        // PBOs for Y, U, V planes (async upload)
    bool use_pbo;         // Enable PBO async uploads
    
    GLuint vbo;
    GLuint ebo;
    GLuint vao;
    
    // Modern ES 3.1 Uniform Buffer Objects
    GLuint transform_ubo;      // TransformMatrices UBO
    GLuint video_settings_ubo; // VideoSettings UBO
    
    // UBO binding points
    GLuint transform_binding_point;
    GLuint video_settings_binding_point;
    
    // Legacy uniform locations (fallback)
    GLint u_mvp_matrix;
    GLint u_texture_y;    // Y plane sampler
    GLint u_texture_u;    // U plane sampler
    GLint u_texture_v;    // V plane sampler
    GLint u_texture_nv12; // NV12 packed sampler
    GLint u_keystone_matrix;
    
    // Vertex attributes
    GLint a_position;
    GLint a_texcoord;
    
    // Corner highlighting
    GLuint corner_program;
    GLuint corner_vbo;
    GLint corner_a_position;
    GLint corner_u_mvp_matrix;
    GLint corner_u_color;
    
    // Border rendering
    GLuint border_vbo;
    
    // Help overlay rendering
    GLuint help_vbo;
} gl_context_t;

// OpenGL ES functions
int gl_init(gl_context_t *gl, display_ctx_t *drm);
void gl_cleanup(gl_context_t *gl);
int gl_create_shaders(gl_context_t *gl);
void gl_setup_buffers(gl_context_t *gl);
void gl_render_frame(gl_context_t *gl, uint8_t *y_data, uint8_t *u_data, uint8_t *v_data, 
                    int width, int height, int y_stride, int u_stride, int v_stride,
                    struct display_ctx *drm, keystone_context_t *keystone);
void gl_render_nv12(gl_context_t *gl, uint8_t *nv12_data, int width, int height, int stride,
                    struct display_ctx *drm, keystone_context_t *keystone);
void gl_render_corners(gl_context_t *gl, keystone_context_t *keystone);
void gl_render_border(gl_context_t *gl, keystone_context_t *keystone);
void gl_render_help_overlay(gl_context_t *gl, keystone_context_t *keystone);
void gl_render_wifi_overlay(gl_context_t *gl, wifi_manager_t *mgr);
void gl_swap_buffers(gl_context_t *gl, struct display_ctx *drm);

// Shader source code
extern const char *vertex_shader_source;
extern const char *fragment_shader_source;

#endif // GL_CONTEXT_H