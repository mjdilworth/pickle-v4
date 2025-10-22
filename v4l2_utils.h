#ifndef V4L2_UTILS_H
#define V4L2_UTILS_H

#include <stdint.h>
#include <stddef.h>

// Convert avcC extradata to Annex-B format (SPS/PPS with 0x00000001 prefixes)
// in: avcc pointer/length  
// out: *out_buf / *out_len allocated with malloc (caller must free)
// returns: 0 on success, -1 on error
int avcc_extradata_to_annexb(const uint8_t *avcc, size_t avcc_len,
                             uint8_t **out_buf, size_t *out_len);

// Convert NAL-length prefixed samples to Annex-B format in-place
// sample: pointer to buffer containing encoded sample with NAL lengths
// sample_len: length of the buffer
// length_size: number of bytes used for NAL length (1..4, typically 4)
// returns: new length after conversion, or -1 on error
int convert_sample_avcc_to_annexb_inplace(uint8_t *sample, size_t sample_len, int length_size);

// Extract length size from avcC extradata
// avcc: pointer to avcC extradata
// avcc_len: length of extradata
// returns: length size (1-4), or -1 on error
int get_avcc_length_size(const uint8_t *avcc, size_t avcc_len);

// Check and print V4L2 hardware decoder capabilities
// Returns 1 if h264 decoding is supported, 0 if not, -1 on error
int check_v4l2_decoder_capabilities(void);

#endif // V4L2_UTILS_H