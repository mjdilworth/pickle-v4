#include "v4l2_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// Declare popen and pclose if not included
#ifndef _POSIX_C_SOURCE
FILE *popen(const char *command, const char *type);
int pclose(FILE *stream);
#endif

// Convert avcC extradata to Annex-B (SPS/PPS with 0x00000001 prefixes)
int avcc_extradata_to_annexb(const uint8_t *avcc, size_t avcc_len,
                             uint8_t **out_buf, size_t *out_len)
{
    if (!avcc || avcc_len < 7 || avcc[0] != 1) {
        printf("V4L2 Utils: Invalid avcC extradata (len=%zu, version=%d)\n", avcc_len, avcc ? avcc[0] : -1);
        return -1; // Not avcC format
    }
    
    const uint8_t *p = avcc;
    size_t pos = 0;

    // Skip version (1 byte), profile (1), profile_compat (1), level (1)
    pos = 4;
    
    // Read lengthSizeMinusOne (byte 4, lower 2 bits)
    uint8_t lengthSizeMinusOne = p[4] & 0x03;
    printf("V4L2 Utils: avcC length size: %d bytes\n", lengthSizeMinusOne + 1);
    
    // Read number of SPS (byte 5, lower 5 bits)
    uint8_t numSPS = p[5] & 0x1f;
    pos = 6;
    
    size_t total_size = 0;
    uint8_t *buf = NULL;

    printf("V4L2 Utils: Processing %d SPS units\n", numSPS);
    
    // Process SPS units
    for (uint8_t i = 0; i < numSPS; ++i) {
        if (pos + 2 > avcc_len) {
            printf("V4L2 Utils: SPS length out of bounds\n");
            free(buf);
            return -1;
        }
        
        uint16_t sps_len = (p[pos] << 8) | p[pos+1];
        pos += 2;
        
        if (pos + sps_len > avcc_len) {
            printf("V4L2 Utils: SPS data out of bounds\n");
            free(buf);
            return -1;
        }
        
        printf("V4L2 Utils: SPS %d length: %d bytes\n", i, sps_len);
        
        // Allocate space for start code + SPS data
        size_t old_size = total_size;
        total_size += 4 + sps_len;
        buf = realloc(buf, total_size);
        if (!buf) {
            printf("V4L2 Utils: Memory allocation failed\n");
            return -1;
        }
        
        // Add Annex-B start code
        memcpy(buf + old_size, "\x00\x00\x00\x01", 4);
        memcpy(buf + old_size + 4, p + pos, sps_len);
        pos += sps_len;
    }
    
    // Read number of PPS 
    if (pos + 1 > avcc_len) {
        printf("V4L2 Utils: PPS count out of bounds\n");
        free(buf);
        return -1;
    }
    uint8_t numPPS = p[pos++];

    printf("V4L2 Utils: Processing %d PPS units\n", numPPS);
    
    // Process PPS units
    for (uint8_t i = 0; i < numPPS; ++i) {
        if (pos + 2 > avcc_len) {
            printf("V4L2 Utils: PPS length out of bounds\n");
            free(buf);
            return -1;
        }
        
        uint16_t pps_len = (p[pos] << 8) | p[pos+1];
        pos += 2;
        
        if (pos + pps_len > avcc_len) {
            printf("V4L2 Utils: PPS data out of bounds\n");
            free(buf);
            return -1;
        }
        
        printf("V4L2 Utils: PPS %d length: %d bytes\n", i, pps_len);
        
        // Allocate space for start code + PPS data
        size_t old_size = total_size;
        total_size += 4 + pps_len;
        buf = realloc(buf, total_size);
        if (!buf) {
            printf("V4L2 Utils: Memory allocation failed\n");
            return -1;
        }
        
        // Add Annex-B start code
        memcpy(buf + old_size, "\x00\x00\x00\x01", 4);
        memcpy(buf + old_size + 4, p + pos, pps_len);
        pos += pps_len;
    }

    *out_buf = buf;
    *out_len = total_size;
    
    printf("V4L2 Utils: Converted avcC to Annex-B: %zu bytes\n", total_size);
    return 0;
}

