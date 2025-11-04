#define _POSIX_C_SOURCE 200809L
#include "wifi_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/wait.h>

int wifi_manager_init(wifi_manager_t *mgr) {
    memset(mgr, 0, sizeof(*mgr));
    mgr->selected_index = 0;
    mgr->keyboard_cursor = 0;
    mgr->state = WIFI_STATE_IDLE;
    mgr->status[0] = '\0';
    mgr->last_exit_code = 0;
    mgr->show_password = true; // Default to visible per request; can be toggled from UI
    
    // Initialize on-screen keyboard layout - QWERTY style with special keys
    // Row3 includes '<' (Backspace), '_' (Space), '>' (Done)
    const char *rows[] = {
        "1234567890",
        "qwertyuiop",
        "asdfghjkl",
        "<zxcvbnm_>!"  // '!' = toggle show/hide
    };
    for (int i = 0; i < 4; i++) {
        memset(mgr->keyboard_layout[i], 0, sizeof(mgr->keyboard_layout[i]));
        strncpy(mgr->keyboard_layout[i], rows[i], 12);
    }
    
    return 0;
}

void wifi_manager_scan(wifi_manager_t *mgr) {
    if (!mgr) {
        printf("[WIFI] Error: WiFi manager is NULL\n");
        return;
    }
    
    mgr->state = WIFI_STATE_SCANNING;
    mgr->network_count = 0;
    printf("[WIFI] Scanning for networks...\n");
    fflush(stdout);
    
    // Use nmcli directly without pipes to avoid shell issues
    FILE *fp = popen("nmcli -t -f SSID,SIGNAL dev wifi list 2>/dev/null", "r");
    if (!fp) {
        printf("[WIFI] Failed to open nmcli command\n");
        fflush(stdout);
        // Provide some dummy networks for testing
        mgr->networks[0].signal_strength = 80;
        strncpy(mgr->networks[0].ssid, "Test-Network-1", MAX_SSID_LENGTH);
        mgr->networks[1].signal_strength = 60;
        strncpy(mgr->networks[1].ssid, "Test-Network-2", MAX_SSID_LENGTH);
        mgr->network_count = 2;
        mgr->state = WIFI_STATE_NETWORK_LIST;
        return;
    }
    
    char line[512];
    int line_count = 0;
    while (fgets(line, sizeof(line), fp) != NULL && mgr->network_count < MAX_NETWORKS) {
        line_count++;
        if (line_count > 20) break;  // Limit to 20 networks
        
        // Remove newline
        line[strcspn(line, "\n")] = 0;
        
        // Skip empty lines
        if (strlen(line) == 0) continue;
        
        char ssid[MAX_SSID_LENGTH + 1] = {0};
        int signal = 0;
        
        // Try to parse: SSID:SIGNAL format
        // Find the last colon (signal strength is after it)
        char *last_colon = strrchr(line, ':');
        if (last_colon) {
            // Extract signal strength
            signal = atoi(last_colon + 1);
            
            // Extract SSID (everything before the last colon)
            int ssid_len = last_colon - line;
            if (ssid_len > 0 && ssid_len <= MAX_SSID_LENGTH) {
                strncpy(ssid, line, ssid_len);
                ssid[ssid_len] = '\0';
                
                if (strlen(ssid) > 0 && signal > 0) {
                    strncpy(mgr->networks[mgr->network_count].ssid, ssid, MAX_SSID_LENGTH);
                    mgr->networks[mgr->network_count].signal_strength = signal;
                    mgr->networks[mgr->network_count].is_open = true;
                    printf("[WIFI] Found: %s (%d%%)\n", ssid, signal);
                    fflush(stdout);
                    mgr->network_count++;
                }
            }
        }
    }
    
    pclose(fp);
    
    mgr->state = WIFI_STATE_NETWORK_LIST;
    mgr->selected_index = 0;
    printf("[WIFI] Scan complete. Found %d networks\n", mgr->network_count);
    fflush(stdout);
}

void wifi_manager_select_network(wifi_manager_t *mgr, int index) {
    if (!mgr) {
        printf("[WIFI] Error: WiFi manager is NULL\n");
        return;
    }
    if (index >= 0 && index < mgr->network_count) {
        mgr->selected_index = index;
        mgr->state = WIFI_STATE_PASSWORD_ENTRY;
        mgr->password_length = 0;
        mgr->keyboard_cursor = 0;
        memset(mgr->password, 0, sizeof(mgr->password));
        printf("[WIFI] Selected: %s\n", mgr->networks[index].ssid);
    }
}

void wifi_manager_add_password_char(wifi_manager_t *mgr, char c) {
    if (!mgr) return;
    if (mgr->password_length < MAX_PASSWORD_LENGTH) {
        mgr->password[mgr->password_length++] = c;
        mgr->password[mgr->password_length] = '\0';
           printf("[WIFI] Password length: %d\n", mgr->password_length);
    }
}

