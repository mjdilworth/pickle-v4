#ifndef VIDEO_DECODER_H
#define VIDEO_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
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
    AVCodec *codec;
    AVCodecContext *codec_ctx;
    AVPacket *packet;
    AVFrame *frame;
    int video_stream_index;
    int width;
    int height;
    double fps;
    int64_t duration;
    
    bool initialized;
    bool use_hardware_decode;
    hw_decode_type_t hw_decode_type;
    int avcc_length_size; // For V4L2 M2M hardware decoder
    bool eof_reached;
    bool loop_playback;
    bool using_drm_prime;
    bool advanced_diagnostics; // Flag for detailed diagnostics output
    
    // 2-stage Bitstream filter chain for V4L2 M2M: avcC→Annex-B + AUD insertion
    AVBSFContext *bsf_annexb_ctx;    // Stage 1: h264_mp4toannexb (avcC to Annex-B conversion)
    AVBSFContext *bsf_aud_ctx;       // Stage 2: h264_metadata (AUD insertion)
} video_context_t;

// Video decoder functions
int video_init(video_context_t *video, const char *filename, bool advanced_diagnostics);
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