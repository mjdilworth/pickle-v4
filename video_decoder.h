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
#include "v4l2_utils.h"

// Hardware decoding types
typedef enum {
    HW_DECODE_NONE = 0,
    HW_DECODE_V4L2M2M,
    HW_DECODE_MMAL,
    HW_DECODE_DRM_PRIME
} hw_decode_type_t;

// Error recovery options
typedef enum {
    ERROR_RECOVERY_NONE = 0,    // No recovery, fail on errors
    ERROR_RECOVERY_SKIP,        // Skip problematic packets
    ERROR_RECOVERY_KEYFRAME,    // Skip until next keyframe
    ERROR_RECOVERY_FALLBACK     // Fall back to software decoding
} error_recovery_t;

// Note: BSF chain type is declared in v4l2_utils.h as bsf_chain_t

typedef struct {
    AVFormatContext *format_ctx;
    AVCodecContext *codec_ctx;
    const AVCodec *codec;
    AVFrame *frame;
    AVFrame *hw_frame;        // For hardware decoded frames
    AVPacket *packet;         // Used for reading from format context
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
    
    // BSF chain for optimized H.264 stream processing
    bsf_chain_t bsf_chain;      // Bitstream filter chain for H.264
    bool use_bsf_chain;         // Whether to use BSF chain for this stream
    bool first_idr_seen;        // Whether we've seen the first IDR frame
    bool first_au_reordered;    // Whether the first access unit has been reordered
    
    // H.264 Parser for complete access units
    AVCodecParserContext *parser; // H.264 parser context
    bool use_parser;              // Whether to use the parser
    // Access unit aggregation state (ensures full AU packets)
    uint8_t *au_buffer;         // Temporary buffer accumulating Annex-B data
    int au_buffer_size;         // Bytes currently stored in AU buffer
    int au_buffer_capacity;     // Allocated size for AU buffer
    int64_t au_pts;             // PTS for current AU (if any)
    int64_t au_dts;             // DTS for current AU (if any)
    int au_flags;               // Aggregated packet flags (bitwise OR)
    bool au_has_pts;            // Whether AU PTS is valid
    bool au_has_dts;            // Whether AU DTS is valid
    bool au_has_aud;            // AU contains Access Unit Delimiter
    bool au_has_sps;            // AU contains SPS
    bool au_has_pps;            // AU contains PPS
    bool au_has_sei;            // AU contains SEI
    bool au_has_idr;            // AU contains IDR slice
    bool au_has_slice;          // AU contains non-IDR slice
    bool force_annexb_format;   // Force Annex-B format (strip start codes)
    bool insert_aud;            // Insert Access Unit Delimiters
    
    // Error handling and recovery
    error_recovery_t error_recovery;
    int consecutive_errors;     // Count of consecutive packet errors
    int error_threshold;        // Threshold for fallback to software decoding
    bool try_sw_fallback;       // Whether to try software fallback on errors
    AVCodecContext *sw_codec_ctx; // Software decoder context for fallback
    bool is_using_fallback;     // Whether currently using fallback decoder
    
    // Retry logic
    int retry_count;            // Number of retries for current packet
    int max_retries;            // Maximum number of retries per packet
    bool flush_on_reject;       // Whether to flush decoder on packet rejection
    
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
    
    // Diagnostics
    uint64_t packets_processed;
    uint64_t packets_rejected;
    uint64_t frames_decoded;
    uint64_t decode_errors;
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

// New functions for enhanced error handling and configuration
void video_set_error_recovery(video_context_t *video, error_recovery_t mode);
void video_configure_bsf(video_context_t *video, bool force_annexb, bool insert_aud);
bool video_try_fallback_decoder(video_context_t *video);
void video_reset_error_state(video_context_t *video);
void video_print_diagnostics(video_context_t *video);
int video_flush_decoder(video_context_t *video);
void video_set_retry_options(video_context_t *video, int max_retries, bool flush_on_reject);

#endif // VIDEO_DECODER_H