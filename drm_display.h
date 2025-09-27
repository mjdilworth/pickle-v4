#ifndef DRM_DISPLAY_H
#define DRM_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

typedef struct display_ctx {
    int drm_fd;
    struct gbm_device *gbm_device;
    struct gbm_surface *gbm_surface;
    
    drmModeConnector *connector;
    drmModeEncoder *encoder;
    drmModeCrtc *crtc;
    drmModeModeInfo mode;
    
    uint32_t connector_id;
    uint32_t encoder_id;
    uint32_t crtc_id;
    
    uint32_t width;
    uint32_t height;
    uint32_t refresh_rate;
    
    // Framebuffer management
    struct gbm_bo *current_bo;
    struct gbm_bo *next_bo;
    uint32_t current_fb_id;
    uint32_t next_fb_id;
    
    // Page flip state
    bool waiting_for_flip;
    bool mode_set_done;
} display_ctx_t;

// DRM/KMS functions
int drm_init(display_ctx_t *drm);
void drm_cleanup(display_ctx_t *drm);
int drm_set_mode(display_ctx_t *drm, uint32_t fb_id);
int drm_swap_buffers(display_ctx_t *drm);
uint32_t drm_get_fb_for_bo(display_ctx_t *drm, struct gbm_bo *bo);
void drm_page_flip_handler(int fd, unsigned int frame, unsigned int sec, 
                          unsigned int usec, void *data);

#endif // DRM_DISPLAY_H