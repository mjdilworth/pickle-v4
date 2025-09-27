#ifndef KEYSTONE_H
#define KEYSTONE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float x, y;
} point_t;

typedef struct {
    point_t corners[4];  // Top-left, top-right, bottom-right, bottom-left
    int selected_corner; // 0-3, -1 if none selected
    float matrix[16];    // 4x4 transformation matrix
    bool matrix_dirty;   // Need to recalculate matrix
    
    // Corner highlighting
    bool show_corners;   // Toggle corner visibility
    
    // Border highlighting
    bool show_border;    // Toggle border visibility
    
    // Help overlay
    bool show_help;      // Toggle help overlay visibility
    
    // Movement sensitivity
    float move_step;
} keystone_context_t;

// Keystone correction functions
int keystone_init(keystone_context_t *keystone);
void keystone_cleanup(keystone_context_t *keystone);
void keystone_select_corner(keystone_context_t *keystone, int corner);
void keystone_move_corner(keystone_context_t *keystone, float dx, float dy);
void keystone_calculate_matrix(keystone_context_t *keystone);
const float* keystone_get_matrix(keystone_context_t *keystone);
void keystone_reset_corners(keystone_context_t *keystone);
void keystone_toggle_corners(keystone_context_t *keystone);
bool keystone_corners_visible(keystone_context_t *keystone);
void keystone_toggle_border(keystone_context_t *keystone);
bool keystone_border_visible(keystone_context_t *keystone);
void keystone_toggle_help(keystone_context_t *keystone);
bool keystone_help_visible(keystone_context_t *keystone);

// Corner indices
#define CORNER_TOP_LEFT     0
#define CORNER_TOP_RIGHT    1
#define CORNER_BOTTOM_RIGHT 2
#define CORNER_BOTTOM_LEFT  3

#endif // KEYSTONE_H