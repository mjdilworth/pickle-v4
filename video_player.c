#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE
#include "video_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <linux/input.h>
#include <sys/stat.h>
#include "production_config.h"

// Validate video file before processing
static int validate_video_file(const char *filename) {
    if (!filename) {
        fprintf(stderr, "Error: No video file specified\n");
        return -1;
    }
    
    // Check if file exists and get size
    struct stat st;
    if (stat(filename, &st) != 0) {
        fprintf(stderr, "Error: Cannot access video file: %s\n", filename);
        return -1;
    }
    
    // Check file size limits
    if (st.st_size > MAX_VIDEO_FILE_SIZE) {
        fprintf(stderr, "Error: Video file too large (%lld bytes, limit: %lld bytes)\n",
                (long long)st.st_size, (long long)MAX_VIDEO_FILE_SIZE);
        return -1;
    }
    
    if (st.st_size < 1024) {  // Minimum 1KB for valid video
        fprintf(stderr, "Error: Video file too small (%lld bytes)\n", (long long)st.st_size);
        return -1;
    }
    
    printf("Video file validation passed: %s (%lld bytes)\n", filename, (long long)st.st_size);
    return 0;
}

static bool process_keystone_movement(app_context_t *app, double delta_time, double target_frame_time) {
    if (!app || !app->input) {
        return false;
    }
    
    // Get active keystone
    keystone_context_t *active_ks = (app->active_keystone == 1 && app->keystone2) ? app->keystone2 : app->keystone;
    if (!active_ks) {
        return false;
    }

    bool left = input_is_key_pressed(app->input, KEY_LEFT);
    bool right = input_is_key_pressed(app->input, KEY_RIGHT);
    bool up = input_is_key_pressed(app->input, KEY_UP);
    bool down = input_is_key_pressed(app->input, KEY_DOWN);

    if (app->input->use_stdin_fallback) {
        left = left || input_is_key_just_pressed(app->input, KEY_LEFT);
        right = right || input_is_key_just_pressed(app->input, KEY_RIGHT);
        up = up || input_is_key_just_pressed(app->input, KEY_UP);
        down = down || input_is_key_just_pressed(app->input, KEY_DOWN);
    }
    
    // Add gamepad input (D-pad and left analog stick)
    if (app->input->gamepad_enabled) {
        // D-pad input
        if (app->input->gamepad_dpad_x < 0) left = true;
        if (app->input->gamepad_dpad_x > 0) right = true;
        if (app->input->gamepad_dpad_y < 0) up = true;
        if (app->input->gamepad_dpad_y > 0) down = true;
        
        // Analog stick input (with deadzone)
        const int16_t deadzone = 8000; // ~25% deadzone
        if (app->input->gamepad_axis_x < -deadzone) left = true;
        if (app->input->gamepad_axis_x > deadzone) right = true;
        if (app->input->gamepad_axis_y < -deadzone) up = true;
        if (app->input->gamepad_axis_y > deadzone) down = true;
    }

    float move_x = 0.0f;
    float move_y = 0.0f;

    if (left != right) {
        move_x = left ? -1.0f : 1.0f;
    }
    if (up != down) {
        move_y = up ? 1.0f : -1.0f;
    }

    if (active_ks->selected_corner < 0 || (move_x == 0.0f && move_y == 0.0f)) {
        return false;
    }
    
    // Only allow movement if border or corners are visible
    if (!active_ks->show_border && !active_ks->show_corners) {
        return false;
    }

    float speed_scale = 1.0f;
    if (target_frame_time > 0.0 && delta_time > 0.0) {
        speed_scale = (float)(delta_time / target_frame_time);
        if (speed_scale < 0.25f) speed_scale = 0.25f;
        if (speed_scale > 3.0f) speed_scale = 3.0f;
    }

    keystone_move_corner(active_ks, move_x * speed_scale, move_y * speed_scale);
    app->needs_redraw = true;
    return true;
}

