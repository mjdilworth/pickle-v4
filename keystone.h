#ifndef KEYSTONE_H
#define KEYSTONE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float x, y;
} point_t;

// 8x8 mesh grid for advanced warp (64 control points)
typedef struct {
    point_t grid[8][8];  // 8x8 mesh control points
    bool mesh_dirty;     // Need to recalculate from mesh
    bool mesh_enabled;   // Toggle mesh warp mode
} mesh_warp_t;

typedef struct {
    point_t corners[4];  // Top-left, top-right, bottom-right, bottom-left
    int selected_corner; // 0-3, -1 if none selected
    float matrix[16];    // 4x4 transformation matrix
    bool matrix_dirty;   // Need to recalculate matrix
    
    // Corner highlighting
    bool show_corners;   // Toggle corner visibility
    bool corners_dirty;  // Corner positions changed, need VBO update
    
    // Border highlighting
    bool show_border;    // Toggle border visibility
    
    // Help overlay
    bool show_help;      // Toggle help overlay visibility
    
    // Movement sensitivity
    float move_step;
    
    // Mesh warp feature
    mesh_warp_t mesh;    // 8x8 mesh warp grid
    int mesh_selected_x; // Selected mesh point X coordinate (0-7), -1 if none
    int mesh_selected_y; // Selected mesh point Y coordinate (0-7), -1 if none
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
void keystone_increase_step_size(keystone_context_t *keystone);
void keystone_decrease_step_size(keystone_context_t *keystone);
float keystone_get_step_size(keystone_context_t *keystone);
int keystone_save_to_file(keystone_context_t *keystone, const char *filename);
int keystone_load_from_file(keystone_context_t *keystone, const char *filename);
int keystone_save_settings(keystone_context_t *keystone);
int keystone_load_settings(keystone_context_t *keystone);

// Mesh warp functions
void keystone_toggle_mesh_warp(keystone_context_t *keystone);
bool keystone_mesh_warp_enabled(keystone_context_t *keystone);
void keystone_reset_mesh(keystone_context_t *keystone);
void keystone_mesh_select_point(keystone_context_t *keystone, int x, int y);
void keystone_mesh_move_point(keystone_context_t *keystone, float dx, float dy);

// Corner indices
#define CORNER_TOP_LEFT     0
#define CORNER_TOP_RIGHT    1
#define CORNER_BOTTOM_RIGHT 2
#define CORNER_BOTTOM_LEFT  3

#endif // KEYSTONE_H