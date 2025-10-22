#include "video_decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Implement wrapper functions with proper error handling

void video_get_yuv_data(video_context_t *video, uint8_t **y, uint8_t **u, uint8_t **v,
                       int *y_stride, int *u_stride, int *v_stride) {
    // Initialize outputs to safe defaults
    if (y) *y = NULL;
    if (u) *u = NULL;
    if (v) *v = NULL;
    if (y_stride) *y_stride = 0;
    if (u_stride) *u_stride = 0;
    if (v_stride) *v_stride = 0;
    
    if (!video || !video->frame || !video->initialized) {
        return;
    }
    
    // Thread safety
    pthread_mutex_lock(&video->decode_mutex);
    
    if (video->cleanup_in_progress) {
        pthread_mutex_unlock(&video->decode_mutex);
        return;
    }
    
    if (y && video->frame->data[0]) *y = video->frame->data[0];
    if (u && video->frame->data[1]) *u = video->frame->data[1];
    if (v && video->frame->data[2]) *v = video->frame->data[2];
    
    if (y_stride) *y_stride = video->frame->linesize[0];
    if (u_stride) *u_stride = video->frame->linesize[1];
    if (v_stride) *v_stride = video->frame->linesize[2];
    
    pthread_mutex_unlock(&video->decode_mutex);
}

void video_get_dimensions(video_context_t *video, int *width, int *height) {
    if (width) *width = 0;
    if (height) *height = 0;
    
    if (!video || !video->initialized) return;
    
    if (width) *width = video->width;
    if (height) *height = video->height;
}

uint8_t* video_get_nv12_data(video_context_t *video) {
    if (!video || !video->frame || !video->initialized) return NULL;
    
    pthread_mutex_lock(&video->decode_mutex);
    
    if (video->cleanup_in_progress) {
        pthread_mutex_unlock(&video->decode_mutex);
        return NULL;
    }
    
    int width = video->width;
    int height = video->height;
    int needed_size = (width * height * 3) / 2;  // Y plane + packed UV plane
    
    // Allocate or reallocate buffer if needed
    if (video->nv12_buffer_size < needed_size) {
        free(video->nv12_buffer);
        video->nv12_buffer = malloc(needed_size);
        if (!video->nv12_buffer) {
            LOG_ERROR("Failed to allocate NV12 buffer");
            video->nv12_buffer_size = 0;
            pthread_mutex_unlock(&video->decode_mutex);
            return NULL;
        }
        video->nv12_buffer_size = needed_size;
    }
    
    uint8_t *y_data = video->frame->data[0];
    uint8_t *u_data = video->frame->data[1];
    uint8_t *v_data = video->frame->data[2];
    int y_stride = video->frame->linesize[0];
    int u_stride = video->frame->linesize[1];
    int v_stride = video->frame->linesize[2];
    
    if (!y_data) return NULL;
    
    // Copy Y plane (full resolution)
    uint8_t *dst = video->nv12_buffer;
    uint8_t *src = y_data;
    for (int row = 0; row < height; row++) {
        memcpy(dst, src, width);
        dst += width;
        src += y_stride;
    }
    
    // Copy interleaved UV plane (half resolution)
    int uv_width = width / 2;
    int uv_height = height / 2;
    
    if (u_data && v_data) {
        for (int row = 0; row < uv_height; row++) {
            uint8_t *u_row = u_data + row * u_stride;
            uint8_t *v_row = v_data + row * v_stride;
            for (int col = 0; col < uv_width; col++) {
                *dst++ = u_row[col];
                *dst++ = v_row[col];
            }
        }
    }
    
    pthread_mutex_unlock(&video->decode_mutex);
    return video->nv12_buffer;
}

int video_get_nv12_stride(video_context_t *video) {
    if (!video || !video->frame) return 0;
    return video->frame->linesize[0];
}

bool video_is_eof(video_context_t *video) {
    return video ? video->eof_reached : true;
}

bool video_is_hardware_decoded(video_context_t *video) {
    return video ? video->use_hardware_decode : false;
}

void video_set_loop(video_context_t *video, bool loop) {
    if (video) {
        video->loop_playback = loop;
    }
}