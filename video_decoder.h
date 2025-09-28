#ifndef VIDEO_DECODER_H
#define VIDEO_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>

// Hardware decoding types
typedef enum {
    HW_DECODE_NONE = 0,
    HW_DECODE_V4L2M2M,
    HW_DECODE_MMAL,
    HW_DECODE_DRM_PRIME
} hw_decode_type_t;

typedef struct {
    AVFormatContext *format_ctx;
    AVCodecContext *codec_ctx;
    const AVCodec *codec;
    AVFrame *frame;
    AVFrame *hw_frame;        // For hardware decoded frames
    AVPacket *packet;
    struct SwsContext *sws_ctx; // Fallback only
    
    int video_stream_index;
    uint32_t width;
    uint32_t height;
    double fps;
    int64_t duration;
    
    // Hardware decoding support
    bool use_hardware_decode;
    hw_decode_type_t hw_decode_type;
    enum AVPixelFormat hw_pix_fmt;
    AVBufferRef *hw_device_ctx;
    
    // Frame buffering for smooth playback
    #define MAX_BUFFERED_FRAMES 4
    AVFrame *frame_buffer[MAX_BUFFERED_FRAMES];
    int buffer_write_index;
    int buffer_read_index;
    int buffered_frame_count;
    pthread_mutex_t buffer_mutex;
    
    // YUV output (no RGB conversion)
    uint8_t *y_data, *u_data, *v_data;
    int y_linesize, u_linesize, v_linesize;
    
    bool initialized;
    bool eof_reached;
    bool loop_playback;         // Flag to enable looped playback
} video_context_t;

// Video decoder functions
int video_init(video_context_t *video, const char *filename);
void video_cleanup(video_context_t *video);
int video_decode_frame(video_context_t *video);
uint8_t* video_get_rgb_data(video_context_t *video);  // Legacy - for fallback
uint8_t* video_get_y_data(video_context_t *video);
uint8_t* video_get_u_data(video_context_t *video);
uint8_t* video_get_v_data(video_context_t *video);
int video_get_y_stride(video_context_t *video);
int video_get_u_stride(video_context_t *video);
int video_get_v_stride(video_context_t *video);
void video_get_yuv_data(video_context_t *video, uint8_t **y, uint8_t **u, uint8_t **v, 
                       int *y_stride, int *u_stride, int *v_stride);  // New YUV output
double video_get_frame_time(video_context_t *video);
bool video_is_eof(video_context_t *video);
void video_seek(video_context_t *video, int64_t timestamp);
void video_get_dimensions(video_context_t *video, int *width, int *height);
bool video_is_hardware_decoded(video_context_t *video);
int video_restart_playback(video_context_t *video);
void video_set_loop(video_context_t *video, bool loop);

#endif // VIDEO_DECODER_H