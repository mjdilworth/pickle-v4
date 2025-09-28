#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <linux/input.h>
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

#endif // INPUT_HANDLER_H