// Convert NAL-length prefixed samples to Annex-B format in-place
int convert_sample_avcc_to_annexb_inplace(uint8_t *sample, size_t sample_len, int length_size)
{
    if (!sample || length_size < 1 || length_size > 4) {
        return -1;
    }
    
    size_t read_pos = 0;
    size_t write_pos = 0;
    
    while (read_pos + length_size <= sample_len) {
        // Read NAL unit length
        uint32_t nal_size = 0;
        for (int i = 0; i < length_size; ++i) {
            nal_size = (nal_size << 8) | sample[read_pos + i];
        }
        read_pos += length_size;
        
        // Check bounds
        if (read_pos + nal_size > sample_len) {
            printf("V4L2 Utils: NAL unit size %u exceeds buffer bounds\n", nal_size);
            return -1;
        }
        
        // Write Annex-B start code
        sample[write_pos + 0] = 0x00;
        sample[write_pos + 1] = 0x00;
        sample[write_pos + 2] = 0x00;
        sample[write_pos + 3] = 0x01;
        write_pos += 4;
        
        // Move NAL unit data (might overlap, so use memmove)
        if (read_pos != write_pos) {
            memmove(sample + write_pos, sample + read_pos, nal_size);
        }
        
        read_pos += nal_size;
        write_pos += nal_size;
    }
    
    // Clear any remaining bytes
    if (write_pos < sample_len) {
        memset(sample + write_pos, 0, sample_len - write_pos);
    }
    
    return (int)write_pos;
}

// Extract length size from avcC extradata
int get_avcc_length_size(const uint8_t *avcc, size_t avcc_len)
{
    if (!avcc || avcc_len < 5 || avcc[0] != 1) {
        return -1; // Not valid avcC
    }
    
    return (avcc[4] & 0x03) + 1;
}

// Check and print V4L2 hardware decoder capabilities
int check_v4l2_decoder_capabilities(void) 
{
    FILE *fp;
    char path[1024];
    char line[1024];
    int found_decoder = 0;
    
    printf("\n===== V4L2 Hardware Decoder Capabilities =====\n");
    
    // Run v4l2-ctl to get device list
    fp = popen("v4l2-ctl --list-devices 2>/dev/null", "r");
    if (!fp) {
        printf("Failed to run v4l2-ctl. Make sure it's installed.\n");
        return -1;
    }
    
    // Look for mem2mem devices (potentially hardware decoders)
    bool in_mem2mem = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "mem2mem") || strstr(line, "stateless") || strstr(line, "codec")) {
            in_mem2mem = true;
            printf("Potential hardware codec found: %s", line);
        } else if (in_mem2mem && line[0] == '\t') {
            // This is a device path under a mem2mem device
            char *device_path = line;
            while (*device_path == '\t' || *device_path == ' ') {
                device_path++;
            }
            
            // Remove trailing newline
            size_t len = strlen(device_path);
            if (len > 0 && device_path[len-1] == '\n') {
                device_path[len-1] = '\0';
            }
            
            printf("Device path: %s\n", device_path);
            
            // Save path for further inspection
            snprintf(path, sizeof(path), "%s", device_path);
        } else {
            in_mem2mem = false;
        }
    }
    pclose(fp);
    
    // Check capabilities of identified devices
    if (strlen(path) > 0) {
        char cmd[1500];
        
        // Check for codec capabilities
        snprintf(cmd, sizeof(cmd), "v4l2-ctl --device=%s --list-formats 2>/dev/null", path);
        printf("\nChecking codec capabilities for %s:\n", path);
        fp = popen(cmd, "r");
        if (fp) {
            while (fgets(line, sizeof(line), fp)) {
                printf("%s", line);
                if (strstr(line, "H264") || strstr(line, "h264")) {
                    found_decoder = 1;
                }
            }
            pclose(fp);
        }
        
        // Check for m2m capabilities
        snprintf(cmd, sizeof(cmd), "v4l2-ctl --device=%s --all 2>/dev/null | grep -i -e caps -e flags -e codec -e h264 -e hevc", path);
        printf("\nAdditional codec details:\n");
        system(cmd);
    } else {
        printf("No V4L2 hardware decoder devices found\n");
    }
    
    // Additional diagnostics for V4L2 hardware acceleration
    printf("\nFFmpeg hardware acceleration support:\n");
    system("ffmpeg -hide_banner -hwaccels 2>/dev/null | grep -i v4l2 || echo 'No V4L2 hardware acceleration in FFmpeg'");
    
    // Check for available V4L2 codecs in FFmpeg
    printf("\nFFmpeg V4L2 codecs:\n");
    system("ffmpeg -hide_banner -encoders 2>/dev/null | grep -i v4l2");
    system("ffmpeg -hide_banner -decoders 2>/dev/null | grep -i v4l2");
    
    printf("===============================================\n\n");
    return found_decoder;
}