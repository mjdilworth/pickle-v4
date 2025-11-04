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
static volatile sig_atomic_t g_signal_received = 0;

static void signal_handler(int sig) {
    // CRITICAL: Only async-signal-safe operations here!
    // Do NOT call printf, malloc, free, pthread functions, etc.
    g_signal_received = sig;
    
    // Re-raise to get default behavior (but allow atexit to run)
    signal(sig, SIG_DFL);
}

static void cleanup_on_exit(void) {
    printf("\nRestoring terminal state...\n");
    fflush(stdout);
    
    input_restore_terminal_global();
    
    if (g_app) {
        app_cleanup(g_app);
        g_app = NULL;
    }
    
    if (g_signal_received != 0) {
        printf("Exiting after signal %d\n", g_signal_received);
        fflush(stdout);
    }
}

static void setup_signal_handlers(void) {
    // Register signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);  // Termination signal
    signal(SIGHUP, signal_handler);   // Hangup (SSH disconnect)
    signal(SIGABRT, signal_handler);  // Abort signal
    signal(SIGSEGV, signal_handler);  // Segmentation fault
    
    // Register atexit handler for safe cleanup
    atexit(cleanup_on_exit);
}

int main(int argc, char *argv[]) {
    printf("Starting pickle video player...\n");
    fflush(stdout);
    
    bool loop_playback = false;
    bool show_timing = false;
    bool debug_gamepad = false;
    bool advanced_diagnostics = false;
    char *video_file = NULL;
    char *video_file2 = NULL;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            loop_playback = true;
        } else if (strcmp(argv[i], "--timing") == 0) {
            show_timing = true;
        } else if (strcmp(argv[i], "--debug-gamepad") == 0) {
            debug_gamepad = true;
        } else if (strcmp(argv[i], "--hw-debug") == 0) {
            advanced_diagnostics = true;
            printf("Advanced hardware decoder diagnostics enabled\n");
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [options] <video_file1.mp4> [video_file2.mp4]\n", argv[0]);
            fprintf(stderr, "\nOptions:\n");
            fprintf(stderr, "  -l               Loop video playback\n");
            fprintf(stderr, "  --timing         Show frame timing information\n");
            fprintf(stderr, "  --debug-gamepad  Log gamepad button presses\n");
            fprintf(stderr, "  --hw-debug       Enable detailed hardware decoder diagnostics\n");
            fprintf(stderr, "  -h, --help       Show this help message\n");
            fprintf(stderr, "\nKeyboard Controls:\n");
            fprintf(stderr, "  q/ESC    Quit\n");
            fprintf(stderr, "  h        Toggle help overlay\n");
            fprintf(stderr, "  1-4      Select keystone corners (video 1)\n");
            fprintf(stderr, "  5-8      Select keystone corners (video 2)\n");
            fprintf(stderr, "  arrows   Move selected corner\n");
            fprintf(stderr, "  r        Reset keystone\n");
            fprintf(stderr, "  s        Save keystone settings\n");
            fprintf(stderr, "  c        Toggle corner markers\n");
            fprintf(stderr, "  b        Toggle border outline\n");
            fprintf(stderr, "\nGamepad Controls (8BitDo Zero 2):\n");
            fprintf(stderr, "  X        Cycle through corners\n");
            fprintf(stderr, "  D-pad/Stick  Move selected corner\n");
            fprintf(stderr, "  L1/R1    Decrease/Increase step size\n");
            fprintf(stderr, "  SELECT   Reset keystone\n");
            fprintf(stderr, "  START    Toggle corner markers\n");
            fprintf(stderr, "  B        Toggle help overlay\n");
            fprintf(stderr, "  HOME     Toggle border outline\n");
            fprintf(stderr, "  START+SELECT (2s)  Quit\n");
            return 0;
        } else if (argv[i][0] != '-') {
            if (!video_file) {
                video_file = argv[i];
            } else if (!video_file2) {
                video_file2 = argv[i];
            } else {
                fprintf(stderr, "Too many video files specified\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Use -h or --help for usage information\n");
            return 1;
        }
    }
    
    if (!video_file) {
        fprintf(stderr, "Error: No video file specified\n");
        fprintf(stderr, "Usage: %s [options] <video_file1.mp4> [video_file2.mp4]\n", argv[0]);
        fprintf(stderr, "Use -h or --help for more information\n");
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
    if (app_init(&app, video_file, video_file2, loop_playback, show_timing, debug_gamepad, advanced_diagnostics) != 0) {
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