#ifndef DRM_DISPLAY_H
#define DRM_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
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
    
    // KMS video overlay plane for hardware video playback
    uint32_t video_plane_id;        // DRM plane ID for video overlay
    bool video_plane_available;     // True if overlay plane found
    uint32_t video_fb_id;           // Current framebuffer on video plane
    uint32_t prev_video_fb_id;      // Previous framebuffer (for cleanup)
    // Cached atomic property IDs for the video plane (if atomic supported)
    uint32_t video_plane_prop_fb_id;
    uint32_t video_plane_prop_crtc_id;
    uint32_t video_plane_prop_src_x;
    uint32_t video_plane_prop_src_y;
    uint32_t video_plane_prop_src_w;
    uint32_t video_plane_prop_src_h;
    uint32_t video_plane_prop_crtc_x;
    uint32_t video_plane_prop_crtc_y;
    uint32_t video_plane_prop_crtc_w;
    uint32_t video_plane_prop_crtc_h;
    
    // Framebuffer cache for DMA buffers (reuse FBs for decoder's buffer pool)
    struct {
        int dma_fd;                 // DMA buffer FD
        uint32_t fb_id;             // Corresponding framebuffer ID
    } fb_cache[8];                  // Cache up to 8 framebuffers (typical pool size)
    int fb_cache_count;
    
    // Worker thread for non-blocking plane updates
    pthread_t plane_worker_thread;
    pthread_mutex_t plane_mutex;
    pthread_cond_t plane_cond;
    bool plane_worker_running;
    bool plane_worker_shutdown;
    
    // Pending plane update (single-item mailbox)
    struct {
        uint32_t fb_id;
        uint32_t x, y;
        uint32_t width, height;
        bool pending;
    } plane_update;
} display_ctx_t;

// DRM/KMS functions
int drm_init(display_ctx_t *drm);
void drm_cleanup(display_ctx_t *drm);
int drm_set_mode(display_ctx_t *drm, uint32_t fb_id);
int drm_swap_buffers(display_ctx_t *drm);
uint32_t drm_get_fb_for_bo(display_ctx_t *drm, struct gbm_bo *bo);
void drm_page_flip_handler(int fd, unsigned int frame, unsigned int sec, 
                          unsigned int usec, void *data);

// KMS video overlay plane functions (for hardware zero-copy video)
int drm_init_video_plane(display_ctx_t *drm);
int drm_create_video_fb(display_ctx_t *drm, int dma_fd, uint32_t width, uint32_t height,
                        int plane_offsets[3], int plane_pitches[3], uint32_t *fb_id_out);
int drm_display_video_frame(display_ctx_t *drm, uint32_t fb_id, uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height);
void drm_hide_video_plane(display_ctx_t *drm);

#endif // DRM_DISPLAY_H