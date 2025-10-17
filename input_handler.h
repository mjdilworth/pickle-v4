#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <linux/input.h>
#include <linux/joystick.h>
#include <termios.h>

typedef struct {
    int keyboard_fd;
    bool keys_pressed[256];
    bool keys_just_pressed[256];  // One-shot keys for terminal mode
    bool should_quit;
    bool toggle_corners;
    bool toggle_border;
    bool toggle_help;
    bool save_keystone;
    
    // Terminal input fallback
    bool use_stdin_fallback;
    struct termios orig_termios;
    int stdin_fd;
    
    // Gamepad support
    int gamepad_fd;
    bool gamepad_enabled;
    bool gamepad_buttons[32];           // Button states (up to 32 buttons)
    bool gamepad_buttons_just_pressed[32]; // One-shot button presses
    int16_t gamepad_axis_x;             // Left stick X axis (-32768 to 32767)
    int16_t gamepad_axis_y;             // Left stick Y axis
    int16_t gamepad_dpad_x;             // D-pad X (-1, 0, 1)
    int16_t gamepad_dpad_y;             // D-pad Y (-1, 0, 1)
    
    // Gamepad action flags
    bool gamepad_cycle_corner;          // X button: cycle through corners
    bool gamepad_decrease_step;         // L1: decrease step size
    bool gamepad_increase_step;         // R1: increase step size
    bool gamepad_reset_keystone;        // SELECT: reset keystone
    bool gamepad_toggle_mode;           // START: toggle keystone mode
    uint64_t gamepad_start_select_time; // For START+SELECT hold detection
    bool debug_gamepad;                 // Debug flag for logging button presses
} input_context_t;

// Input handling functions
int input_init(input_context_t *input);
void input_cleanup(input_context_t *input);
void input_update(input_context_t *input);
bool input_is_key_pressed(input_context_t *input, int key);
bool input_is_key_just_pressed(input_context_t *input, int key);
bool input_should_quit(input_context_t *input);
void input_clear_keys(input_context_t *input);
void input_restore_terminal_global(void);  // Emergency terminal restoration

// Key codes for our application
#define KEY_QUIT         KEY_Q
#define KEY_CORNER_1     KEY_1
#define KEY_CORNER_2     KEY_2
#define KEY_CORNER_3     KEY_3
#define KEY_CORNER_4     KEY_4

// Gamepad button mappings for 8BitDo Zero 2 (using joystick API)
// These match the js_event.number values from /dev/input/js0
// Note: 8BitDo Zero 2 uses different button numbering than expected
#define JS_BUTTON_B      0  // BOTTOM button
#define JS_BUTTON_A      1  // RIGHT button  
#define JS_BUTTON_Y      2  // LEFT button
#define JS_BUTTON_X      3  // TOP button
#define JS_BUTTON_L1     4
#define JS_BUTTON_R1     5
#define JS_BUTTON_SELECT 6
#define JS_BUTTON_START  7
#define JS_BUTTON_HOME   8

// Joystick axis indices
#define JS_AXIS_LEFT_X   0
#define JS_AXIS_LEFT_Y   1
#define JS_AXIS_DPAD_X   6
#define JS_AXIS_DPAD_Y   7

#endif // INPUT_HANDLER_H