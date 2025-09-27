#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

int main() {
    printf("=== Simple DRM Test ===\n");
    
    // Try opening DRM devices
    const char *devices[] = {"/dev/dri/card0", "/dev/dri/card1", "/dev/dri/renderD128"};
    
    for (int i = 0; i < 3; i++) {
        printf("\nTrying %s:\n", devices[i]);
        
        int fd = open(devices[i], O_RDWR);
        if (fd < 0) {
            perror("  Open failed");
            continue;
        }
        printf("  ✓ Opened successfully (fd=%d)\n", fd);
        
        // Try to become DRM master
        if (drmSetMaster(fd) == 0) {
            printf("  ✓ Became DRM master\n");
            
            // Try to get resources
            drmModeRes *res = drmModeGetResources(fd);
            if (res) {
                printf("  ✓ Got DRM resources\n");
                printf("    Connectors: %d\n", res->count_connectors);
                printf("    Encoders: %d\n", res->count_encoders);
                printf("    CRTCs: %d\n", res->count_crtcs);
                
                // Check connectors
                for (int j = 0; j < res->count_connectors; j++) {
                    drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[j]);
                    if (conn) {
                        printf("    Connector %d: %s (%s)\n", j, 
                               conn->connection == DRM_MODE_CONNECTED ? "CONNECTED" : "DISCONNECTED",
                               conn->connector_type == DRM_MODE_CONNECTOR_HDMIA ? "HDMI" : "OTHER");
                        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
                            printf("      Mode: %dx%d@%d\n", 
                                   conn->modes[0].hdisplay, 
                                   conn->modes[0].vdisplay,
                                   conn->modes[0].vrefresh);
                        }
                        drmModeFreeConnector(conn);
                    }
                }
                
                drmModeFreeResources(res);
            } else {
                printf("  ✗ Failed to get DRM resources\n");
            }
            
            drmDropMaster(fd);
        } else {
            printf("  ✗ Failed to become DRM master\n");
        }
        
        close(fd);
    }
    
    return 0;
}