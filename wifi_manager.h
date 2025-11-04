#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

#define MAX_SSID_LENGTH 32
#define MAX_NETWORKS 20
#define MAX_PASSWORD_LENGTH 63
#define WPA_SUPPLICANT_CONF "/etc/wpa_supplicant/wpa_supplicant.conf"

typedef struct {
    char ssid[MAX_SSID_LENGTH + 1];
    int signal_strength;  // 0-100 percentage
    bool is_open;  // true if no security
} wifi_network_t;

typedef enum {
    WIFI_STATE_IDLE,
    WIFI_STATE_SCANNING,
    WIFI_STATE_NETWORK_LIST,
    WIFI_STATE_PASSWORD_ENTRY,
    WIFI_STATE_CONNECTING
} wifi_state_t;

typedef struct {
    wifi_network_t networks[MAX_NETWORKS];
    int network_count;
    int selected_index;
    char password[MAX_PASSWORD_LENGTH + 1];
    int password_length;
    wifi_state_t state;
    int keyboard_cursor;  // Position in virtual keyboard
    char keyboard_layout[4][13];  // 4 rows of keyboard (strings, null-terminated)
    // Status / error reporting
    char status[160];
    int last_exit_code;
    bool show_password;  // Toggle to show/hide password characters
} wifi_manager_t;

// WiFi functions
int wifi_manager_init(wifi_manager_t *mgr);
void wifi_manager_scan(wifi_manager_t *mgr);
void wifi_manager_select_network(wifi_manager_t *mgr, int index);
void wifi_manager_add_password_char(wifi_manager_t *mgr, char c);
void wifi_manager_remove_password_char(wifi_manager_t *mgr);
void wifi_manager_move_cursor(wifi_manager_t *mgr, int dx, int dy);
char wifi_manager_get_key_at(const wifi_manager_t *mgr, int cursor);
char wifi_manager_get_cursor_key(const wifi_manager_t *mgr);
int wifi_manager_connect(wifi_manager_t *mgr, const char *ssid, const char *password);
void wifi_manager_update_config(const char *ssid, const char *password);
void wifi_manager_cleanup(wifi_manager_t *mgr);

#endif // WIFI_MANAGER_H
