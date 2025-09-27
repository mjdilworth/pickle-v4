#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE
#include "video_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <linux/input.h>

int app_init(app_context_t *app, const char *video_file, bool loop_playback) {
    memset(app, 0, sizeof(*app));
    app->video_file = video_file;
    app->running = true;
    app->loop_playback = loop_playback;

    // Allocate contexts
    app->drm = calloc(1, sizeof(display_ctx_t));
    app->gl = calloc(1, sizeof(gl_context_t));
    app->video = calloc(1, sizeof(video_context_t));
    app->keystone = calloc(1, sizeof(keystone_context_t));
    app->input = calloc(1, sizeof(input_context_t));

    if (!app->drm || !app->gl || !app->video || !app->keystone || !app->input) {
        fprintf(stderr, "Failed to allocate contexts\n");
        app_cleanup(app);
        return -1;
    }

    // Initialize DRM display
    if (drm_init(app->drm) != 0) {
        fprintf(stderr, "Failed to initialize DRM display\n");
        app_cleanup(app);
        return -1;
    }

    // Initialize OpenGL context
    if (gl_init(app->gl, app->drm) != 0) {
        fprintf(stderr, "Failed to initialize OpenGL context\n");
        app_cleanup(app);
        return -1;
    }

    // Initialize video decoder
    if (video_init(app->video, video_file) != 0) {
        fprintf(stderr, "Failed to initialize video decoder\n");
        app_cleanup(app);
        return -1;
    }
    
    // Set loop playback if requested
    video_set_loop(app->video, loop_playback);

    // Initialize keystone correction
    if (keystone_init(app->keystone) != 0) {
        fprintf(stderr, "Failed to initialize keystone correction\n");
        app_cleanup(app);
        return -1;
    }

    // Initialize input handler
    if (input_init(app->input) != 0) {
        fprintf(stderr, "Failed to initialize input handler\n");
        app_cleanup(app);
        return -1;
    }

    gl_setup_buffers(app->gl);

    // Application initialization complete
    return 0;
}

