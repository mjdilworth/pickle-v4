#include "v4l2_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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