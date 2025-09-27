#include "keystone.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Matrix math utilities for perspective transformation
static void matrix_identity(float *matrix) {
    memset(matrix, 0, 16 * sizeof(float));
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

// Commented out unused function for future use
/*
static void matrix_multiply(float *result, const float *a, const float *b) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            result[i * 4 + j] = 0;
            for (int k = 0; k < 4; k++) {
                result[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
            }
        }
    }
}
*/

// Calculate perspective transformation matrix from four corner points
static void calculate_perspective_matrix(float *matrix, const point_t corners[4]) {
    // Source points (normalized device coordinates: -1 to 1)
    // TODO: Implement proper perspective matrix calculation using these points
    // float src_x[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    // float src_y[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
    
    // Destination points (keystone corners)
    float dst_x[4] = {corners[0].x, corners[1].x, corners[2].x, corners[3].x};
    float dst_y[4] = {corners[0].y, corners[1].y, corners[2].y, corners[3].y};
    
    // Calculate perspective transformation using homogeneous coordinates
    // This is a simplified version - for production use a proper perspective matrix calculation
    
    // For now, we'll use a simple bilinear transformation
    // In a full implementation, you'd solve for the 8-parameter perspective transform
    
    matrix_identity(matrix);
    
    // Simple scaling and translation for basic keystone effect
    float center_x = (dst_x[0] + dst_x[1] + dst_x[2] + dst_x[3]) * 0.25f;
    float center_y = (dst_y[0] + dst_y[1] + dst_y[2] + dst_y[3]) * 0.25f;
    
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    
    // Apply transformation
    matrix[0] = scale_x;  // Scale X
    matrix[5] = scale_y;  // Scale Y
    matrix[12] = center_x; // Translate X
    matrix[13] = center_y; // Translate Y
    
    // Note: This is a simplified transformation. For proper keystone correction,
    // you would implement a full perspective transformation matrix calculation
    // using techniques like solving the system of linear equations for the
    // 8 parameters of the perspective transformation.
}

int keystone_init(keystone_context_t *keystone) {
    memset(keystone, 0, sizeof(*keystone));
    
    // Initialize default corners (no keystone correction)
    keystone->corners[CORNER_TOP_LEFT]     = (point_t){-1.0f, -1.0f};
    keystone->corners[CORNER_TOP_RIGHT]    = (point_t){ 1.0f, -1.0f};
    keystone->corners[CORNER_BOTTOM_RIGHT] = (point_t){ 1.0f,  1.0f};
    keystone->corners[CORNER_BOTTOM_LEFT]  = (point_t){-1.0f,  1.0f};
    
    keystone->selected_corner = -1;
    keystone->move_step = 0.02f; // 2% movement per key press
    keystone->matrix_dirty = true;
    keystone->show_corners = false; // Corners hidden by default
    keystone->show_border = false;  // Border hidden by default
    keystone->show_help = false;    // Help overlay hidden by default
    
    // Initialize identity matrix
    matrix_identity(keystone->matrix);
    
    // Keystone correction initialization complete
    return 0;
}

void keystone_cleanup(keystone_context_t *keystone) {
    // Nothing to cleanup for this simple implementation
    memset(keystone, 0, sizeof(*keystone));
}

void keystone_select_corner(keystone_context_t *keystone, int corner) {
    if (corner >= 0 && corner < 4) {
        keystone->selected_corner = corner;
        printf("Selected corner %d\n", corner + 1);
    } else {
        keystone->selected_corner = -1;
        printf("Deselected corner\n");
    }
}

void keystone_move_corner(keystone_context_t *keystone, float dx, float dy) {
    if (keystone->selected_corner >= 0 && keystone->selected_corner < 4) {
        point_t *corner = &keystone->corners[keystone->selected_corner];
        
        // Apply movement with bounds checking
        corner->x += dx * keystone->move_step;
        corner->y += dy * keystone->move_step;
        
        // Clamp to reasonable bounds
        if (corner->x < -2.0f) corner->x = -2.0f;
        if (corner->x >  2.0f) corner->x =  2.0f;
        if (corner->y < -2.0f) corner->y = -2.0f;
        if (corner->y >  2.0f) corner->y =  2.0f;
        
        keystone->matrix_dirty = true;
        
        printf("Moved corner %d to (%.3f, %.3f)\n", 
               keystone->selected_corner + 1, corner->x, corner->y);
    }
}

void keystone_calculate_matrix(keystone_context_t *keystone) {
    if (!keystone->matrix_dirty) {
        return;
    }
    
    calculate_perspective_matrix(keystone->matrix, keystone->corners);
    keystone->matrix_dirty = false;
}

const float* keystone_get_matrix(keystone_context_t *keystone) {
    if (keystone->matrix_dirty) {
        keystone_calculate_matrix(keystone);
    }
    return keystone->matrix;
}

void keystone_reset_corners(keystone_context_t *keystone) {
    keystone->corners[CORNER_TOP_LEFT]     = (point_t){-1.0f, -1.0f};
    keystone->corners[CORNER_TOP_RIGHT]    = (point_t){ 1.0f, -1.0f};
    keystone->corners[CORNER_BOTTOM_RIGHT] = (point_t){ 1.0f,  1.0f};
    keystone->corners[CORNER_BOTTOM_LEFT]  = (point_t){-1.0f,  1.0f};
    
    keystone->matrix_dirty = true;
    printf("Keystone corners reset to default\n");
}

void keystone_toggle_corners(keystone_context_t *keystone) {
    keystone->show_corners = !keystone->show_corners;
    printf("Corner highlights %s\n", keystone->show_corners ? "enabled" : "disabled");
}

bool keystone_corners_visible(keystone_context_t *keystone) {
    return keystone->show_corners;
}

void keystone_toggle_border(keystone_context_t *keystone) {
    keystone->show_border = !keystone->show_border;
    printf("Border highlights %s\n", keystone->show_border ? "enabled" : "disabled");
}

bool keystone_border_visible(keystone_context_t *keystone) {
    return keystone->show_border;
}

void keystone_toggle_help(keystone_context_t *keystone) {
    keystone->show_help = !keystone->show_help;
    printf("Help overlay %s\n", keystone->show_help ? "enabled" : "disabled");
}

bool keystone_help_visible(keystone_context_t *keystone) {
    return keystone->show_help;
}