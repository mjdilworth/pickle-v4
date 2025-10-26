#ifndef VIDEO_PLAYER_H
#define VIDEO_PLAYER_H

#include <stdint.h>
#include <stdbool.h>

// Include the actual struct definitions so we can use them
#include "drm_display.h"
#include "gl_context.h"
#include "video_decoder.h"
#include "keystone.h"
#include "input_handler.h"
#include "wifi_manager.h"

// Main application context
typedef struct {
    video_context_t *video;
    display_ctx_t *drm;
    gl_context_t *gl;
    input_context_t *input;
    keystone_context_t *keystone;
    wifi_manager_t wifi_mgr;
    bool running;
    bool loop_playback;
    const char *video_file;
    bool needs_redraw;       // Flag to indicate frame needs redrawing
    bool show_timing;        // Flag to show frame timing information
    bool debug_gamepad;      // Flag to log gamepad button presses
    bool advanced_diagnostics; // Flag to enable detailed hardware decoder diagnostics
    bool show_wifi_overlay;  // Show WiFi connection overlay
} app_context_t;

// Main application functions
int app_init(app_context_t *app, const char *video_file, bool loop_playback, bool show_timing, bool debug_gamepad, bool advanced_diagnostics);
void app_run(app_context_t *app);
void app_cleanup(app_context_t *app);

#endif // VIDEO_PLAYER_H