int app_init(app_context_t *app, const char *video_file, const char *video_file2, bool loop_playback, 
            bool show_timing, bool debug_gamepad, bool advanced_diagnostics) {
    printf("app_init: Starting initialization...\n");
    fflush(stdout);
    
    // Validate video file before proceeding
    if (validate_video_file(video_file) != 0) {
        fprintf(stderr, "Failed to validate video file\n");
        return -1;
    }
    
    // Validate second video file if provided
    if (video_file2 && validate_video_file(video_file2) != 0) {
        fprintf(stderr, "Failed to validate second video file\n");
        return -1;
    }
    
    memset(app, 0, sizeof(*app));
    
    // Set all flags
    app->video_file = video_file;
    app->video_file2 = video_file2;
    app->running = true;
    app->loop_playback = loop_playback;
    app->show_timing = show_timing;
    app->debug_gamepad = debug_gamepad;
    app->advanced_diagnostics = advanced_diagnostics;
    app->active_keystone = 0;  // Start with first keystone active

    // Allocate contexts
    app->drm = calloc(1, sizeof(display_ctx_t));
    app->gl = calloc(1, sizeof(gl_context_t));
    app->video = calloc(1, sizeof(video_context_t));
    app->keystone = calloc(1, sizeof(keystone_context_t));
    app->input = calloc(1, sizeof(input_context_t));
    
    // Allocate second video and keystone if second file provided
    if (video_file2) {
        app->video2 = calloc(1, sizeof(video_context_t));
        app->keystone2 = calloc(1, sizeof(keystone_context_t));
        if (!app->video2 || !app->keystone2) {
            fprintf(stderr, "Failed to allocate second video/keystone contexts\n");
            app_cleanup(app);
            return -1;
        }
    }

    if (!app->drm || !app->gl || !app->video || !app->keystone || !app->input) {
        fprintf(stderr, "Failed to allocate contexts\n");
        app_cleanup(app);
        return -1;
    }

    printf("app_init: Initializing DRM display...\n");
    fflush(stdout);
    
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
    if (video_init(app->video, video_file, app->advanced_diagnostics) != 0) {
        fprintf(stderr, "Failed to initialize video decoder\n");
        app_cleanup(app);
        return -1;
    }
    
    // Validate video dimensions after decoder opens file
    if (app->video->width > MAX_VIDEO_WIDTH || app->video->height > MAX_VIDEO_HEIGHT) {
        fprintf(stderr, "Error: Video dimensions %dx%d exceed limits (%dx%d max)\n",
                app->video->width, app->video->height, MAX_VIDEO_WIDTH, MAX_VIDEO_HEIGHT);
        app_cleanup(app);
        return -1;
    }
    
    printf("Video 1 dimensions: %dx%d (within limits)\n", app->video->width, app->video->height);
    
    // Set loop playback if requested
    video_set_loop(app->video, loop_playback);

    // Initialize second video decoder if provided
    if (video_file2) {
        if (video_init(app->video2, video_file2, app->advanced_diagnostics) != 0) {
            fprintf(stderr, "Failed to initialize second video decoder\n");
            app_cleanup(app);
            return -1;
        }
        
        if (app->video2->width > MAX_VIDEO_WIDTH || app->video2->height > MAX_VIDEO_HEIGHT) {
            fprintf(stderr, "Error: Video 2 dimensions %dx%d exceed limits (%dx%d max)\n",
                    app->video2->width, app->video2->height, MAX_VIDEO_WIDTH, MAX_VIDEO_HEIGHT);
            app_cleanup(app);
            return -1;
        }
        
        printf("Video 2 dimensions: %dx%d (within limits)\n", app->video2->width, app->video2->height);
        video_set_loop(app->video2, loop_playback);
    }

    // Initialize keystone correction
    if (keystone_init(app->keystone) != 0) {
        fprintf(stderr, "Failed to initialize keystone correction\n");
        app_cleanup(app);
        return -1;
    }
    
    // Load saved keystone settings if available
    if (keystone_load_settings(app->keystone) == 0) {
        printf("Loaded saved keystone settings from pickle_keystone.conf\n");
    } else {
        printf("No saved keystone settings found, using defaults\n");
    }
    
    // Initialize second keystone if second video provided
    if (video_file2) {
        if (keystone_init(app->keystone2) != 0) {
            fprintf(stderr, "Failed to initialize second keystone correction\n");
            app_cleanup(app);
            return -1;
        }
        
        // Load saved keystone settings for second keystone
        if (keystone_load_from_file(app->keystone2, "pickle_keystone2.conf") == 0) {
            printf("Loaded saved keystone2 settings from pickle_keystone2.conf\n");
        } else {
            printf("No saved keystone2 settings found, using defaults\n");
        }
    }

    // Initialize input handler
    if (input_init(app->input) != 0) {
        fprintf(stderr, "Failed to initialize input handler\n");
        app_cleanup(app);
        return -1;
    }
    
    // Pass debug flag to input handler
    app->input->debug_gamepad = app->debug_gamepad;

    gl_setup_buffers(app->gl);

    // Application initialization complete
    return 0;
}

