#include <stdio.h>
#include <unistd.h>
#include "input_handler.h"

int main() {
    printf("Testing input handling - press 1,2,3,4 to test key repeat fix (q to quit)\n");
    
    input_context_t input = {0};
    
    if (input_init(&input) != 0) {
        fprintf(stderr, "Failed to initialize input\n");
        return 1;
    }
    
    printf("Input initialized. Testing key press detection:\n");
    printf("- Press 1,2,3,4 to test corner selection\n");
    printf("- Each key should only trigger once per press\n");
    printf("- Press 'q' to quit\n");
    
    while (!input_should_quit(&input)) {
        input_update(&input);
        
        // Test the "just pressed" functionality
        if (input_is_key_just_pressed(&input, KEY_1)) {
            printf("Corner 1 selected (should only show once per press)\n");
        }
        if (input_is_key_just_pressed(&input, KEY_2)) {
            printf("Corner 2 selected (should only show once per press)\n");
        }
        if (input_is_key_just_pressed(&input, KEY_3)) {
            printf("Corner 3 selected (should only show once per press)\n");
        }
        if (input_is_key_just_pressed(&input, KEY_4)) {
            printf("Corner 4 selected (should only show once per press)\n");
        }
        
        usleep(16666); // ~60 FPS
    }
    
    printf("Quitting test...\n");
    input_cleanup(&input);
    return 0;
}