void app_run(app_context_t *app) {
    struct timespec current_time, last_time;
    clock_gettime(CLOCK_MONOTONIC, &last_time);

    double target_frame_time = 1.0 / 60.0; // 60 FPS target
    if (app->video && app->video->fps > 0) {
        target_frame_time = 1.0 / app->video->fps;
    }

    // Main loop starting

    while (app->running) {
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        
        // Calculate frame time
        double delta_time = (current_time.tv_sec - last_time.tv_sec) +
                           (current_time.tv_nsec - last_time.tv_nsec) / 1e9;

        // Handle input
        input_update(app->input);
        
        // Check for quit
        if (input_should_quit(app->input)) {
            app->running = false;
            break;
        }

        // Handle keystone corner selection
        if (input_is_key_just_pressed(app->input, KEY_1)) {
            keystone_select_corner(app->keystone, CORNER_TOP_LEFT);
        } else if (input_is_key_just_pressed(app->input, KEY_2)) {
            keystone_select_corner(app->keystone, CORNER_TOP_RIGHT);
        } else if (input_is_key_just_pressed(app->input, KEY_3)) {
            keystone_select_corner(app->keystone, CORNER_BOTTOM_RIGHT);
        } else if (input_is_key_just_pressed(app->input, KEY_4)) {
            keystone_select_corner(app->keystone, CORNER_BOTTOM_LEFT);
        }

        // Handle keystone movement
        float move_x = 0.0f, move_y = 0.0f;
        if (input_is_key_pressed(app->input, KEY_LEFT)) move_x = -1.0f;
        if (input_is_key_pressed(app->input, KEY_RIGHT)) move_x = 1.0f;
        if (input_is_key_pressed(app->input, KEY_UP)) move_y = -1.0f;
        if (input_is_key_pressed(app->input, KEY_DOWN)) move_y = 1.0f;

        if (move_x != 0.0f || move_y != 0.0f) {
            keystone_move_corner(app->keystone, move_x, move_y);
        }

        // Reset keystone
        if (input_is_key_just_pressed(app->input, KEY_R)) {
            keystone_reset_corners(app->keystone);
        }
        
        // Toggle corner visibility
        if (app->input->toggle_corners) {
            keystone_toggle_corners(app->keystone);
            app->input->toggle_corners = false; // Reset flag
        }
        
        // Toggle border visibility
        if (app->input->toggle_border) {
            keystone_toggle_border(app->keystone);
            app->input->toggle_border = false; // Reset flag
        }
        
        // Toggle help overlay visibility
        if (app->input->toggle_help) {
            keystone_toggle_help(app->keystone);
            app->input->toggle_help = false; // Reset flag
        }

        // Decode video frame if enough time has passed
        uint8_t *video_data = NULL;
        static int frame_count = 0;
        static uint8_t *last_video_data = NULL;
        
        if (delta_time >= target_frame_time) {
            int decode_result = video_decode_frame(app->video);
            if (decode_result == 0) {
                video_data = video_get_rgb_data(app->video);
                last_video_data = video_data;
                frame_count++;
                last_time = current_time; // Reset frame timer only when we decode
            } else if (video_is_eof(app->video)) {
                printf("Playback finished.\n");
                app->running = false;
                break;
            } else {
                if (frame_count < 10) {  // Only show first few decode errors
                    printf("Video decode failed: %d\n", decode_result);
                }
                // Use last good frame if decode fails
                video_data = last_video_data;
            }
        } else {
            // Use the last decoded frame for rendering
            video_data = last_video_data;
        }

        // Get video dimensions
        int video_width, video_height;
        video_get_dimensions(app->video, &video_width, &video_height);
        if (video_width == 0) {
            video_width = 256;
            video_height = 256;
        }
        
        // Get YUV data for GPU rendering
        uint8_t *y_data, *u_data, *v_data;
        int y_stride, u_stride, v_stride;
        video_get_yuv_data(app->video, &y_data, &u_data, &v_data, &y_stride, &u_stride, &v_stride);
        
        static int debug_frame = 0;
        if (debug_frame < 2) {
            printf("Frame %d: YUV pointers: Y=%p U=%p V=%p\n", 
                   debug_frame, (void*)y_data, (void*)u_data, (void*)v_data);
            debug_frame++;
        }
        
        // Only render if we have valid YUV data
        if (y_data && u_data && v_data) {
            // Render with YUV data (GPU will do YUV→RGB conversion)
            gl_render_frame(app->gl, y_data, u_data, v_data, video_width, video_height, 
                          y_stride, u_stride, v_stride, app->drm, app->keystone);
        } else {
            // Render a blank frame or keep the last frame displayed
            gl_render_frame(app->gl, NULL, NULL, NULL, video_width, video_height, 
                          0, 0, 0, app->drm, app->keystone);
        }
        
        // Render corner highlights if enabled
        gl_render_corners(app->gl, app->keystone);
        
        // Render border highlights if enabled
        gl_render_border(app->gl, app->keystone);
        
        // Render help overlay if enabled (on top of everything)
        gl_render_help_overlay(app->gl, app->keystone);

        // Sleep based on remaining frame time
        double remaining_time = target_frame_time - delta_time;
        if (remaining_time > 0) {
            usleep((useconds_t)(remaining_time * 1000000)); // Convert to microseconds
        } else {
            usleep(100); // Small sleep to prevent 100% CPU usage
        }
    }
}

void app_cleanup(app_context_t *app) {
    if (app->input) {
        input_cleanup(app->input);
        free(app->input);
    }
    if (app->keystone) {
        keystone_cleanup(app->keystone);
        free(app->keystone);
    }
    if (app->video) {
        video_cleanup(app->video);
        free(app->video);
    }
    if (app->gl) {
        gl_cleanup(app->gl);
        free(app->gl);
    }
    if (app->drm) {
        drm_cleanup(app->drm);
        free(app->drm);
    }

    memset(app, 0, sizeof(*app));
    printf("Application cleanup complete\n");
}