void wifi_manager_remove_password_char(wifi_manager_t *mgr) {
    if (!mgr) return;
    if (mgr->password_length > 0) {
        mgr->password_length--;
        mgr->password[mgr->password_length] = '\0';
           printf("[WIFI] Password length: %d\n", mgr->password_length);
    }
}

void wifi_manager_move_cursor(wifi_manager_t *mgr, int dx, int dy) {
    if (!mgr) return;
    // Cursor movement in keyboard grid using actual row lengths
    int current_row = mgr->keyboard_cursor / 12; // maximum row width bound
    int current_col = mgr->keyboard_cursor % 12;
    if (current_row < 0) current_row = 0;
    if (current_row > 3) current_row = 3;
    int row_len = (int)strlen(mgr->keyboard_layout[current_row]);
    if (current_col >= row_len) current_col = row_len > 0 ? row_len - 1 : 0;
    
    current_col += dx;
    current_row += dy;
    
    // Bounds checking
    if (current_row < 0) current_row = 0;
    if (current_row > 3) current_row = 3;
    if (current_col < 0) current_col = 0;
    row_len = (int)strlen(mgr->keyboard_layout[current_row]);
    if (current_col >= row_len) current_col = row_len > 0 ? row_len - 1 : 0;
    
    mgr->keyboard_cursor = current_row * 12 + current_col;
}

char wifi_manager_get_key_at(const wifi_manager_t *mgr, int cursor) {
    if (!mgr) return '\0';
    int row = cursor / 12;
    int col = cursor % 12;
    if (row < 0 || row > 3) return '\0';
    int row_len = (int)strlen(mgr->keyboard_layout[row]);
    if (col < 0 || col >= row_len) return '\0';
    return mgr->keyboard_layout[row][col];
}

char wifi_manager_get_cursor_key(const wifi_manager_t *mgr) {
    if (!mgr) return '\0';
    return wifi_manager_get_key_at(mgr, mgr->keyboard_cursor);
}

void wifi_manager_update_config(const char *ssid, const char *password) {
    FILE *fp = fopen(WPA_SUPPLICANT_CONF, "a");
    if (!fp) {
        printf("[WIFI] Failed to open %s\n", WPA_SUPPLICANT_CONF);
        return;
    }
    
    fprintf(fp, "\nnetwork={\n");
    fprintf(fp, "    ssid=\"%s\"\n", ssid);
    fprintf(fp, "    psk=\"%s\"\n", password);
    fprintf(fp, "    priority=10\n");
    fprintf(fp, "}\n");
    fclose(fp);
    
    printf("[WIFI] Updated wpa_supplicant.conf with network: %s\n", ssid);
}

// Execute: nmcli device wifi connect <ssid> password <password> without shell
static int nmcli_connect_noninteractive(const char *ssid, const char *password) {
    pid_t pid = fork();
    if (pid == 0) {
        const char *argv[] = {
            "nmcli", "device", "wifi", "connect", ssid, "password", password, NULL
        };
        execvp("nmcli", (char * const *)argv);
        _exit(127); // Exec failed
    } else if (pid < 0) {
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

int wifi_manager_connect(wifi_manager_t *mgr, const char *ssid, const char *password) {
    if (!mgr) {
        printf("[WIFI] Error: WiFi manager is NULL\n");
        return -1;
    }
    if (!ssid || !password) {
        printf("[WIFI] Error: Invalid SSID or password\n");
        return -1;
    }
    
    mgr->state = WIFI_STATE_CONNECTING;
    printf("[WIFI] Connecting to: %s\n", ssid);
    
    // Try NetworkManager via nmcli (no shell, no sudo)
    int nm_rc = nmcli_connect_noninteractive(ssid, password);
    mgr->last_exit_code = nm_rc;
    if (nm_rc == 0) {
        snprintf(mgr->status, sizeof(mgr->status), "Connecting to %s...", ssid);
        printf("[WIFI] nmcli connection command issued successfully (exit=%d)\n", nm_rc);
        return 0; // success
    }
    // Do NOT attempt to modify system files or restart services without privileges
    snprintf(mgr->status, sizeof(mgr->status), "Connect failed (nmcli exit=%d). Insufficient privileges?", nm_rc);
    printf("[WIFI] nmcli failed (exit=%d). Not attempting privileged fallbacks.\n", nm_rc);
    // Return non-zero to signal failure to caller (keep overlay open)
    return nm_rc != 0 ? nm_rc : -1;
}

void wifi_manager_cleanup(wifi_manager_t *mgr) {
    memset(mgr, 0, sizeof(*mgr));
}
