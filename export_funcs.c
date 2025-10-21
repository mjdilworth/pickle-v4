// This file ensures that important functions are not optimized out by LTO
#include "video_decoder.h"

// Create a set of function pointers to all the functions we need to preserve
void (*__video_get_yuv_data_ptr)(video_context_t *, uint8_t **, uint8_t **, uint8_t **, int *, int *, int *) = video_get_yuv_data;
void (*__video_get_dimensions_ptr)(video_context_t *, int *, int *) = video_get_dimensions;
uint8_t* (*__video_get_nv12_data_ptr)(video_context_t *) = video_get_nv12_data;
int (*__video_get_nv12_stride_ptr)(video_context_t *) = video_get_nv12_stride;
bool (*__video_is_eof_ptr)(video_context_t *) = video_is_eof;
bool (*__video_is_hardware_decoded_ptr)(video_context_t *) = video_is_hardware_decoded;
void (*__video_cleanup_ptr)(video_context_t *) = video_cleanup;
void (*__video_set_loop_ptr)(video_context_t *, bool) = video_set_loop;