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
    
    // YUV output (no RGB conversion)
    uint8_t *y_data, *u_data, *v_data;
    int y_linesize, u_linesize, v_linesize;
    
    bool initialized;
    bool eof_reached;
    bool is_transcoded_stream;  // Flag to indicate this is a transcoded stream
    bool loop_playback;         // Flag to enable looped playback
    char *temp_file;  // For cleanup of transcoded files
    
    // Streaming transcoder
    struct {
        pthread_t transcoder_thread;
        int pipe_fd[2];
        bool transcoding_active;
        bool should_stop;
    } transcoder;
} video_context_t;

// Video decoder functions
int video_init(video_context_t *video, const char *filename);
void video_cleanup(video_context_t *video);
int video_decode_frame(video_context_t *video);
uint8_t* video_get_rgb_data(video_context_t *video);  // Legacy - for fallback
void video_get_yuv_data(video_context_t *video, uint8_t **y, uint8_t **u, uint8_t **v, 
                       int *y_stride, int *u_stride, int *v_stride);  // New YUV output
double video_get_frame_time(video_context_t *video);
bool video_is_eof(video_context_t *video);
void video_seek(video_context_t *video, int64_t timestamp);
void video_get_dimensions(video_context_t *video, int *width, int *height);
bool video_is_hardware_decoded(video_context_t *video);
int video_transcode_for_hardware(const char *input_file, const char *output_file);
int video_init_streaming_transcode(video_context_t *video, const char *filename);
int video_restart_playback(video_context_t *video);
void video_set_loop(video_context_t *video, bool loop);

#endif // VIDEO_DECODER_H