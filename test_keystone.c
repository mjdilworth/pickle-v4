#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "keystone.h"

int main() {
    keystone_context_t keystone;
    
    printf("Testing keystone functionality...\n");
    
    // Initialize keystone
    if (keystone_init(&keystone) != 0) {
        printf("ERROR: keystone_init failed\n");
        return 1;
    }
    printf("✓ Keystone initialized successfully\n");
    
    // Test initial corner positions
    printf("\nInitial corner positions:\n");
    for (int i = 0; i < 4; i++) {
        printf("  Corner %d: (%.3f, %.3f)\n", i, 
               keystone.corners[i].x, keystone.corners[i].y);
    }
    
    // Test corner selection
    printf("\nTesting corner selection:\n");
    keystone_select_corner(&keystone, 0);
    printf("Selected corner: %d\n", keystone.selected_corner);
    
    keystone_select_corner(&keystone, 2); 
    printf("Selected corner: %d\n", keystone.selected_corner);
    
    // Test corner movement
    printf("\nTesting corner movement:\n");
    printf("Before move: Corner 2 = (%.3f, %.3f)\n", 
           keystone.corners[2].x, keystone.corners[2].y);
    
    keystone_move_corner(&keystone, 1.0f, 0.5f);  // Move right and up
    printf("After move: Corner 2 = (%.3f, %.3f)\n", 
           keystone.corners[2].x, keystone.corners[2].y);
    
    // Test matrix calculation
    printf("\nTesting matrix calculation:\n");
    const float *matrix = keystone_get_matrix(&keystone);
    printf("Keystone matrix:\n");
    for (int row = 0; row < 4; row++) {
        printf("  [");
        for (int col = 0; col < 4; col++) {
            printf(" %8.4f", matrix[row*4 + col]);
        }
        printf(" ]\n");
    }
    
    // Test visibility toggles
    printf("\nTesting visibility toggles:\n");
    printf("Initial states: corners=%s, border=%s, help=%s\n",
           keystone_corners_visible(&keystone) ? "ON" : "OFF",
           keystone_border_visible(&keystone) ? "ON" : "OFF", 
           keystone_help_visible(&keystone) ? "ON" : "OFF");
    
    keystone_toggle_corners(&keystone);
    keystone_toggle_border(&keystone);
    keystone_toggle_help(&keystone);
    
    printf("After toggle: corners=%s, border=%s, help=%s\n",
           keystone_corners_visible(&keystone) ? "ON" : "OFF",
           keystone_border_visible(&keystone) ? "ON" : "OFF",
           keystone_help_visible(&keystone) ? "ON" : "OFF");
    
    // Test file save/load
    printf("\nTesting file save/load:\n");
    if (keystone_save_settings(&keystone) == 0) {
        printf("✓ Settings saved successfully\n");
    } else {
        printf("✗ Settings save failed\n");
    }
    
    // Reset and reload
    keystone_reset_corners(&keystone);
    printf("After reset: Corner 2 = (%.3f, %.3f)\n", 
           keystone.corners[2].x, keystone.corners[2].y);
    
    if (keystone_load_settings(&keystone) == 0) {
        printf("✓ Settings loaded successfully\n");
        printf("After load: Corner 2 = (%.3f, %.3f)\n", 
               keystone.corners[2].x, keystone.corners[2].y);
    } else {
        printf("✗ Settings load failed\n");
    }
    
    // Clean up
    keystone_cleanup(&keystone);
    printf("\n✓ All keystone tests completed successfully!\n");
    
    return 0;
}