void app_run(app_context_t *app) {
    struct timespec current_time, last_time;
    clock_gettime(CLOCK_MONOTONIC, &last_time);

    // Print app configuration to verify settings
    printf("App configuration - Loop: %d, Show timing: %d, Debug gamepad: %d\n",
           app->loop_playback ? 1 : 0, 
           app->show_timing ? 1 : 0, 
           app->debug_gamepad ? 1 : 0);
    
    // Force timing display for debugging if needed
    if (!app->show_timing) {
        char *env_timing = getenv("PICKLE_SHOW_TIMING");
        if (env_timing && (strcmp(env_timing, "1") == 0 || 
                          strcmp(env_timing, "yes") == 0 || 
                          strcmp(env_timing, "true") == 0)) {
            app->show_timing = true;
            printf("[FORCE] Timing display enabled via PICKLE_SHOW_TIMING environment variable\n");
        }
    }
    fflush(stdout);

    double target_frame_time = 1.0 / 60.0; // 60 FPS target
    if (app->video && app->video->fps > 0) {
        target_frame_time = 1.0 / app->video->fps;
        printf("Video FPS: %.2f, Target frame time: %.3fms\n", 
               app->video->fps, target_frame_time * 1000);
        fflush(stdout);
    }
    
    // IMPROVED: Simpler, more stable frame timing
    double frame_budget = target_frame_time;  // How much time we have per frame
    // 10% buffer for timing overhead
    
    // Adaptive frame timing variables
    double min_frame_time = target_frame_time;  // Minimum frame time (target FPS)
    double adaptive_frame_time = target_frame_time;  // Current adaptive frame time
    
    // Frame timing diagnostics
    struct timespec decode_start, decode_end, render_start, render_end;
    double total_decode_time = 0, total_render_time = 0;
    int diagnostic_frame_count = 0;
    
    // Add a timer to ensure we print timing data periodically regardless of frame counts
    struct timespec last_timing_report;
    clock_gettime(CLOCK_MONOTONIC, &last_timing_report);
    
    // Initialize timing metrics display
    if (app->show_timing) {
        printf("\n[TIMING] Timing display is enabled. Will show metrics every 30 frames.\n");
        printf("[TIMING] Video FPS: %.2f, Target frame time: %.3fms\n", 
               app->video->fps, target_frame_time * 1000);
        printf("[TIMING] Hardware decode: %s, Resolution: %dx%d\n", 
               video_is_hardware_decoded(app->video) ? "YES" : "NO",
               app->video->width, app->video->height);
               
        // Start a background timer to ensure timing info is displayed
        // regardless of frame decode success
        printf("[TIMING] Starting timing display timer\n");
        
        // Write directly to a file as a backup method for timing data
        FILE *timing_log = fopen("timing_log.txt", "w");
        if (timing_log) {
            fprintf(timing_log, "Pickle timing log started\n");
            fprintf(timing_log, "Video FPS: %.2f, Target frame time: %.3fms\n", 
                   app->video->fps, target_frame_time * 1000);
            fprintf(timing_log, "Hardware decode: %s, Resolution: %dx%d\n", 
                   video_is_hardware_decoded(app->video) ? "YES" : "NO",
                   app->video->width, app->video->height);
            fclose(timing_log);
        }
        fflush(stdout);
    }

    // Main loop starting
    double startup_time = (double)last_time.tv_sec + last_time.tv_nsec / 1e9;
    bool first_decode_attempted = false;
    
    while (app->running) {
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        
        // Calculate frame time
        double delta_time = (current_time.tv_sec - last_time.tv_sec) +
                           (current_time.tv_nsec - last_time.tv_nsec) / 1e9;
        
        double current_total_time = (double)current_time.tv_sec + current_time.tv_nsec / 1e9;
        double decode_time = 0.0, render_time = 0.0;
        
        // Handle input regardless of video state
        input_update(app->input);
        
        // Check for quit
        if (input_should_quit(app->input)) {
            printf("Quit requested by user\n");
            fflush(stdout);
            app->running = false;
            break;
        }

        // Handle keystone corner selection - match visual numbering
        // Keys 1-4 select corners for first keystone (video 1)
        // Keys 5-8 select corners for second keystone (video 2)
        if (input_is_key_just_pressed(app->input, KEY_1)) {
            keystone_select_corner(app->keystone, CORNER_BOTTOM_LEFT);  // Visual "1" (bottom-left)
            app->active_keystone = 0;
        } else if (input_is_key_just_pressed(app->input, KEY_2)) {
            keystone_select_corner(app->keystone, CORNER_BOTTOM_RIGHT); // Visual "2" (bottom-right)
            app->active_keystone = 0;
        } else if (input_is_key_just_pressed(app->input, KEY_3)) {
            keystone_select_corner(app->keystone, CORNER_TOP_RIGHT);    // Visual "3" (top-right)
            app->active_keystone = 0;
        } else if (input_is_key_just_pressed(app->input, KEY_4)) {
            keystone_select_corner(app->keystone, CORNER_TOP_LEFT);     // Visual "4" (top-left)
            app->active_keystone = 0;
        } else if (app->keystone2 && input_is_key_just_pressed(app->input, KEY_5)) {
            keystone_select_corner(app->keystone2, CORNER_TOP_LEFT);     // Key "5" = TL (next to keystone1 TR)
            app->active_keystone = 1;
        } else if (app->keystone2 && input_is_key_just_pressed(app->input, KEY_6)) {
            keystone_select_corner(app->keystone2, CORNER_TOP_RIGHT);    // Key "6" = TR
            app->active_keystone = 1;
        } else if (app->keystone2 && input_is_key_just_pressed(app->input, KEY_7)) {
            keystone_select_corner(app->keystone2, CORNER_BOTTOM_RIGHT); // Key "7" = BR
            app->active_keystone = 1;
        } else if (app->keystone2 && input_is_key_just_pressed(app->input, KEY_8)) {
            keystone_select_corner(app->keystone2, CORNER_BOTTOM_LEFT);  // Key "8" = BL
            app->active_keystone = 1;
        }
        
        // Check for arrow key input (simply helps the corner movement by triggering input_is_key_pressed)

        // Get active keystone
        keystone_context_t *active_ks = (app->active_keystone == 1 && app->keystone2) ? app->keystone2 : app->keystone;
        
        // Reset keystone to defaults
        if (input_is_key_just_pressed(app->input, KEY_R)) {
            keystone_reset_corners(active_ks);
            printf("Keystone %d reset to defaults\n", app->active_keystone + 1);
        }
        
        // Save keystone settings (S key or P key for compatibility)
        if (app->input->save_keystone) {
            if (app->active_keystone == 0) {
                if (keystone_save_settings(app->keystone) == 0) {
                    printf("Keystone 1 settings saved to pickle_keystone.conf\n");
                } else {
                    printf("Failed to save keystone 1 settings\n");
                }
            } else if (app->keystone2) {
                if (keystone_save_to_file(app->keystone2, "pickle_keystone2.conf") == 0) {
                    printf("Keystone 2 settings saved to pickle_keystone2.conf\n");
                } else {
                    printf("Failed to save keystone 2 settings\n");
                }
            }
            app->input->save_keystone = false; // Reset flag
        }
        
        // Toggle corner visibility
        if (app->input->toggle_corners) {
            keystone_toggle_corners(active_ks);
            app->input->toggle_corners = false; // Reset flag
        }
        
        // Toggle border visibility
        if (app->input->toggle_border) {
            keystone_toggle_border(active_ks);
            printf("Border toggled: %s (keystone %d)\n", active_ks->show_border ? "ON" : "OFF", app->active_keystone + 1);
            app->input->toggle_border = false; // Reset flag
        }
        
        // Toggle help overlay visibility
        if (app->input->toggle_help) {
            keystone_toggle_help(active_ks);
            app->input->toggle_help = false; // Reset flag
        }
        
        // Gamepad-specific actions
        if (app->input->gamepad_enabled) {
            // X button: Cycle through corners
            if (app->input->gamepad_cycle_corner) {
                int current = active_ks->selected_corner;
                
                // If no corner selected, start with top-left
                if (current < 0 || current > 3) {
                    current = CORNER_TOP_LEFT;
                    keystone_select_corner(active_ks, current);
                    printf("[X-CYCLE] Starting with index %d (TOP_LEFT) keystone %d\n", current, app->active_keystone + 1);
                } else {
                    // Cycle clockwise: TOP_LEFT(0) -> BOTTOM_LEFT(3) -> BOTTOM_RIGHT(2) -> TOP_RIGHT(1) -> TOP_LEFT
                    int next;
                    switch (current) {
                        case CORNER_TOP_LEFT:     next = CORNER_BOTTOM_LEFT; break;
                        case CORNER_BOTTOM_LEFT:  next = CORNER_BOTTOM_RIGHT; break;
                        case CORNER_BOTTOM_RIGHT: next = CORNER_TOP_RIGHT; break;
                        case CORNER_TOP_RIGHT:    next = CORNER_TOP_LEFT; break;
                        default:                  next = CORNER_TOP_LEFT; break;
                    }
                    keystone_select_corner(active_ks, next);
                    const char* names[] = {"TL", "TR", "BR", "BL"};
                    printf("[X-CYCLE] %s(%.2f,%.2f) -> %s(%.2f,%.2f) keystone %d\n", 
                           names[current], active_ks->corners[current].x, active_ks->corners[current].y,
                           names[next], active_ks->corners[next].x, active_ks->corners[next].y,
                           app->active_keystone + 1);
                }
                app->input->gamepad_cycle_corner = false;
            }
            
            // L1/R1: Decrease/increase step size
            if (app->input->gamepad_decrease_step) {
                keystone_decrease_step_size(active_ks);
                printf("[GAMEPAD] R1 - Step size decreased to %.6f (keystone %d)\n", active_ks->move_step, app->active_keystone + 1);
                app->input->gamepad_decrease_step = false;
            }
            if (app->input->gamepad_increase_step) {
                keystone_increase_step_size(active_ks);
                printf("[GAMEPAD] L1 - Step size increased to %.6f (keystone %d)\n", active_ks->move_step, app->active_keystone + 1);
                app->input->gamepad_increase_step = false;
            }
            
            // SELECT: Reset keystone
            if (app->input->gamepad_reset_keystone) {
                keystone_reset_corners(app->keystone);
                printf("Keystone reset to defaults (gamepad)\n");
                app->input->gamepad_reset_keystone = false;
            }
            
            // START: Toggle keystone mode (corners visibility)
            if (app->input->gamepad_toggle_mode) {
                keystone_toggle_corners(app->keystone);
                app->input->gamepad_toggle_mode = false;
            }
        }

        // OPTIMIZATION: Pre-decode next frame while rendering current frame
        // This hides decode latency by overlapping decode with render/swap
        static bool next_frame_ready = false;
        static bool first_frame_decoded = false;
        static bool next_frame_ready2 = false;
        static bool first_frame_decoded2 = false;
        
        // Decode video frame if enough time has passed
        uint8_t *video_data = NULL;
        uint8_t *video_data2 = NULL;
        static int frame_count = 0;
        static int frame_count2 = 0;
        static uint8_t *last_video_data = NULL;
        static uint8_t *last_video_data2 = NULL;
        
        if (delta_time >= frame_budget) {
            // Update timing first to maintain consistent frame rate
            last_time = current_time;
            
            if (frame_count == 0 && !first_decode_attempted) {
                printf("Attempting first frame decode...\n");
                fflush(stdout);
                first_decode_attempted = true;
            }
            
            // Add timeout for first decode to prevent hanging
            if (frame_count == 0 && (current_total_time - startup_time) > 5.0) {
                printf("Video decode timeout after 5 seconds, continuing without video...\n");
                fflush(stdout);
                frame_count = 1; // Skip further decode attempts
            } else {
                // OPTIMIZATION: Use pre-decoded frame if available, then decode next
                if (next_frame_ready && first_frame_decoded) {
                    // Current frame was already decoded - just use it (zero decode time!)
                    decode_time = 0.0;
                    
                    // Get YUV data from already-decoded frame
                    uint8_t *y_data = NULL, *u_data = NULL, *v_data = NULL;
                    int y_stride = 0, u_stride = 0, v_stride = 0;
                    video_get_yuv_data(app->video, &y_data, &u_data, &v_data, &y_stride, &u_stride, &v_stride);
                    
                    if (y_data != NULL) {
                        video_data = y_data;
                        last_video_data = video_data;
                        frame_count++;
                        
                        total_decode_time += decode_time;
                        diagnostic_frame_count++;
                        
                        if (app->show_timing && (frame_count % 10 == 0 || frame_count < 10)) {
                            printf("DEBUG: Frame %d (pre-decoded, 0.0ms), diagnostic_frame_count=%d\n", 
                                   frame_count, diagnostic_frame_count);
                            fflush(stdout);
                        }
                        
                        // Now decode the NEXT frame (during rendering of current frame)
                        // This will be ready for next iteration
                        next_frame_ready = false;
                        // Actual decode happens after rendering (see below)
                    }
                } else {
                    // Normal decode path (first frame or fallback)
                    clock_gettime(CLOCK_MONOTONIC, &decode_start);
                    int decode_result = video_decode_frame(app->video);
                    clock_gettime(CLOCK_MONOTONIC, &decode_end);
                    
                    decode_time = (decode_end.tv_sec - decode_start.tv_sec) +
                                (decode_end.tv_nsec - decode_start.tv_nsec) / 1e9;
                    
                    if (decode_result == 0) {
                        if (frame_count == 0) {
                            printf("First frame decoded successfully\n");
                            first_frame_decoded = true;
                            fflush(stdout);
                        }
                        
                        uint8_t *y_data = NULL, *u_data = NULL, *v_data = NULL;
                        int y_stride = 0, u_stride = 0, v_stride = 0;
                        video_get_yuv_data(app->video, &y_data, &u_data, &v_data, &y_stride, &u_stride, &v_stride);
                        
                        if (y_data != NULL) {
                            video_data = y_data;
                            last_video_data = video_data;
                            frame_count++;
                            
                            total_decode_time += decode_time;
                            diagnostic_frame_count++;
                            
                            if (app->show_timing && (frame_count % 10 == 0 || frame_count < 10)) {
                                printf("DEBUG: Frame %d decoded (%.1fms), diagnostic_frame_count=%d\n", 
                                       frame_count, decode_time * 1000, diagnostic_frame_count);
                                fflush(stdout);
                            }
                            
                            // Mark that we should pre-decode next time
                            next_frame_ready = true;
                        }
                    } else if (video_is_eof(app->video)) {
                        if (app->loop_playback) {
                            printf("End of video reached - restarting playback (loop mode)\n");
                            video_seek(app->video, 0);
                            next_frame_ready = false;
                            first_frame_decoded = false;
                            first_decode_attempted = false;  // Reset to allow "Attempting first frame" message
                            frame_count = 0;
                            // Update startup_time to reset the 5-second timeout
                            clock_gettime(CLOCK_MONOTONIC, &startup_time);
                        } else {
                            printf("Playback finished.\n");
                            app->running = false;
                            break;
                        }
                    } else {
                        if (frame_count < 10) {
                            printf("Video decode failed: %d\n", decode_result);
                            fflush(stdout);
                        }
                        video_data = last_video_data;
                        next_frame_ready = false;
                    }
                }
                
                // Measure decode time (removed old code)
                // clock_gettime(CLOCK_MONOTONIC, &decode_start);
                    
                    // Show diagnostic info periodically
                    if (app->show_timing && diagnostic_frame_count % 30 == 0) {
                        double avg_decode_time = total_decode_time / diagnostic_frame_count;
                        printf("Frame timing - Avg decode: %.1fms, Target: %.1fms, Frame count: %d\n",
                               avg_decode_time * 1000, target_frame_time * 1000, diagnostic_frame_count);
                        fflush(stdout);
                    }
                    
                                        // We now clear the arrow keys ONLY if we've successfully processed a movement
            // This allows for continued movement if a corner is not selected
            
                // Decode second video if available
                if (app->video2) {
                    if (next_frame_ready2 && first_frame_decoded2) {
                        // Use pre-decoded frame
                        uint8_t *y_data2 = NULL, *u_data2 = NULL, *v_data2 = NULL;
                        int y_stride2 = 0, u_stride2 = 0, v_stride2 = 0;
                        video_get_yuv_data(app->video2, &y_data2, &u_data2, &v_data2, &y_stride2, &u_stride2, &v_stride2);
                        
                        if (y_data2 != NULL) {
                            video_data2 = y_data2;
                            last_video_data2 = video_data2;
                            frame_count2++;
                        }
                        next_frame_ready2 = false;
                    } else {
                        // Decode new frame for video 2
                        int decode_result2 = video_decode_frame(app->video2);
                        
                        if (decode_result2 == 0) {
                            if (frame_count2 == 0) {
                                printf("First frame of video 2 decoded successfully\n");
                                first_frame_decoded2 = true;
                                fflush(stdout);
                            }
                            
                            uint8_t *y_data2 = NULL, *u_data2 = NULL, *v_data2 = NULL;
                            int y_stride2 = 0, u_stride2 = 0, v_stride2 = 0;
                            video_get_yuv_data(app->video2, &y_data2, &u_data2, &v_data2, &y_stride2, &u_stride2, &v_stride2);
                            
                            if (y_data2 != NULL) {
                                video_data2 = y_data2;
                                last_video_data2 = video_data2;
                                frame_count2++;
                                next_frame_ready2 = true;
                            }
                        } else if (video_is_eof(app->video2)) {
                            if (app->loop_playback) {
                                video_seek(app->video2, 0);
                                next_frame_ready2 = false;
                                first_frame_decoded2 = false;
                                frame_count2 = 0;
                            }
                        } else {
                            video_data2 = last_video_data2;
                            next_frame_ready2 = false;
                        }
                    }
                }
                
            if (app->keystone->selected_corner < 0) {
                // No corner selected, clear key states to prevent movement later
                app->input->keys_pressed[KEY_UP] = false;
                app->input->keys_pressed[KEY_DOWN] = false;
                app->input->keys_pressed[KEY_LEFT] = false;
                app->input->keys_pressed[KEY_RIGHT] = false;
            }
            }  // Close the if (delta_time >= frame_budget) block
        } else {
            // Use the last decoded frame for rendering
            video_data = last_video_data;
            video_data2 = last_video_data2;
        }

        // Get video dimensions
        int video_width = 0, video_height = 0;
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
            fflush(stdout);
        }
        
        // Measure render time with detailed breakdown
        clock_gettime(CLOCK_MONOTONIC, &render_start);
        struct timespec gl_render_start, gl_render_end, overlay_start, overlay_end, swap_start, swap_end;
        
        // Time the main GL rendering
        clock_gettime(CLOCK_MONOTONIC, &gl_render_start);
        
        // Render first video with first keystone (clear screen)
        uint8_t *nv12_data = video_get_nv12_data(app->video);
        int nv12_stride = video_get_nv12_stride(app->video);
        
        // DISABLED: NV12 rendering causes color issues - use separate YUV planes instead
        if (false && nv12_data) {
            // Render with NV12 packed format (faster - single texture upload)
            gl_render_nv12(app->gl, nv12_data, video_width, video_height, nv12_stride, app->drm, app->keystone, true);
        } else {
            // Fallback to original YUV rendering (for compatibility)
            video_get_yuv_data(app->video, &y_data, &u_data, &v_data, &y_stride, &u_stride, &v_stride);
            
            if (y_data && u_data && v_data) {
                gl_render_frame(app->gl, y_data, u_data, v_data, video_width, video_height, 
                              y_stride, u_stride, v_stride, app->drm, app->keystone, true);
            } else {
                gl_render_frame(app->gl, NULL, NULL, NULL, video_width, video_height, 
                              0, 0, 0, app->drm, app->keystone, true);
            }
        }
        
        // Render second video with second keystone (don't clear screen)
        if (app->video2 && app->keystone2 && video_data2) {
            uint8_t *y_data2 = NULL, *u_data2 = NULL, *v_data2 = NULL;
            int y_stride2 = 0, u_stride2 = 0, v_stride2 = 0;
            int video_width2 = app->video2->width;
            int video_height2 = app->video2->height;
            
            video_get_yuv_data(app->video2, &y_data2, &u_data2, &v_data2, &y_stride2, &u_stride2, &v_stride2);
            
            if (y_data2 && u_data2 && v_data2) {
                gl_render_frame(app->gl, y_data2, u_data2, v_data2, video_width2, video_height2, 
                              y_stride2, u_stride2, v_stride2, app->drm, app->keystone2, false);
            }
        }
        
        clock_gettime(CLOCK_MONOTONIC, &gl_render_end);
        
        // Time overlay rendering
        clock_gettime(CLOCK_MONOTONIC, &overlay_start);
        
        // OPTIMIZED: Only render overlays when actually needed
        // Render overlays for first keystone
        bool any_overlay_visible = (app->keystone && (app->keystone->show_corners || 
                                                     app->keystone->show_border || 
                                                     app->keystone->show_help));
        if (any_overlay_visible) {
            // Only render the overlays that are actually visible
            if (app->keystone->show_corners) {
                gl_render_corners(app->gl, app->keystone);
            }
            if (app->keystone->show_border) {
                static int border_render_count = 0;
                if (border_render_count < 3) {  // Only log first 3 times
                    printf("Rendering border overlay (frame %d)\n", border_render_count + 1);
                    border_render_count++;
                }
                gl_render_border(app->gl, app->keystone);
            }
            if (app->keystone->show_help) {
                gl_render_help_overlay(app->gl, app->keystone);
            }
            
            // NOTE: State restoration is now handled by gl_render_frame()
            // The main video rendering function detects and fixes state corruption
            
            // Clear any OpenGL errors from overlay rendering
            while (glGetError() != GL_NO_ERROR) {
                // Clear error queue
            }
        }
        
        // Render overlays for second keystone
        bool any_overlay_visible2 = (app->keystone2 && (app->keystone2->show_corners || 
                                                       app->keystone2->show_border || 
                                                       app->keystone2->show_help));
        if (any_overlay_visible2) {
            if (app->keystone2->show_corners) {
                gl_render_corners(app->gl, app->keystone2);
            }
            if (app->keystone2->show_border) {
                gl_render_border(app->gl, app->keystone2);
            }
            if (app->keystone2->show_help) {
                gl_render_help_overlay(app->gl, app->keystone2);
            }
            
            while (glGetError() != GL_NO_ERROR) {
                // Clear error queue
            }
        }
        
        clock_gettime(CLOCK_MONOTONIC, &overlay_end);
        
        // Time buffer swapping
        clock_gettime(CLOCK_MONOTONIC, &swap_start);
        
        // Swap buffers after all rendering is complete
        gl_swap_buffers(app->gl, app->drm);
        
        clock_gettime(CLOCK_MONOTONIC, &swap_end);
        
        // OPTIMIZATION: Pre-decode next frame while GPU presents current frame
        // This overlaps decode with VSync wait, hiding decode latency
        if (first_frame_decoded && !next_frame_ready && frame_count > 0) {
            struct timespec predecode_start, predecode_end;
            clock_gettime(CLOCK_MONOTONIC, &predecode_start);
            
            int predecode_result = video_decode_frame(app->video);
            
            clock_gettime(CLOCK_MONOTONIC, &predecode_end);
            double predecode_time = (predecode_end.tv_sec - predecode_start.tv_sec) +
                                   (predecode_end.tv_nsec - predecode_start.tv_nsec) / 1e9;
            
            if (predecode_result == 0) {
                next_frame_ready = true;
                if (app->show_timing && frame_count < 5) {
                    printf("Pre-decoded next frame in %.1fms (during swap/VSync)\n", predecode_time * 1000);
                }
            } else if (video_is_eof(app->video)) {
                // EOF during pre-decode - will be handled in main decode section next iteration
                next_frame_ready = false;
            } else {
                next_frame_ready = false;
            }
        }
        
        clock_gettime(CLOCK_MONOTONIC, &render_end);
        
        // Calculate detailed timing for debugging
        double gl_time = (gl_render_end.tv_sec - gl_render_start.tv_sec) + 
                        (gl_render_end.tv_nsec - gl_render_start.tv_nsec) / 1e9;
        double overlay_time = (overlay_end.tv_sec - overlay_start.tv_sec) + 
                             (overlay_end.tv_nsec - overlay_start.tv_nsec) / 1e9;
        double swap_time = (swap_end.tv_sec - swap_start.tv_sec) + 
                          (swap_end.tv_nsec - swap_start.tv_nsec) / 1e9;
                          
        // Check if we should do a time-based timing report (every 2 seconds)
        if (app->show_timing) {
            struct timespec current_report_time;
            clock_gettime(CLOCK_MONOTONIC, &current_report_time);
            double time_since_report = (current_report_time.tv_sec - last_timing_report.tv_sec) + 
                                      (current_report_time.tv_nsec - last_timing_report.tv_nsec) / 1e9;
                                      
            if (time_since_report >= 2.0) {  // Every 2 seconds
                printf("\n[TIME-BASED] Frames decoded: %d, Diagnostic frames: %d\n",
                       frame_count, diagnostic_frame_count);
                printf("[TIME-BASED] Last decode: %.1fms, Last render: %.1fms, Swap: %.1fms\n",
                       decode_time * 1000, render_time * 1000, swap_time * 1000);
                       
                // Log to file as well
                FILE *timing_log = fopen("timing_log.txt", "a");
                if (timing_log) {
                    fprintf(timing_log, "[%d] Frames: %d, Decode: %.1fms, Render: %.1fms\n",
                           (int)current_report_time.tv_sec, frame_count, 
                           decode_time * 1000, render_time * 1000);
                    fclose(timing_log);
                }
                
                // Reset the timer
                last_timing_report = current_report_time;
                fflush(stdout);
            }
        }
        
        // Report detailed timing for long frames
        if (app->show_timing) {
            static int detailed_timing_count = 0;
            if (swap_time > 0.050 || (detailed_timing_count++ % 60 == 0)) { // Every second or if swap > 50ms
                printf("Detailed timing - GL: %.1fms, Overlays: %.1fms, Swap: %.1fms\n",
                       gl_time * 1000, overlay_time * 1000, swap_time * 1000);
                fflush(stdout);
            }
        }
        render_time = (render_end.tv_sec - render_start.tv_sec) +
                    (render_end.tv_nsec - render_start.tv_nsec) / 1e9;
        
        total_render_time += render_time;

        // Process keystone movement immediately after input update
        process_keystone_movement(app, delta_time, target_frame_time);

        // Implement variable frame rate control based on performance
        double total_frame_time = decode_time + render_time;
        double remaining_time = adaptive_frame_time - total_frame_time;
        
        if (remaining_time > 0.001) { // Only sleep if >1ms remaining
            // OPTIMIZED: Use nanosleep for better precision
            struct timespec sleep_time;
            sleep_time.tv_sec = 0;
            sleep_time.tv_nsec = (long)(remaining_time * 1000000000);
            nanosleep(&sleep_time, NULL);
            
            // Show render diagnostics periodically
            if (app->show_timing && diagnostic_frame_count % 30 == 0 && diagnostic_frame_count > 0) {
                double avg_render_time = total_render_time / diagnostic_frame_count;
                printf("Render timing - Avg render: %.1fms, Total frame: %.1fms, Frames: %d\n",
                       avg_render_time * 1000, total_frame_time * 1000, diagnostic_frame_count);
                fflush(stdout);
            }
        } else {
            // We're running behind - no sleep, but don't make timing worse
            if (app->show_timing) {
                printf("Frame overrun: %.1fms (target: %.1fms)\n", 
                       total_frame_time * 1000, adaptive_frame_time * 1000);
                fflush(stdout);
            }
            
            // FIXED: Only increase timing slightly if consistently overrunning
            static int overrun_count = 0;
            overrun_count++;
            if (overrun_count > 5) { // Only after 5 consecutive overruns
                adaptive_frame_time += 0.001; // Add 1ms, don't multiply
                if (adaptive_frame_time > min_frame_time * 2) {
                    adaptive_frame_time = min_frame_time * 2; // Max 33ms for 30fps
                }
                overrun_count = 0;
            }
        }
    }
}

void app_cleanup(app_context_t *app) {
    if (app->input) {
        input_cleanup(app->input);
        free(app->input);
    }
    if (app->keystone) {
        // Don't auto-save on exit - only save when user presses S key
        keystone_cleanup(app->keystone);
        free(app->keystone);
    }
    if (app->keystone2) {
        keystone_cleanup(app->keystone2);
        free(app->keystone2);
    }
    if (app->video) {
        video_cleanup(app->video);
        free(app->video);
    }
    if (app->video2) {
        video_cleanup(app->video2);
        free(app->video2);
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
