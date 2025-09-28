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

// Gaussian elimination to solve 8x8 system for perspective transformation
static int solve_linear_system(float A[8][9]) {
    // Forward elimination
    for (int i = 0; i < 8; i++) {
        // Find pivot
        int max_row = i;
        for (int k = i + 1; k < 8; k++) {
            if (fabs(A[k][i]) > fabs(A[max_row][i])) {
                max_row = k;
            }
        }
        
        // Swap rows
        if (max_row != i) {
            for (int j = 0; j < 9; j++) {
                float temp = A[i][j];
                A[i][j] = A[max_row][j];
                A[max_row][j] = temp;
            }
        }
        
        // Check for singular matrix
        if (fabs(A[i][i]) < 1e-10) {
            return -1; // Singular matrix
        }
        
        // Eliminate
        for (int k = i + 1; k < 8; k++) {
            float factor = A[k][i] / A[i][i];
            for (int j = i; j < 9; j++) {
                A[k][j] -= factor * A[i][j];
            }
        }
    }
    
    // Back substitution
    for (int i = 7; i >= 0; i--) {
        for (int j = i + 1; j < 8; j++) {
            A[i][8] -= A[i][j] * A[j][8];
        }
        A[i][8] /= A[i][i];
    }
    
    return 0;
}

// Calculate perspective transformation matrix from four corner points
static void calculate_perspective_matrix(float *matrix, const point_t corners[4]) {
    // CORRECTED: Source points should be the keystone trapezoid
    // Destination points should be the full video quad
    // This makes the video fill the keystone shape, not get warped by it
    
    // Source points (keystone corners - the trapezoid shape)
    float src_x[4] = {corners[0].x, corners[1].x, corners[2].x, corners[3].x};
    float src_y[4] = {corners[0].y, corners[1].y, corners[2].y, corners[3].y};
    
    // Destination points (full video quad: -1 to 1)
    // Corner order: top-left, top-right, bottom-right, bottom-left
    // Flip Y coordinates to match OpenGL coordinate system
    float dst_x[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    float dst_y[4] = {1.0f, 1.0f, -1.0f, -1.0f};
    
    // Check if corners are at default positions (no transformation needed)
    // Default corners: TL(-1,1), TR(1,1), BR(1,-1), BL(-1,-1)
    float default_x[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    float default_y[4] = {1.0f, 1.0f, -1.0f, -1.0f};
    
    bool is_identity = true;
    for (int i = 0; i < 4; i++) {
        if (fabs(src_x[i] - default_x[i]) > 1e-6 || fabs(src_y[i] - default_y[i]) > 1e-6) {
            is_identity = false;
            break;
        }
    }
    
    if (is_identity) {
        matrix_identity(matrix);
        return;
    }
    
    // Set up the system of equations for perspective transformation
    // We need to solve for 8 parameters (a-h) in the transformation:
    // x' = (ax + by + c) / (gx + hy + 1)
    // y' = (dx + ey + f) / (gx + hy + 1)
    //
    // This gives us 8 equations from 4 point correspondences
    float A[8][9]; // Augmented matrix
    
    for (int i = 0; i < 4; i++) {
        float x = src_x[i];
        float y = src_y[i];
        float x_prime = dst_x[i];
        float y_prime = dst_y[i];
        
        // Equation for x': ax + by + c - gx*x' - hy*x' = x'
        A[i*2][0] = x;       // a
        A[i*2][1] = y;       // b
        A[i*2][2] = 1.0f;    // c
        A[i*2][3] = 0.0f;    // d
        A[i*2][4] = 0.0f;    // e
        A[i*2][5] = 0.0f;    // f
        A[i*2][6] = -x * x_prime; // g
        A[i*2][7] = -y * x_prime; // h
        A[i*2][8] = x_prime; // result
        
        // Equation for y': dx + ey + f - gx*y' - hy*y' = y'
        A[i*2+1][0] = 0.0f;  // a
        A[i*2+1][1] = 0.0f;  // b
        A[i*2+1][2] = 0.0f;  // c
        A[i*2+1][3] = x;     // d
        A[i*2+1][4] = y;     // e
        A[i*2+1][5] = 1.0f;  // f
        A[i*2+1][6] = -x * y_prime; // g
        A[i*2+1][7] = -y * y_prime; // h
        A[i*2+1][8] = y_prime; // result
    }
    
    // Solve the system
    if (solve_linear_system(A) != 0) {
        // Fallback to identity matrix if system is singular
        matrix_identity(matrix);
        return;
    }
    
    // Extract parameters
    float a = A[0][8], b = A[1][8], c = A[2][8];
    float d = A[3][8], e = A[4][8], f = A[5][8];
    float g = A[6][8], h = A[7][8];
    
    // Build the 4x4 perspective transformation matrix
    matrix_identity(matrix);
    
    // The perspective transformation matrix for OpenGL (column-major):
    // [ a  d  0  g ]
    // [ b  e  0  h ]
    // [ 0  0  1  0 ]
    // [ c  f  0  1 ]
    
    matrix[0] = a;   matrix[1] = d;   matrix[2] = 0.0f; matrix[3] = g;
    matrix[4] = b;   matrix[5] = e;   matrix[6] = 0.0f; matrix[7] = h;
    matrix[8] = 0.0f; matrix[9] = 0.0f; matrix[10] = 1.0f; matrix[11] = 0.0f;
    matrix[12] = c;   matrix[13] = f;   matrix[14] = 0.0f; matrix[15] = 1.0f;
}

int keystone_init(keystone_context_t *keystone) {
    memset(keystone, 0, sizeof(*keystone));
    
    // Initialize default corners (no keystone correction)
    keystone->corners[CORNER_TOP_LEFT]     = (point_t){-1.0f,  1.0f};  // Corner 4 (visual)
    keystone->corners[CORNER_TOP_RIGHT]    = (point_t){ 1.0f,  1.0f};  // Corner 3 (visual)
    keystone->corners[CORNER_BOTTOM_RIGHT] = (point_t){ 1.0f, -1.0f};  // Corner 2 (visual) 
    keystone->corners[CORNER_BOTTOM_LEFT]  = (point_t){-1.0f, -1.0f};  // Corner 1 (visual)
    
    keystone->selected_corner = -1;
    keystone->move_step = 0.1f; // 10% movement per key press for more noticeable movement
    keystone->matrix_dirty = true;
    keystone->show_corners = true;  // ALWAYS show corners for easier debugging
    keystone->show_border = true;   // ALWAYS show border for easier debugging
    keystone->show_help = true;     // ALWAYS show help for easier debugging
    
    // Initialize identity matrix
    matrix_identity(keystone->matrix);
    
    // Display current corner positions for reference
    printf("\n========== KEYSTONE CORNER POSITIONS ==========\n");
    printf("Corner 1 (BL): (%.2f, %.2f)\n", keystone->corners[CORNER_BOTTOM_LEFT].x, keystone->corners[CORNER_BOTTOM_LEFT].y);
    printf("Corner 2 (BR): (%.2f, %.2f)\n", keystone->corners[CORNER_BOTTOM_RIGHT].x, keystone->corners[CORNER_BOTTOM_RIGHT].y);
    printf("Corner 3 (TR): (%.2f, %.2f)\n", keystone->corners[CORNER_TOP_RIGHT].x, keystone->corners[CORNER_TOP_RIGHT].y);
    printf("Corner 4 (TL): (%.2f, %.2f)\n", keystone->corners[CORNER_TOP_LEFT].x, keystone->corners[CORNER_TOP_LEFT].y);
    printf("===============================================\n\n");
    
    // For debugging: Automatically select corner 1 for testing
    printf("\n========== AUTOMATICALLY SELECTING CORNER 1 FOR TESTING ==========\n");
    keystone->selected_corner = CORNER_BOTTOM_LEFT;
    printf("Selected corner %d for testing\n", keystone->selected_corner + 1);
    
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
        printf("\n=======================\n");
        printf("SELECTED CORNER %d\n", corner + 1);
        printf("=======================\n");
        // Print current position for reference
        printf("Current position: (%.2f, %.2f)\n", 
               keystone->corners[corner].x, keystone->corners[corner].y);
    } else {
        keystone->selected_corner = -1;
        printf("\n=======================\n");
        printf("DESELECTED CORNER\n");
        printf("=======================\n");
    }
}

void keystone_move_corner(keystone_context_t *keystone, float dx, float dy) {
    if (keystone->selected_corner >= 0 && keystone->selected_corner < 4) {
        point_t *corner = &keystone->corners[keystone->selected_corner];
        
        // Print debug info
        printf("\n==== KEYSTONE MOVEMENT ====\n");
        printf("Moving corner %d: Current pos (%.2f,%.2f), delta (%.1f,%.1f)\n", 
               keystone->selected_corner + 1, corner->x, corner->y, dx, dy);
        
        // Apply movement with GREATLY increased step size for better visibility
        float move_step = 0.1f;  // Use a fixed large step size (10% per keypress)
        
        // Apply movement with bounds checking
        corner->x += dx * move_step;
        corner->y += dy * move_step;
        
        // Clamp to reasonable bounds
        if (corner->x < -2.0f) corner->x = -2.0f;
        if (corner->x >  2.0f) corner->x =  2.0f;
        if (corner->y < -2.0f) corner->y = -2.0f;
        if (corner->y >  2.0f) corner->y =  2.0f;
        
        // Print updated position
        printf("  -> New position: (%.2f,%.2f)\n", corner->x, corner->y);
        
        keystone->matrix_dirty = true;
        
        printf("Moved corner %d to (%.3f, %.3f)\n", 
               keystone->selected_corner + 1, corner->x, corner->y);
    } else {
        printf("ERROR: Cannot move corner - no corner selected (%d)\n", keystone->selected_corner);
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

int keystone_save_to_file(keystone_context_t *keystone, const char *filename) {
    if (!keystone || !filename) {
        return -1;
    }
    
    FILE *file = fopen(filename, "w");
    if (!file) {
        printf("Failed to open keystone settings file for writing: %s\n", filename);
        return -1;
    }
    
    // Write header comment
    fprintf(file, "# Pickle Video Player Keystone Settings\n");
    fprintf(file, "# Corner positions: Top-Left, Top-Right, Bottom-Right, Bottom-Left\n");
    fprintf(file, "# Format: corner_index x y\n");
    
    // Write corner positions
    for (int i = 0; i < 4; i++) {
        fprintf(file, "corner %d %.6f %.6f\n", i, 
                keystone->corners[i].x, keystone->corners[i].y);
    }
    
    // Write other settings
    fprintf(file, "move_step %.6f\n", keystone->move_step);
    fprintf(file, "selected_corner %d\n", keystone->selected_corner);
    
    fclose(file);
    printf("Keystone settings saved to: %s\n", filename);
    return 0;
}

int keystone_load_from_file(keystone_context_t *keystone, const char *filename) {
    if (!keystone || !filename) {
        return -1;
    }
    
    FILE *file = fopen(filename, "r");
    if (!file) {
        printf("No keystone settings file found: %s\n", filename);
        return -1;
    }
    
    char line[256];
    int corners_loaded = 0;
    
    while (fgets(line, sizeof(line), file)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        
        // Parse corner positions
        if (strncmp(line, "corner", 6) == 0) {
            int corner_index;
            float x, y;
            if (sscanf(line, "corner %d %f %f", &corner_index, &x, &y) == 3) {
                if (corner_index >= 0 && corner_index < 4) {
                    keystone->corners[corner_index].x = x;
                    keystone->corners[corner_index].y = y;
                    corners_loaded++;
                }
            }
        }
        // Parse move step
        else if (strncmp(line, "move_step", 9) == 0) {
            float step;
            if (sscanf(line, "move_step %f", &step) == 1) {
                keystone->move_step = step;
            }
        }
        // Parse selected corner (but don't restore selection on startup)
        else if (strncmp(line, "selected_corner", 15) == 0) {
            // We'll keep selected_corner at -1 on startup
        }
    }
    
    fclose(file);
    
    if (corners_loaded == 4) {
        // Validate corner positions to prevent collapsed transformations
        bool corners_valid = true;
        float min_distance = 0.1f; // Minimum distance between corners
        
        // Check if any corners are too close to each other
        for (int i = 0; i < 4; i++) {
            for (int j = i + 1; j < 4; j++) {
                float dx = keystone->corners[i].x - keystone->corners[j].x;
                float dy = keystone->corners[i].y - keystone->corners[j].y;
                float distance = sqrtf(dx*dx + dy*dy);
                if (distance < min_distance) {
                    printf("Warning: Corners %d and %d are too close (distance: %.3f)\n", i, j, distance);
                    corners_valid = false;
                }
            }
        }
        
        // Check if corners form a reasonable quadrilateral (not degenerate)
        float area = fabs((keystone->corners[1].x - keystone->corners[0].x) * (keystone->corners[2].y - keystone->corners[0].y) - 
                         (keystone->corners[2].x - keystone->corners[0].x) * (keystone->corners[1].y - keystone->corners[0].y));
        if (area < 0.5f) {
            printf("Warning: Keystone corners form degenerate quadrilateral (area: %.3f)\n", area);
            corners_valid = false;
        }
        
        if (!corners_valid) {
            printf("Resetting to default keystone configuration due to invalid corners\n");
            // Reset to default positions
            keystone->corners[CORNER_TOP_LEFT]     = (point_t){-1.0f, -1.0f};
            keystone->corners[CORNER_TOP_RIGHT]    = (point_t){ 1.0f, -1.0f};
            keystone->corners[CORNER_BOTTOM_RIGHT] = (point_t){ 1.0f,  1.0f};
            keystone->corners[CORNER_BOTTOM_LEFT]  = (point_t){-1.0f,  1.0f};
        }
        
        keystone->matrix_dirty = true; // Recalculate transformation matrix
        printf("Keystone settings loaded from: %s\n", filename);
        printf("Corner positions: TL(%.2f,%.2f) TR(%.2f,%.2f) BR(%.2f,%.2f) BL(%.2f,%.2f)\n",
               keystone->corners[0].x, keystone->corners[0].y,
               keystone->corners[1].x, keystone->corners[1].y,
               keystone->corners[2].x, keystone->corners[2].y,
               keystone->corners[3].x, keystone->corners[3].y);
        return 0;
    } else {
        printf("Invalid keystone settings file: %s (only loaded %d/4 corners)\n", 
               filename, corners_loaded);
        return -1;
    }
}

int keystone_save_settings(keystone_context_t *keystone) {
    // Save to current working directory (same directory as application)
    const char *filename = "pickle_keystone.conf";
    
    return keystone_save_to_file(keystone, filename);
}

int keystone_load_settings(keystone_context_t *keystone) {
    // Load from current working directory (same directory as application)
    const char *filename = "pickle_keystone.conf";
    
    return keystone_load_from_file(keystone, filename);
}