#include "video_player.h"
#include "input_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>

// Global app context for signal handlers
static app_context_t *g_app = NULL;

static void signal_handler(int sig) {
    printf("\nReceived signal %d, cleaning up...\n", sig);
    
    // Always try to restore terminal first
    input_restore_terminal_global();
    
    if (g_app) {
        // Clean up application resources
        app_cleanup(g_app);
    }
    
    // Exit gracefully
    exit(0);
}

static void cleanup_on_exit(void) {
    printf("\nRestoring terminal state...\n");
    input_restore_terminal_global();
    
    if (g_app) {
        app_cleanup(g_app);
        g_app = NULL;
    }
}

static void setup_signal_handlers(void) {
    signal(SIGINT, signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);  // Termination signal
    signal(SIGHUP, signal_handler);   // Hangup (SSH disconnect)
    signal(SIGABRT, signal_handler);  // Abort signal
    signal(SIGSEGV, signal_handler);  // Segmentation fault
    
    // Register atexit handler as backup
    atexit(cleanup_on_exit);
}

int main(int argc, char *argv[]) {
    printf("Starting pickle video player...\n");
    fflush(stdout);
    
    bool loop_playback = false;
    char *video_file = NULL;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            loop_playback = true;
        } else if (argv[i][0] != '-') {
            video_file = argv[i];
        }
    }
    
    if (!video_file) {
        fprintf(stderr, "Usage: %s [-l] <video_file.mp4>\n", argv[0]);
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "  -l    - loop video playback\n");
        fprintf(stderr, "Controls:\n");
        fprintf(stderr, "  q/ESC - quit\n");
        fprintf(stderr, "  h     - toggle help overlay\n");
        fprintf(stderr, "  1-4   - select keystone corners\n");
        fprintf(stderr, "  arrows/wasd - move selected corner\n");
        fprintf(stderr, "  r     - reset keystone\n");
        fprintf(stderr, "  p     - save keystone settings\n");
        fprintf(stderr, "  c     - toggle corner highlights\n");
        fprintf(stderr, "  b     - toggle border highlights\n");
        return 1;
    }

    printf("Setting up application context...\n");
    fflush(stdout);
    
    app_context_t app = {0};
    g_app = &app;  // Set global reference for signal handlers
    
    printf("Setting up signal handlers...\n");
    fflush(stdout);
    
    // Set up signal handlers for clean exit
    setup_signal_handlers();
    
    // Initialize and run the video player
    if (app_init(&app, video_file, loop_playback) != 0) {
        fprintf(stderr, "Failed to initialize application\n");
        g_app = NULL;  // Clear global reference
        return 1;
    }
    
    printf("Starting main application loop...\n");
    fflush(stdout);
    
    app_run(&app);
    app_cleanup(&app);
    
    g_app = NULL;  // Clear global reference
    
    return 0;
}