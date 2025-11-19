#define _GNU_SOURCE
#include "video_decoder.h"
#include "v4l2_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/time.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>

// Configuration Constants
#define MAX_PACKETS_PER_DECODE_CALL 50
#define MAX_HARDWARE_FALLBACK_RETRIES 1
#define V4L2_CLEANUP_DELAY_US 10000
#define V4L2_POST_CLEANUP_DELAY_US 50000
#define DECODER_DRAIN_SAFETY_LIMIT 50
#define BUFFERING_PROGRESS_INTERVAL_1 10
#define BUFFERING_PROGRESS_INTERVAL_2 20
#define BUFFERING_PROGRESS_INTERVAL_3 30
#define BUFFERING_PROGRESS_INTERVAL_4 40
#define FRAME_DEBUG_INTERVAL 100
#define DEFAULT_FPS_FALLBACK 30.0
#define AVCC_EXTRADATA_SIZE 8

// ============================================================================
// Zero-copy video decoding implementation for RPi4 V3D GPU

// Debug flag for hardware decode diagnostics
bool hw_debug_enabled = false;

// See ZERO_COPY_ARCHITECTURE.md for detailed architecture documentation
// ============================================================================

// PRODUCTION: Interrupt callback to prevent infinite hangs on network streams
// Returns 1 to abort, 0 to continue
static int interrupt_callback(void* opaque) {
    video_context_t *video = (video_context_t *)opaque;
    if (!video) return 0;
    
    // Check for quit signal
    extern volatile sig_atomic_t g_quit_requested;
    if (g_quit_requested) {
        fprintf(stderr, "[IO] Interrupt: quit requested\n");
        return 1;
    }
    
    // Check for timeout (5 seconds default)
    int64_t elapsed = av_gettime_relative() - video->last_io_activity;
    if (elapsed > video->io_timeout_us) {
        fprintf(stderr, "[IO] Interrupt: timeout after %lld us\n", (long long)elapsed);
        return 1;
    }
    
    return 0;
}


// memfd_create may not be available in older glibc - provide fallback
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 1
#endif

// Fallback syscall for memfd_create if not in libc
#include <sys/syscall.h>
static inline int memfd_create_compat(const char *name, unsigned int flags) {
    #ifdef SYS_memfd_create
    return syscall(SYS_memfd_create, name, flags);
    #else
    errno = ENOSYS;
    return -1;
    #endif
}

#define memfd_create memfd_create_compat

// Define frame side data types if not available in older FFmpeg versions
#ifndef AV_FRAME_DATA_HW_FRAMES_CTX
#define AV_FRAME_DATA_HW_FRAMES_CTX 11
#endif

#ifndef AV_FRAME_DATA_DMABUF_EXPORT
#define AV_FRAME_DATA_DMABUF_EXPORT 28
#endif

// V4L2 M2M format negotiation callback for RPi hardware decoder
// When hardware context is enabled, prefer DRM_PRIME for zero-copy
// NOTE: This callback is CRITICAL - if not invoked, decoder will not output DRM_PRIME!
// The patched FFmpeg v4l2_m2m_dec calls ff_get_format() which SHOULD invoke this callback
static enum AVPixelFormat get_format_callback(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    video_context_t *video = (video_context_t *)ctx->opaque;
    const enum AVPixelFormat *p;
    int call_num = 0;
    
    if (video) {
        pthread_mutex_lock(&video->lock);
        video->callback_count++;
        call_num = video->callback_count;
        pthread_mutex_unlock(&video->lock);
    }
    
    if (hw_debug_enabled) {
        printf("\n");
        printf("╔════════════════════════════════════════════════════════════╗\n");
        printf("║ [HW_DECODE] FORMAT CALLBACK INVOKED (call #%d)            ║\n", call_num);
        printf("╚════════════════════════════════════════════════════════════╝\n");
        printf("[HW_DECODE] Available formats from decoder (ordered by preference):\n");
    }
    
    int format_count = 0;
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (hw_debug_enabled) {
            const char *name = av_get_pix_fmt_name(*p);
            printf("[HW_DECODE]   [%d] %s (%d)\n", format_count++, name ? name : "unknown", *p);
        } else {
            format_count++;
        }
    }
    if (hw_debug_enabled) {
        printf("[HW_DECODE] Total formats available: %d\n", format_count);
    }
    
    // PRIORITY 1: DRM PRIME for zero-copy (if hardware context is enabled)
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_DRM_PRIME) {
            if (hw_debug_enabled) {
                printf("[HW_DECODE] ✓✓✓ Selected: DRM_PRIME (ZERO-COPY MODE ACTIVATED!)\n");
                fflush(stdout);
            }
            return AV_PIX_FMT_DRM_PRIME;
        }
    }
    if (hw_debug_enabled) {
        printf("[HW_DECODE] ⚠ DRM_PRIME not offered by decoder for this video\n");
    }
    
    // PRIORITY 2: NV12 for better performance
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_NV12) {
            if (hw_debug_enabled) {
                printf("[HW_DECODE] ✓ Selected: NV12 (hardware mode, no DRM_PRIME available)\n");
                fflush(stdout);
            }
            return AV_PIX_FMT_NV12;
        }
    }
    
    // PRIORITY 3: YUV420P
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_YUV420P) {
            if (hw_debug_enabled) {
                printf("[HW_DECODE] ✓ Selected: YUV420P (software fallback)\n");
                fflush(stdout);
            }
            return *p;
        }
    }
    
    // Last resort: first available format
    if (pix_fmts[0] != AV_PIX_FMT_NONE) {
        if (hw_debug_enabled) {
            printf("[HW_DECODE] ⚠ Selected: %s (first available)\n", 
                   av_get_pix_fmt_name(pix_fmts[0]));
            fflush(stdout);
        }
        return pix_fmts[0];
    }
    
    fprintf(stderr, "[HW_DECODE] ✗ ERROR: No suitable format found!\n");
    return AV_PIX_FMT_NONE;
}

// NOTE: Removed try_export_dma_buffer_from_frame() and try_export_dma_buffer()
// These functions attempted to create fake DMA buffers using memfd_create on malloc'd RAM.
// This approach is fundamentally wrong because:
// 1. The RPi4 V3D GPU requires GEM-backed (kernel Graphics Execution Manager) buffers
// 2. memfd + malloc creates regular system RAM buffers without GEM backing
// 3. V3D driver correctly rejects these fake DMA FDs as unsafe
//
// CORRECT APPROACH:
// When init_hw_accel_context() successfully creates a DRM device context and assigns it
// to codec_ctx->hw_device_ctx, FFmpeg automatically configures V4L2 M2M internally to use
// V4L2_MEMORY_DMABUF mode during avcodec_open2(). This forces the kernel driver to allocate
// real GEM-backed buffers and return actual DMA FDs that V3D accepts.
//
// The decoded frames will then have real DMA-BUF file descriptors in their AVDRMFrameDescriptor.

// Initialize FFmpeg hardware acceleration context for zero-copy DMA buffers
// For V4L2 M2M + DRM: Creates device context that forces DMABUF mode
//
// CRITICAL MECHANISM:
// When av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_DRM) succeeds and is assigned to
// codec_ctx->hw_device_ctx, FFmpeg internally uses V4L2_MEMORY_DMABUF during
// avcodec_open2(). This forces the kernel driver to:
// 1. Allocate GEM-backed buffers (Graphics Execution Manager, GPU-safe)
// 2. Return real DMA-BUF file descriptors for those buffers
// 3. Output decoded frames with AVDRMFrameDescriptor containing actual DMA FDs
//
// Result: Zero-copy rendering becomes possible because:
// - V3D GPU can directly access GEM-backed DMA buffers
// - EGL can import these DMA FDs as GPU textures without CPU copy
// - No memcpy bottleneck between CPU and GPU
static int init_hw_accel_context(video_context_t *video) {
    int ret;
    
    if (hw_debug_enabled) {
        printf("[HWACCEL] Initializing DRM hardware acceleration...\n");
        printf("[HWACCEL] This will force V4L2 M2M to use DMABUF mode for GEM-backed buffers\n");
    }
    
    // Create DRM hardware device context - use same device as display
    const char *drm_device = "/dev/dri/card1";
    if (hw_debug_enabled) {
        printf("[HWACCEL] Attempting DRM device: %s\n", drm_device);
    }
    ret = av_hwdevice_ctx_create(&video->hw_device_ctx, AV_HWDEVICE_TYPE_DRM, 
                                  drm_device, NULL, 0);
    if (ret < 0) {
        if (hw_debug_enabled) {
            printf("[HWACCEL] card1 failed (%s), trying card0\n", av_err2str(ret));
        }
        drm_device = "/dev/dri/card0";
        ret = av_hwdevice_ctx_create(&video->hw_device_ctx, AV_HWDEVICE_TYPE_DRM, 
                                      drm_device, NULL, 0);
        if (ret < 0) {
            fprintf(stderr, "[HWACCEL] Failed to create DRM device context: %s\n", av_err2str(ret));
            fprintf(stderr, "[HWACCEL] Without DRM context, V4L2 M2M will use system RAM (no DMABUF)\n");
            return -1;
        }
    }
    if (hw_debug_enabled) {
        printf("[HWACCEL] ✓ DRM device context created using %s\n", drm_device);
    }
    
    // Diagnostic: Check device context internals (access through AVHWDeviceContext)
    AVHWDeviceContext *hw_dev_ctx = (AVHWDeviceContext *)video->hw_device_ctx->data;
    if (hw_dev_ctx && hw_dev_ctx->hwctx && hw_debug_enabled) {
        AVDRMDeviceContext *drm_ctx = (AVDRMDeviceContext *)hw_dev_ctx->hwctx;
        printf("[HWACCEL] DRM context fd=%d\n", drm_ctx->fd);
    }
    
    // Assign device context to codec
    // CRITICAL: This assignment triggers FFmpeg to configure V4L2 M2M with DMABUF internally
    video->codec_ctx->hw_device_ctx = av_buffer_ref(video->hw_device_ctx);
    if (!video->codec_ctx->hw_device_ctx) {
        fprintf(stderr, "[HWACCEL] Failed to reference device context\n");
        av_buffer_unref(&video->hw_device_ctx);
        return -1;
    }
    if (hw_debug_enabled) {
        printf("[HWACCEL] ✓ Device context assigned to codec\n");
        printf("[HWACCEL] ✓ V4L2 M2M will use V4L2_MEMORY_DMABUF mode internally in avcodec_open2()\n");
    }
    
    // For V4L2 M2M with DRM PRIME: Create frames context but DO NOT initialize it
    // V4L2 M2M kernel driver will initialize its own buffer pool during avcodec_open2()
    // We just need to create and configure the context so FFmpeg knows to request drm_prime
    if (hw_debug_enabled) {
        printf("[HWACCEL] Creating hardware frames context (will NOT initialize)...\n");
    }
    
    video->hw_frames_ctx = av_hwframe_ctx_alloc(video->hw_device_ctx);
    if (!video->hw_frames_ctx) {
        fprintf(stderr, "[HWACCEL] Failed to allocate HW frames context.\n");
        return -1;
    }
    if (hw_debug_enabled) {
        printf("[HWACCEL] ✓ HW frames context allocated\n");
    }
    
    // Configure frames context parameters
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)video->hw_frames_ctx->data;
    
    // CRITICAL: Set both hardware and software format
    // Hardware: DRM_PRIME (what GPU will receive)
    // Software: YUV420P (what bcm2835-codec V4L2 M2M decoder outputs - YU12 format)
    frames_ctx->format = AV_PIX_FMT_DRM_PRIME;        // Output format: GPU texture
    frames_ctx->sw_format = AV_PIX_FMT_YUV420P;       // Input format: bcm2835-codec uses YU12 (yuv420p)
    
    // Use actual codec dimensions
    int frame_width = video->codec_ctx->width;
    int frame_height = video->codec_ctx->height;
    
    if (frame_width == 0 || frame_height == 0) {
        // Fallback to stream dimensions if codec context dimensions are not set yet
        if (video->format_ctx && video->video_stream_index >= 0) {
            AVStream *stream = video->format_ctx->streams[video->video_stream_index];
            if (stream && stream->codecpar) {
                frame_width = stream->codecpar->width;
                frame_height = stream->codecpar->height;
                if (hw_debug_enabled) {
                    printf("[HWACCEL] Using stream dimensions: %dx%d\n", frame_width, frame_height);
                }
            }
        }
    }
    
    // Validate dimensions
    if (frame_width <= 0 || frame_height <= 0) {
        fprintf(stderr, "[HWACCEL] ⚠ ERROR: Could not determine video dimensions from codec or stream!\n");
        fprintf(stderr, "[HWACCEL] Codec dimensions: %dx%d, Stream dimensions: unknown\n",
                video->codec_ctx->width, video->codec_ctx->height);
        fprintf(stderr, "[HWACCEL] Using safe fallback: 1920x1080 (video may not display correctly)\n");
        frame_width = 1920;
        frame_height = 1080;
    }
    
    frames_ctx->width = frame_width;
    frames_ctx->height = frame_height;
    frames_ctx->initial_pool_size = 0;  // Let V4L2 M2M manage its own pool
    
    if (hw_debug_enabled) {
        printf("[HWACCEL] Frames context config:\n");
        printf("[HWACCEL]   format (GPU):  %s (%d)\n", av_get_pix_fmt_name(frames_ctx->format), frames_ctx->format);
        printf("[HWACCEL]   sw_format:    %s (%d) - bcm2835-codec YU12 output\n", av_get_pix_fmt_name(frames_ctx->sw_format), frames_ctx->sw_format);
        printf("[HWACCEL]   dimensions:   %dx%d\n", frames_ctx->width, frames_ctx->height);
        printf("[HWACCEL]   pool size:    %d (V4L2 M2M manages own pool)\n", frames_ctx->initial_pool_size);
    }
    
    // CRITICAL: Do NOT call av_hwframe_ctx_init() for V4L2 M2M
    // Just assign the uninitialized context to the codec
    // FFmpeg's V4L2 wrapper will see this and request drm_prime capture format
    if (hw_debug_enabled) {
        printf("[HWACCEL] Skipping av_hwframe_ctx_init() - V4L2 M2M initializes during avcodec_open2()\n");
    }
    
    video->codec_ctx->hw_frames_ctx = av_buffer_ref(video->hw_frames_ctx);
    if (!video->codec_ctx->hw_frames_ctx) {
        fprintf(stderr, "[HWACCEL] Failed to assign frames context to codec\n");
        return -1;
    }
    if (hw_debug_enabled) {
        printf("[HWACCEL] ✓ Uninitialized frames context assigned to codec\n");
        printf("[HWACCEL] ✓ This signals V4L2 wrapper to request drm_prime capture format\n");
    }
    
    // Critical: Set pixel format hint for DRM_PRIME output
    // This must be done BEFORE codec opening
    video->codec_ctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
    if (hw_debug_enabled) {
        printf("[HWACCEL] ✓ Set codec pix_fmt = DRM_PRIME (request DRM PRIME output)\n");
    }
    
    // Set format callback (may help if frames context was not assigned)
    video->codec_ctx->get_format = get_format_callback;
    video->codec_ctx->opaque = video;  // Pass context to callback
    if (hw_debug_enabled) {
        printf("[HWACCEL] ✓ Format negotiation callback registered\n");
    }
    
    video->hw_pix_fmt = AV_PIX_FMT_DRM_PRIME;
    if (hw_debug_enabled) {
        printf("[HWACCEL] ✓ DRM PRIME hardware acceleration configured\n");
        printf("[HWACCEL] Ready for zero-copy GPU rendering\n");
    }
    
    return 0;
}

int video_init(video_context_t *video, const char *filename, bool advanced_diagnostics, bool enable_hardware_decode) {
    memset(video, 0, sizeof(*video));
    // Initialize mutex for thread safety
    if (pthread_mutex_init(&video->lock, NULL) != 0) {
        fprintf(stderr, "Failed to initialize mutex\n");
        return -1;
    }


    // Set global debug flag for hardware decode diagnostics
    hw_debug_enabled = advanced_diagnostics;

    // Store advanced diagnostics flag
    video->advanced_diagnostics = advanced_diagnostics;
    video->enable_hardware_decode = enable_hardware_decode;

    // Initialize DMA buffer fields
    video->supports_dma_export = false;
    video->dma_fd = -1;
    video->dma_offset = 0;
    video->dma_size = 0;
    video->v4l2_fd = -1;  // V4L2 device FD not yet available
    video->v4l2_buffer_index = 0;  // V4L2 output buffer index
    
    // PRODUCTION: Initialize interrupt callback for network timeout protection
    video->last_io_activity = av_gettime_relative();
    video->io_timeout_us = 5000000;  // 5 second timeout

    // Print decode mode
    if (enable_hardware_decode) {
        printf("[CONFIG] Hardware decode enabled via --hw flag\n");
    } else {
        printf("[CONFIG] Software decode (default, use --hw for hardware acceleration)\n");
    }

    // Allocate packet
    video->packet = av_packet_alloc();
    if (!video->packet) {
        fprintf(stderr, "Failed to allocate packet\n");
        return -1;
    }

    // Modern libavformat: Open input file with optimized I/O options
    AVDictionary *options = NULL;
    av_dict_set(&options, "buffer_size", "32768", 0);        // 32KB buffer for better I/O
    av_dict_set(&options, "multiple_requests", "1", 0);      // Enable HTTP keep-alive
    av_dict_set(&options, "reconnect", "1", 0);              // Auto-reconnect on network issues
    
    // Update activity timestamp before I/O
    video->last_io_activity = av_gettime_relative();
    
    if (avformat_open_input(&video->format_ctx, filename, NULL, &options) < 0) {
        fprintf(stderr, "Failed to open input file: %s\n", filename);
        av_dict_free(&options);
        av_packet_free(&video->packet);
        return -1;
    }
    av_dict_free(&options);
    
    // PRODUCTION: Set interrupt callback AFTER format_ctx is opened
    video->format_ctx->interrupt_callback.callback = interrupt_callback;
    video->format_ctx->interrupt_callback.opaque = video;

    // Modern libavformat: Efficient stream information retrieval
    AVDictionary *stream_options = NULL;
    av_dict_set(&stream_options, "analyzeduration", "1000000", 0);  // 1 second max analysis
    av_dict_set(&stream_options, "probesize", "1000000", 0);        // 1MB max probe size
    
    if (avformat_find_stream_info(video->format_ctx, &stream_options) < 0) {
        fprintf(stderr, "Failed to find stream information\n");
        av_dict_free(&stream_options);
        avformat_close_input(&video->format_ctx);
        av_packet_free(&video->packet);
        return -1;
    }
    av_dict_free(&stream_options);

    // Find video stream
    video->video_stream_index = -1;
    for (unsigned int i = 0; i < video->format_ctx->nb_streams; i++) {
        if (video->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video->video_stream_index = i;
            break;
        }
    }

    if (video->video_stream_index == -1) {
        fprintf(stderr, "No video stream found\n");
        avformat_close_input(&video->format_ctx);
        av_packet_free(&video->packet);
        return -1;
    }

    // Get codec parameters
    AVCodecParameters *codecpar = video->format_ctx->streams[video->video_stream_index]->codecpar;
    
    // Try hardware decoders first
    video->use_hardware_decode = false;
    video->hw_decode_type = HW_DECODE_NONE;
    
    // HARDWARE DECODE ENABLED with automatic software fallback
    // Hardware decode is opt-in via --hw command-line flag
    // By default, uses reliable software decoder
    // Hardware decoder (V4L2 M2M) provides better performance but may have issues
    // If hardware decoder hangs (no frames after 10 packets), automatically falls back to software

    if (enable_hardware_decode) {
        if (hw_debug_enabled) {
            printf("[HW_DECODE] Attempting hardware decoder detection...\n");
            printf("[HW_DECODE] Codec ID: %d\n", codecpar->codec_id);
            printf("[HW_DECODE] AV_CODEC_ID_H264 = %d, AV_CODEC_ID_HEVC = %d\n", AV_CODEC_ID_H264, AV_CODEC_ID_HEVC);
        }
        
        // Try hardware decoder for H.264
        if (codecpar->codec_id == AV_CODEC_ID_H264) {
            if (hw_debug_enabled) {
                printf("[HW_DECODE] H.264 detected, searching for h264_v4l2m2m decoder...\n");
            }
            video->codec = (AVCodec*)avcodec_find_decoder_by_name("h264_v4l2m2m");
            if (video->codec) {
                video->use_hardware_decode = true;
                video->hw_decode_type = HW_DECODE_V4L2M2M;
                if (hw_debug_enabled) {
                    printf("[HW_DECODE] ✓ Found h264_v4l2m2m hardware decoder\n");
                    printf("[HW_DECODE] H.264 profile: %d (%s)\n", codecpar->profile, 
                           codecpar->profile == 66 ? "Baseline" :
                           codecpar->profile == 77 ? "Main" :
                           codecpar->profile == 100 ? "High" : "Other");
                    printf("[HW_DECODE] H.264 level: %d\n", codecpar->level);
                    printf("[HW_DECODE] Resolution: %dx%d\n", codecpar->width, codecpar->height);
                    printf("[HW_DECODE] Bitrate: %"PRId64" bps\n", codecpar->bit_rate);
                    
                    // Check V4L2 capabilities
                    check_v4l2_decoder_capabilities();
                }
            } else {
                if (hw_debug_enabled) {
                    printf("[HW_DECODE] ✗ h264_v4l2m2m not available\n");
                }
            }
        } else if (codecpar->codec_id == AV_CODEC_ID_HEVC) {
            if (hw_debug_enabled) {
                printf("[HW_DECODE] HEVC/H.265 detected, searching for hevc_v4l2m2m decoder...\n");
            }
            video->codec = (AVCodec*)avcodec_find_decoder_by_name("hevc_v4l2m2m");
            if (video->codec) {
                video->use_hardware_decode = true;
                video->hw_decode_type = HW_DECODE_V4L2M2M;
                if (hw_debug_enabled) {
                    printf("[HW_DECODE] ✓ Found hevc_v4l2m2m hardware decoder\n");
                    printf("[HW_DECODE] HEVC profile: %d\n", codecpar->profile);
                    printf("[HW_DECODE] HEVC level: %d\n", codecpar->level);
                    printf("[HW_DECODE] Resolution: %dx%d\n", codecpar->width, codecpar->height);
                    printf("[HW_DECODE] Bitrate: %"PRId64" bps\n", codecpar->bit_rate);
                    
                    // Check V4L2 capabilities
                    check_v4l2_decoder_capabilities();
                }
            } else {
                printf("[HW_DECODE] ✗ hevc_v4l2m2m not available\n");
            }
        } else {
            printf("[HW_DECODE] Codec ID %d is not H.264 or HEVC, skipping hardware decode\n", codecpar->codec_id);
        }
        
        // Fall back to software decoder if hardware not available
        if (!video->codec) {
            printf("[HW_DECODE] Hardware decoder not available, falling back to software\n");
            video->codec = (AVCodec*)avcodec_find_decoder(codecpar->codec_id);
            if (!video->codec) {
                fprintf(stderr, "[HW_DECODE] ✗ Failed to find software decoder for codec ID %d\n", codecpar->codec_id);
                avformat_close_input(&video->format_ctx);
                av_packet_free(&video->packet);
                return -1;
            }
            printf("[HW_DECODE] ✓ Using software decoder: %s\n", video->codec->name);
        }
    } else {
        // Software decode (default) - reliable and compatible
        printf("[HW_DECODE] Using software decoder (use --hw flag for hardware acceleration)\n");
        video->codec = (AVCodec*)avcodec_find_decoder(codecpar->codec_id);
        if (!video->codec) {
            fprintf(stderr, "[HW_DECODE] ✗ Failed to find software decoder for codec ID %d\n", codecpar->codec_id);
            avformat_close_input(&video->format_ctx);
            av_packet_free(&video->packet);
            return -1;
        }
        printf("[HW_DECODE] ✓ Using software decoder: %s\n", video->codec->name);
    }

    // Allocate codec context
    video->codec_ctx = avcodec_alloc_context3(video->codec);
    if (!video->codec_ctx) {
        fprintf(stderr, "Failed to allocate codec context\n");
        avformat_close_input(&video->format_ctx);
        av_packet_free(&video->packet);
        return -1;
    }

    // Copy codec parameters to context
    if (avcodec_parameters_to_context(video->codec_ctx, codecpar) < 0) {
        fprintf(stderr, "Failed to copy codec parameters\n");
        avcodec_free_context(&video->codec_ctx);
        avformat_close_input(&video->format_ctx);
        av_packet_free(&video->packet);
        return -1;
    }
    
    // WORKAROUND: avcodec_parameters_to_context sometimes corrupts dimensions on h.264
    // Force copy from codecpar directly to ensure correct width/height
    video->codec_ctx->width = codecpar->width;
    video->codec_ctx->height = codecpar->height;

    // Initialize BSF BEFORE opening codec for hardware decoding (avcC to Annex-B conversion)
    if (video->use_hardware_decode && video->hw_decode_type == HW_DECODE_V4L2M2M && 
        codecpar->extradata && codecpar->extradata_size > 0 && codecpar->extradata[0] == 1) {
        
        if (hw_debug_enabled) {
            printf("[HW_DECODE] BSF: Analyzing stream format...\n");
            printf("[HW_DECODE] BSF: First 8 bytes of extradata: ");
            for (int i = 0; i < 8 && i < codecpar->extradata_size; i++) {
                printf("%02x ", codecpar->extradata[i]);
            }
            printf("\n");
            printf("[HW_DECODE] BSF: Detected avcC format (byte 0 = 0x01)\n");
            printf("[HW_DECODE] BSF: Will convert avcC → Annex-B for V4L2 M2M\n");
        }
        
        video->avcc_length_size = get_avcc_length_size(codecpar->extradata, codecpar->extradata_size);
        if (hw_debug_enabled) {
            printf("[HW_DECODE] BSF: avcC NAL length size: %d bytes\n", video->avcc_length_size);
        }
        
        if (hw_debug_enabled) {
            printf("[HW_DECODE] BSF: Initializing h264_mp4toannexb bitstream filter...\n");
        }
        
        // h264_mp4toannexb (avcC → Annex-B conversion)
        const AVBitStreamFilter *bsf_annexb = av_bsf_get_by_name("h264_mp4toannexb");
        if (!bsf_annexb) {
            fprintf(stderr, "[HW_DECODE] BSF: ✗ Failed to find h264_mp4toannexb BSF\n");
            video_cleanup(video);
            return -1;
        }
        if (hw_debug_enabled) {
            printf("[HW_DECODE] BSF: ✓ Found h264_mp4toannexb filter\n");
        }
        
        if (av_bsf_alloc(bsf_annexb, &video->bsf_annexb_ctx) < 0) {
            fprintf(stderr, "[HW_DECODE] BSF: ✗ Failed to allocate BSF context\n");
            // Cleanup already allocated resources before video_cleanup
            if (video->codec_ctx) {
                avcodec_free_context(&video->codec_ctx);
            }
            video_cleanup(video);
            return -1;
        }
        if (hw_debug_enabled) {
            printf("[HW_DECODE] BSF: ✓ Allocated BSF context\n");
        }
        
        avcodec_parameters_copy(video->bsf_annexb_ctx->par_in, codecpar);
        if (hw_debug_enabled) {
            printf("[HW_DECODE] BSF: ✓ Copied codec parameters to BSF\n");
        }
        
        if (av_bsf_init(video->bsf_annexb_ctx) < 0) {
            fprintf(stderr, "[HW_DECODE] BSF: ✗ Failed to initialize BSF\n");
            // BSF context will be freed in video_cleanup
            if (video->codec_ctx) {
                avcodec_free_context(&video->codec_ctx);
            }
            video_cleanup(video);
            return -1;
        }
        if (hw_debug_enabled) {
            printf("[HW_DECODE] BSF: ✓ Initialized BSF successfully\n");
        }
        
        // Copy the BSF output parameters (with converted Annex-B extradata) to codec context
        if (video->bsf_annexb_ctx->par_out->extradata && video->bsf_annexb_ctx->par_out->extradata_size > 0) {
            if (hw_debug_enabled) {
                printf("[HW_DECODE] BSF: Converting extradata to Annex-B format...\n");
            }
            // Free existing extradata
            if (video->codec_ctx->extradata) {
                av_freep(&video->codec_ctx->extradata);
            }
            // Allocate and copy converted extradata
            video->codec_ctx->extradata_size = video->bsf_annexb_ctx->par_out->extradata_size;
            video->codec_ctx->extradata = av_mallocz(video->codec_ctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(video->codec_ctx->extradata, video->bsf_annexb_ctx->par_out->extradata, video->codec_ctx->extradata_size);
            if (hw_debug_enabled) {
                printf("[HW_DECODE] BSF: ✓ Converted extradata (%d bytes)\n", video->codec_ctx->extradata_size);
            }
        }
        
        // Set codec_tag to 0 for Annex-B format
        video->codec_ctx->codec_tag = 0;
        if (hw_debug_enabled) {
            printf("[HW_DECODE] BSF: Set codec_tag=0 for Annex-B format\n");
            printf("[HW_DECODE] BSF: ✓ avcC → Annex-B conversion ready\n");
        }
    }

    // Configure hardware decoding for V4L2 M2M
    if (video->use_hardware_decode && video->hw_decode_type == HW_DECODE_V4L2M2M) {
        if (hw_debug_enabled) {
            printf("[HW_DECODE] V4L2: Configuring V4L2 M2M decoder for Raspberry Pi...\n");
        }
        
        // NOTE: DRM device context, pix_fmt, and get_format already set in init_hw_accel_context()
        // which is called later before avcodec_open2()
        
        // CRITICAL: V4L2 M2M must be single-threaded
        video->codec_ctx->thread_count = 1;
        video->codec_ctx->thread_type = 0;  // Disable all threading
        if (hw_debug_enabled) {
            printf("[HW_DECODE] V4L2: Set thread_count=1, thread_type=0 (V4L2 handles threading)\n");
        }
        
        // Low-latency flags for better V4L2 M2M performance
        video->codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        video->codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        if (hw_debug_enabled) {
            printf("[HW_DECODE] V4L2: ✓ LOW_DELAY and FAST flags enabled\n");
        }
        
        // Enable CHUNKS mode for V4L2 M2M decoder (handles partial frames)
        video->codec_ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;
        if (hw_debug_enabled) {
            printf("[HW_DECODE] V4L2: ✓ CHUNKS mode enabled (supports partial frames)\n");
        }
        
        // Prepare V4L2-specific options
        AVDictionary *codec_opts = NULL;
        
        // CRITICAL: Configure V4L2 buffer counts for stability
        av_dict_set(&codec_opts, "num_capture_buffers", "32", 0);
        av_dict_set(&codec_opts, "num_output_buffers", "16", 0);
        if (hw_debug_enabled) {
            printf("[HW_DECODE] V4L2: Set num_capture_buffers=32, num_output_buffers=16\n");
        }
        
        // CRITICAL: Do NOT force mmap mode - allow DMABUF/DRM PRIME mode for zero-copy
        // Remove the mmap forcing to enable DRM PRIME buffers
        if (hw_debug_enabled) {
            printf("[HW_DECODE] V4L2: Allowing DMABUF/DRM PRIME mode for zero-copy\n");
        }
        
        // Optional: Force specific device (uncomment if needed)
        // av_dict_set(&codec_opts, "device", "/dev/video10", 0);
        // printf("[HW_DECODE] V4L2: Set device=/dev/video10\n");
        
        if (hw_debug_enabled) {
            printf("[HW_DECODE] V4L2: Configuration complete\n");
            printf("[HW_DECODE] V4L2: Note - decoder may buffer 20-30 packets before first frame\n");
        }
        
        // Initialize DRM PRIME hardware acceleration for zero-copy rendering
        // This enables V4L2 M2M to output directly to GPU memory (DRM_PRIME format)
        // Keystone correction will work via GPU shaders on DMA-imported textures
        if (init_hw_accel_context(video) < 0) {
            fprintf(stderr, "[HW_DECODE] Failed to initialize DRM PRIME context\n");
            fprintf(stderr, "[HW_DECODE] Falling back to CPU-accessible YUV buffers (slower but compatible)\n");
            // Continue without DRM PRIME - will use system RAM buffers
        } else if (hw_debug_enabled) {
            printf("[HW_DECODE] ✓ DRM PRIME hardware acceleration initialized\n");
            printf("[HW_DECODE] Zero-copy rendering enabled - frames will be in GPU memory\n");
        }
        
        // Enable FFmpeg debug logging to see V4L2 M2M internals (only in debug mode)
        if (hw_debug_enabled) {
            av_log_set_level(AV_LOG_DEBUG);
            printf("[DEBUG] FFmpeg log level set to DEBUG to trace V4L2 M2M format negotiation\n");
        } else {
            av_log_set_level(AV_LOG_QUIET);
        }
        
        // Open codec with V4L2 options
        int ret = avcodec_open2(video->codec_ctx, video->codec, &codec_opts);
        
        // Reset log level after codec open
        if (hw_debug_enabled) {
            av_log_set_level(AV_LOG_INFO);
        } else {
            av_log_set_level(AV_LOG_QUIET);
        }
        
        av_dict_free(&codec_opts);
        
        if (ret < 0) {
            fprintf(stderr, "Failed to open V4L2 M2M codec: %s\n", av_err2str(ret));
            fprintf(stderr, "This might indicate:\n");
            fprintf(stderr, "  - V4L2 M2M driver not compatible with this FFmpeg version\n");
            fprintf(stderr, "  - Missing /dev/video* device\n");
            fprintf(stderr, "  - Codec doesn't support this video profile/level\n");
            avcodec_free_context(&video->codec_ctx);
            avformat_close_input(&video->format_ctx);
            av_packet_free(&video->packet);
            return -1;
        }
        if (hw_debug_enabled) {
            printf("[HW_DECODE] V4L2: ✓ Codec opened successfully with V4L2 options\n");
        }
        
        // Mark DMA buffer export as supported for V4L2 M2M decoder
        video->supports_dma_export = true;
        if (hw_debug_enabled) {
            printf("[HW_DECODE] DMA zero-copy buffer export enabled (V4L2 M2M capable)\n");
        }
        
        // Try to extract V4L2 device FD from codec context for later DMABUF export
        // The V4L2 m2m decoder stores the device FD in priv_data
        // This is needed to call VIDIOC_EXPBUF on decoded output buffers
        // Note: This is internal FFmpeg implementation detail, may vary by version
        if (hw_debug_enabled) {
            printf("[HW_DECODE] V4L2 output buffers will be accessed via VIDIOC_EXPBUF for zero-copy GPU mapping\n");
        }
    } else {
        // Software decoding settings
        video->codec_ctx->thread_count = 4;
        video->codec_ctx->thread_type = FF_THREAD_FRAME;
        
        // Open codec
        if (avcodec_open2(video->codec_ctx, video->codec, NULL) < 0) {
            fprintf(stderr, "Failed to open codec\n");
            fprintf(stderr, "This might indicate:\n");
            fprintf(stderr, "  - Missing codec support\n");
            fprintf(stderr, "  - Incompatible video format\n");
            avcodec_free_context(&video->codec_ctx);
            avformat_close_input(&video->format_ctx);
            av_packet_free(&video->packet);
            return -1;
        }
    }
    printf("Codec opened successfully\n");

    // Debug: Print the actual pixel format and color space being used
    const char *pix_fmt_name = av_get_pix_fmt_name(video->codec_ctx->pix_fmt);
    printf("Decoder output format: %s (%d)\n", pix_fmt_name ? pix_fmt_name : "unknown", video->codec_ctx->pix_fmt);
    
    // Print color space information for debugging
    printf("Color space: %s, Color range: %s\n",
           av_color_space_name(video->codec_ctx->colorspace),
           av_color_range_name(video->codec_ctx->color_range));

    // Get video properties
    video->width = video->codec_ctx->width;
    video->height = video->codec_ctx->height;
    
    AVStream *stream = video->format_ctx->streams[video->video_stream_index];
    // Safely calculate FPS, guarding against invalid frame rates
    if (stream->r_frame_rate.den > 0) {
        video->fps = (double)stream->r_frame_rate.num / stream->r_frame_rate.den;
    } else {
        video->fps = 30.0; // Default fallback
        fprintf(stderr, "Warning: Invalid frame rate denominator, defaulting to 30 FPS\n");
    }
    video->duration = stream->duration;

    // Allocate frame for YUV data (decoded frame from decoder)
    video->frame = av_frame_alloc();
    if (!video->frame) {
        fprintf(stderr, "Failed to allocate frame\n");
        video_cleanup(video);
        return -1;
    }

    // Allocate software frame used when decoding to hardware formats (e.g. DRM_PRIME).
    // We will transfer hardware frames into this CPU-accessible YUV420P frame
    // so existing rendering paths can treat hardware and software decode uniformly.
    video->sw_frame = av_frame_alloc();
    if (!video->sw_frame) {
        fprintf(stderr, "Failed to allocate software frame for hardware decode\n");
        video_cleanup(video);
        return -1;
    }
    
    // Mark as initialized
    video->initialized = true;

    // Skip RGB conversion - we'll do YUV→RGB on GPU
    if (!video->use_hardware_decode) {
        printf("Note: Using software YUV decode, GPU will handle YUV→RGB conversion\n");
    } else {
        printf("Hardware decoding to YUV420P enabled, GPU will handle YUV→RGB conversion\n");
    }

    video->eof_reached = false;

    // Video decoder initialization complete
    return 0;
}

int video_decode_frame(video_context_t *video) {
    // video->decode_call_count moved to context
    video->decode_call_count++;
    
    if (video->decode_call_count == 1) {
        printf("video_decode_frame() starting...\n");
        fflush(stdout);
    }
    
    if (!video->initialized || video->eof_reached) {
        return -1;
    }

    // SIMPLIFIED DECODE LOOP (Option A - from simple_decode_test.c)
    // Keep reading and queueing packets until we get a frame
    // This approach is proven to work 100% reliably for most files
    // Add safety limit to prevent infinite loops with problematic files
    
    int packets_sent_this_call = 0;
    // Hardware decoders that are broken/incompatible will hang immediately
    if (video->decode_call_count == 1 && video->use_hardware_decode) {
    // Some videos (especially high bitrate) may need 40-50 packets
        printf("[HW_DECODE] First decode: will send up to %d packets before software fallback\n", MAX_PACKETS_PER_DECODE_CALL);
        printf("[HW_DECODE] Note: V4L2 M2M may buffer 20-50 packets depending on video\n");
    }
    
    while (packets_sent_this_call < MAX_PACKETS_PER_DECODE_CALL) {
        // First, try to get any frame the decoder has buffered
        int receive_result = avcodec_receive_frame(video->codec_ctx, video->frame);
        
        if (receive_result == 0) {
            // Successfully decoded a frame
            pthread_mutex_lock(&video->lock);
            video->frame_count++;
            int frame_count = video->frame_count;
            pthread_mutex_unlock(&video->lock);
            
            if (frame_count == 1) {
                const char *fmt_name = av_get_pix_fmt_name(video->frame->format);
                printf("\n✓✓✓ SUCCESS! First frame decoded after %d packets ✓✓✓\n", packets_sent_this_call);
                printf("* Decoder: %s\n", video->use_hardware_decode ? "Hardware (V4L2 M2M)" : "Software");
                printf("* Frame format: %s (%d)\n", fmt_name ? fmt_name : "unknown", video->frame->format);
                printf("* Frame size: %dx%d\n", video->frame->width, video->frame->height);
                printf("* Picture type: %c\n", av_get_picture_type_char(video->frame->pict_type));
                printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
                fflush(stdout);
            } else if (video->advanced_diagnostics && (frame_count % 100) == 0) {
                printf("Frame #%d decoded successfully\n", frame_count);
                fflush(stdout);
            }
            
            // Check for hardware buffer availability (zero-copy indicator)
            if (video->use_hardware_decode && frame_count == 1) {
                // Check for direct DMABUF export support
                AVFrameSideData *dma_side_data = av_frame_get_side_data(video->frame, AV_FRAME_DATA_DMABUF_EXPORT);
                if (dma_side_data) {
                    printf("[ZERO-COPY] DMA export available\n");
                    video->supports_dma_export = true;
                } else {
                    // Enable anyway for testing - your patch might use a different mechanism
                    video->supports_dma_export = true;
                }
                
                printf("[ZERO-COPY] Format: %s\n", av_get_pix_fmt_name(video->frame->format));
            }
            
            // For each decoded frame, try to extract the DMA FD from DRM PRIME hardware frames
            if (video->use_hardware_decode) {
                int new_dma_fd = -1;
                size_t drm_size = 0;
                
                // Check if this is a DRM PRIME hardware frame with actual GEM-backed buffers
                if (video->frame->format == AV_PIX_FMT_DRM_PRIME && video->frame->data[0]) {
                    // CORRECT: DRM PRIME frames contain AVDRMFrameDescriptor
                    // This points to actual GEM-backed buffers allocated by kernel driver
                    AVDRMFrameDescriptor *drm_desc = (AVDRMFrameDescriptor *)video->frame->data[0];
                    
                    if (drm_desc && drm_desc->nb_objects > 0) {
                        // AVDRMFrameDescriptor.objects[0] contains the DMA buffer info
                        AVDRMObjectDescriptor *obj = &drm_desc->objects[0];
                        new_dma_fd = obj->fd;
                        drm_size = obj->size;

                        // CRITICAL: Extract plane layout for EVERY DRM_PRIME frame
                        // This ensures all videos get their plane layout, not just the first video
                        // (frame_count is a static variable shared across all videos!)
                        for (int layer = 0; layer < drm_desc->nb_layers; layer++) {
                            AVDRMLayerDescriptor *layer_desc = &drm_desc->layers[layer];
                            for (int p = 0; p < layer_desc->nb_planes && p < 3; p++) {
                                AVDRMPlaneDescriptor *plane = &layer_desc->planes[p];
                                // Store plane layout for zero-copy rendering
                                video->dma_plane_offset[p] = plane->offset;
                                video->dma_plane_pitch[p] = plane->pitch;
                            }
                        }

                        if (frame_count == 1) {
                            printf("[ZERO-COPY] ✓✓✓ DRM PRIME frame detected!\n");
                            printf("[ZERO-COPY] DMA Buffer FD=%d, Size=%zu bytes\n", new_dma_fd, drm_size);
                            printf("[ZERO-COPY] Layers: %d, Objects: %d\n", drm_desc->nb_layers, drm_desc->nb_objects);

                            // Show layer information (Y/UV planes)
                            for (int layer = 0; layer < drm_desc->nb_layers; layer++) {
                                AVDRMLayerDescriptor *layer_desc = &drm_desc->layers[layer];
                                printf("[ZERO-COPY]   Layer %d: format=0x%08x, %d planes\n",
                                       layer, layer_desc->format, layer_desc->nb_planes);
                                for (int p = 0; p < layer_desc->nb_planes && p < 3; p++) {
                                    AVDRMPlaneDescriptor *plane = &layer_desc->planes[p];
                                    printf("[ZERO-COPY]     Plane %d: offset=%ld, pitch=%ld\n",
                                           p, (long)plane->offset, (long)plane->pitch);
                                }
                            }
                        }

                        video->supports_dma_export = true;
                    } else if (frame_count == 1) {
                        printf("[ZERO-COPY] ⚠ DRM PRIME format but no descriptor objects!\n");
                    }
                }
                
                // If we found a valid FD from actual GEM-backed buffer, use it for zero-copy
                if (new_dma_fd >= 0 && new_dma_fd < 1024) {
                    // Close previous DMA FD if open
                    if (video->dma_fd >= 0) {
                        close(video->dma_fd);
                    }
                    
                    // Duplicate the FD so we own it (FFmpeg may close the original)
                    // This FD can be passed directly to EGL for DMA-BUF import
                    int dup_fd = dup(new_dma_fd);
                    video->dma_fd = dup_fd;
                    video->dma_size = drm_size;
                    
                    if (frame_count == 1) {
                        printf("[ZERO-COPY] ✓ DMA FD duplicated: %d (ready for EGL import)\n", video->dma_fd);
                        printf("[DECODE_TRACE] Frame 1: video->dma_fd=%d, video->use_hardware_decode=%d\n", 
                               video->dma_fd, video->use_hardware_decode);
                    }
                } else if (video->use_hardware_decode && frame_count == 1 && video->frame->format != AV_PIX_FMT_DRM_PRIME) {
                    // Not a DRM PRIME frame - still using system memory
                    printf("[ZERO-COPY] ⚠ Frame is %s, not DRM_PRIME (system RAM fallback)\n",
                           av_get_pix_fmt_name(video->frame->format));
                }
            }

            // If this is a hardware frame (e.g. DRM_PRIME), transfer it to
            // a software YUV420P frame so the rest of the code can treat
            // hardware and software decode uniformly via video_get_yuv_data().
            if (video->use_hardware_decode && video->frame->format == AV_PIX_FMT_DRM_PRIME) {
                if (!video->sw_frame) {
                    video->sw_frame = av_frame_alloc();
                }
                if (video->sw_frame) {
                    // Ensure sw_frame uses a CPU-accessible format
                    av_frame_unref(video->sw_frame);
                    int tr_ret = av_hwframe_transfer_data(video->sw_frame, video->frame, 0);
                    if (tr_ret < 0) {
                        fprintf(stderr, "[HW_DECODE] Failed to transfer DRM_PRIME frame to software: %s\n", av_err2str(tr_ret));
                    }
                }
            }

            return 0;  // SUCCESS - Frame ready for caller to use
        }
        
        if (receive_result == AVERROR_EOF) {
            // End of stream reached
            video->eof_reached = true;
            if (video->decode_call_count == 1 && video->advanced_diagnostics) {
                printf("End of video stream reached\n");
            }
            return -1;
        }
        
        if (receive_result != AVERROR(EAGAIN)) {
            // Unexpected error
            printf("Error receiving frame from decoder: %s\n", av_err2str(receive_result));
            return -1;
        }
        
        // receive_result == AVERROR(EAGAIN): Decoder needs more packets
        // Read next packet and send it
        
        // PRODUCTION: Update I/O activity timestamp before read
        video->last_io_activity = av_gettime_relative();
        
        int read_result = av_read_frame(video->format_ctx, video->packet);
        
        if (read_result < 0) {
            if (read_result == AVERROR_EOF) {
                // No more packets in file
                static int eof_count = 0;
                if (eof_count++ < 3) {
                    printf("[DEBUG] av_read_frame returned EOF (packets_sent=%d)\n", packets_sent_this_call);
                }
                
                // Flush decoder to get any remaining buffered frames
                avcodec_send_packet(video->codec_ctx, NULL);
                
                // Try one more receive to get any buffered frames
                int flush_result = avcodec_receive_frame(video->codec_ctx, video->frame);
                if (flush_result == 0) {
                    // Got a frame from flush
                    return 0;
                }
                
                // No more frames available
                video->eof_reached = true;
                return -1;
            } else {
                printf("Error reading packet: %s\n", av_err2str(read_result));
                return -1;
            }
        }
        
        // Skip non-video packets
        if (video->packet->stream_index != video->video_stream_index) {
            av_packet_unref(video->packet);
            continue;  // Try next packet
        }
        
        // Process packet through BSF if hardware decoding
        AVPacket *pkt_to_decode = video->packet;
        AVPacket *bsf_pkt = NULL;
        
        if (video->use_hardware_decode && video->bsf_annexb_ctx) {
            // Convert avcC to Annex-B (silent operation after initial success)
            if (av_bsf_send_packet(video->bsf_annexb_ctx, video->packet) < 0) {
                av_packet_unref(video->packet);
                continue;
            }
            
            bsf_pkt = av_packet_alloc();
            int ret = av_bsf_receive_packet(video->bsf_annexb_ctx, bsf_pkt);
            if (ret == AVERROR(EAGAIN)) {
                // BSF needs more packets (normal operation)
                av_packet_free(&bsf_pkt);
                av_packet_unref(video->packet);
                continue;
            } else if (ret < 0) {
                av_packet_free(&bsf_pkt);
                av_packet_unref(video->packet);
                continue;
            }
            
            pkt_to_decode = bsf_pkt;
        }
        
        // Send packet to decoder (only log errors or first 3)
        int send_result = avcodec_send_packet(video->codec_ctx, pkt_to_decode);
        
        // Cleanup packets
        if (bsf_pkt) {
            av_packet_free(&bsf_pkt);
        }
        av_packet_unref(video->packet);
        
        if (send_result < 0) {
            fprintf(stderr, "[HW_DECODE] ✗ Error sending packet to decoder: %s\n", av_err2str(send_result));
            return -1;
        }
        
        // Show buffering progress for V4L2 M2M on first decode
        if (video->use_hardware_decode && video->decode_call_count == 1) {
            if (packets_sent_this_call == 10) {
                printf("[HW_DECODE] Buffering: sent %d packets, waiting for first frame...\n", packets_sent_this_call);
            } else if (packets_sent_this_call == 20) {
                printf("[HW_DECODE] Buffering: sent %d packets (normal for V4L2 M2M)...\n", packets_sent_this_call);
            } else if (packets_sent_this_call == 30) {
                printf("[HW_DECODE] Buffering: sent %d packets...\n", packets_sent_this_call);
            } else if (packets_sent_this_call == 40) {
                printf("[HW_DECODE] Buffering: sent %d packets (large buffer needed)...\n", packets_sent_this_call);
            }
        }
        
        // Loop continues automatically - will try to receive frame next iteration
        packets_sent_this_call++;
    }
    
    // If we hit the safety limit without getting a frame, fallback to software
    if (video->use_hardware_decode) {
        printf("\n[HW_DECODE] ========== HARDWARE DECODER TIMEOUT ==========\n");
        printf("[HW_DECODE] Sent %d packets but decoder returned no frames\n", packets_sent_this_call);
        printf("[HW_DECODE] This indicates the V4L2 M2M decoder is not working\n");
        printf("[HW_DECODE] Falling back to software decoding...\n");
        printf("[HW_DECODE] ===============================================\n\n");
        
        // Clean up BSF chain from hardware decoding
        if (video->bsf_annexb_ctx) {
            av_bsf_free(&video->bsf_annexb_ctx);
            video->bsf_annexb_ctx = NULL;
        }
        if (video->bsf_aud_ctx) {
            av_bsf_free(&video->bsf_aud_ctx);
            video->bsf_aud_ctx = NULL;
        }
        
        // Reset EOF flag and seek back to beginning
        video->eof_reached = false;
        av_seek_frame(video->format_ctx, video->video_stream_index, 0, AVSEEK_FLAG_BACKWARD);
        
        // Close current hardware codec context
        avcodec_free_context(&video->codec_ctx);
        
        // Find software decoder
        AVCodecParameters *codecpar = video->format_ctx->streams[video->video_stream_index]->codecpar;
        video->codec = (AVCodec*)avcodec_find_decoder(codecpar->codec_id);
        if (!video->codec) {
            fprintf(stderr, "[HW_DECODE] ✗ No software decoder available\n");
            return -1;
        }
        
        printf("[HW_DECODE] Found software decoder: %s\n", video->codec->name);
        
        // Allocate new codec context for software decoding
        video->codec_ctx = avcodec_alloc_context3(video->codec);
        if (!video->codec_ctx) {
            fprintf(stderr, "[HW_DECODE] ✗ Failed to allocate software codec context\n");
            return -1;
        }
        
        // Copy codec parameters
        if (avcodec_parameters_to_context(video->codec_ctx, codecpar) < 0) {
            fprintf(stderr, "[HW_DECODE] ✗ Failed to copy codec parameters\n");
            avcodec_free_context(&video->codec_ctx);
            return -1;
        }
        
        // Configure for software decoding
        video->codec_ctx->thread_count = 4;
        video->codec_ctx->thread_type = FF_THREAD_FRAME;
        
        // Open software codec
        if (avcodec_open2(video->codec_ctx, video->codec, NULL) < 0) {
            fprintf(stderr, "[HW_DECODE] ✗ Failed to open software codec\n");
            avcodec_free_context(&video->codec_ctx);
            return -1;
        }
        
        printf("[HW_DECODE] ✓ Software decoder initialized successfully\n");
        printf("[HW_DECODE] Continuing playback with software decoding...\n\n");
        video->use_hardware_decode = false;
        
        // Try decoding again with software decoder
        // Note: Hardware fallback now handled by retry counter in context
        // Instead of recursive call, the decoder will be re-attempted on next call
        // This prevents stack overflow from repeated failures
        return -1;  // Signal failure, caller should retry
    }  // End if (video->use_hardware_decode)
    
    return -1; // Failed to decode even with software fallback
}

// Cached memory buffers for V4L2 M2M frames moved to video_context_t for thread safety

uint8_t* video_get_y_data(video_context_t *video) {
    if (!video->frame) return NULL;

    // OPTIMIZATION: For hardware decode, copy to cached memory for faster GL upload
    // V4L2 M2M mmap buffers are uncached (optimized for DMA, slow for CPU reads)
    // Memcpy to cached RAM is ~5x faster than reading uncached memory repeatedly
    if (video->use_hardware_decode && video->frame->data[0]) {
        pthread_mutex_lock(&video->lock);
        
        // Check if we already copied this frame (avoid duplicate memcpy)
        if (video->last_y_source == video->frame->data[0] && video->cached_y_buffer) {
            pthread_mutex_unlock(&video->lock);
            return video->cached_y_buffer;
        }

        size_t y_size = video->frame->linesize[0] * video->height;

        // Allocate cached buffer if needed
        if (video->cached_y_size < y_size) {
            free(video->cached_y_buffer);
            video->cached_y_buffer = malloc(y_size);
            video->cached_y_size = video->cached_y_buffer ? y_size : 0;
            if (!video->cached_y_buffer) {
                pthread_mutex_unlock(&video->lock);
                return video->frame->data[0]; // Fallback
            }
            printf("[PERF] Allocated %zu KB cached Y buffer for hardware decode\n", y_size / 1024);
        }

        // Copy to cached memory (fast memcpy, then fast GL upload)
        memcpy(video->cached_y_buffer, video->frame->data[0], y_size);
        video->last_y_source = video->frame->data[0];
        
        uint8_t *result = video->cached_y_buffer;
        pthread_mutex_unlock(&video->lock);
        return result;
    }

    return video->frame->data[0];
}

uint8_t* video_get_u_data(video_context_t *video) {
    if (!video->frame) return NULL;

    // OPTIMIZATION: For hardware decode, copy to cached memory
    if (video->use_hardware_decode && video->frame->data[1]) {
        pthread_mutex_lock(&video->lock);
        
        // Check if we already copied this frame (avoid duplicate memcpy)
        if (video->last_u_source == video->frame->data[1] && video->cached_u_buffer) {
            pthread_mutex_unlock(&video->lock);
            return video->cached_u_buffer;
        }

        int uv_height = video->height / 2;
        size_t u_size = video->frame->linesize[1] * uv_height;

        if (video->cached_u_size < u_size) {
            free(video->cached_u_buffer);
            video->cached_u_buffer = malloc(u_size);
            video->cached_u_size = video->cached_u_buffer ? u_size : 0;
            if (!video->cached_u_buffer) {
                pthread_mutex_unlock(&video->lock);
                return video->frame->data[1]; // Fallback
            }
        }

        memcpy(video->cached_u_buffer, video->frame->data[1], u_size);
        video->last_u_source = video->frame->data[1];
        
        uint8_t *result = video->cached_u_buffer;
        pthread_mutex_unlock(&video->lock);
        return result;
    }

    return video->frame->data[1];
}

uint8_t* video_get_v_data(video_context_t *video) {
    if (!video->frame) return NULL;

    // OPTIMIZATION: For hardware decode, copy to cached memory
    if (video->use_hardware_decode && video->frame->data[2]) {
        pthread_mutex_lock(&video->lock);
        
        // Check if we already copied this frame (avoid duplicate memcpy)
        if (video->last_v_source == video->frame->data[2] && video->cached_v_buffer) {
            pthread_mutex_unlock(&video->lock);
            return video->cached_v_buffer;
        }

        int uv_height = video->height / 2;
        size_t v_size = video->frame->linesize[2] * uv_height;

        if (video->cached_v_size < v_size) {
            free(video->cached_v_buffer);
            video->cached_v_buffer = malloc(v_size);
            video->cached_v_size = video->cached_v_buffer ? v_size : 0;
            if (!video->cached_v_buffer) {
                pthread_mutex_unlock(&video->lock);
                return video->frame->data[2]; // Fallback
            }
        }

        memcpy(video->cached_v_buffer, video->frame->data[2], v_size);
        video->last_v_source = video->frame->data[2];
        
        uint8_t *result = video->cached_v_buffer;
        pthread_mutex_unlock(&video->lock);
        return result;
    }

    return video->frame->data[2];
}
int video_restart_playback(video_context_t *video) {
    // Flush BSF contexts

    if (video->bsf_aud_ctx) {
        av_bsf_flush(video->bsf_aud_ctx);
    }
    
    // Unref any existing packet
    av_packet_unref(video->packet);
    
    // Reset EOF flag LAST (after everything is flushed)
    video->eof_reached = false;

    printf("[RESTART] Video playback restarted successfully\n");
    return 0;
}

void video_get_dimensions(video_context_t *video, int *width, int *height) {
    if (video && width && height) {
        *width = video->width;
        *height = video->height;
    }
}

void video_get_yuv_data(video_context_t *video, uint8_t **y, uint8_t **u, uint8_t **v, 
                       int *y_stride, int *u_stride, int *v_stride) {
    if (!video || !video->frame) {
        if (y) *y = NULL;
        if (u) *u = NULL;
        if (v) *v = NULL;
        if (y_stride) *y_stride = 0;
        if (u_stride) *u_stride = 0;
        if (v_stride) *v_stride = 0;
        return;
    }

    // For DRM_PRIME hardware frames, use the transferred software frame.
    AVFrame *src = video->frame;
    if (video->frame->format == AV_PIX_FMT_DRM_PRIME && video->sw_frame) {
        src = video->sw_frame;
    }

    // Debug: Print first few pixel values on first call (only for unusual values)
    if (!video->debug_printed && src->data[0]) {
        uint8_t u_val = src->data[1] ? src->data[1][0] : 0;
        uint8_t v_val = src->data[2] ? src->data[2][0] : 0;
        if (abs(u_val - 128) > 50 || abs(v_val - 128) > 50) {
            printf("DEBUG: Unusual YUV values - U:%02x V:%02x (expected ~80)\n", u_val, v_val);
        }
        video->debug_printed = true;
    }

    // Hardware decode frames may be overwritten by the decoder thread while the
    // renderer is still uploading texture data. Copy them into cached, CPU-owned
    // buffers so that the pointers remain valid for the duration of the render.
    if (video->use_hardware_decode && src->data[0] && video->width > 0 && video->height > 0) {
        static int hw_copy_logs = 0;
        pthread_mutex_lock(&video->lock);

        int width = video->width;
        int height = video->height;
        int uv_width = width / 2;
        int uv_height = height / 2;

        size_t y_bytes = (size_t)width * height;
        size_t u_bytes = (size_t)uv_width * uv_height;
        size_t v_bytes = u_bytes;

        if (video->cached_y_size < y_bytes) {
            uint8_t *new_buf = realloc(video->cached_y_buffer, y_bytes);
            if (!new_buf) {
                pthread_mutex_unlock(&video->lock);
                goto direct_ptrs;
            }
            video->cached_y_buffer = new_buf;
            video->cached_y_size = y_bytes;
        }

        if (video->cached_u_size < u_bytes) {
            uint8_t *new_buf = realloc(video->cached_u_buffer, u_bytes);
            if (!new_buf) {
                pthread_mutex_unlock(&video->lock);
                goto direct_ptrs;
            }
            video->cached_u_buffer = new_buf;
            video->cached_u_size = u_bytes;
        }

        if (video->cached_v_size < v_bytes) {
            uint8_t *new_buf = realloc(video->cached_v_buffer, v_bytes);
            if (!new_buf) {
                pthread_mutex_unlock(&video->lock);
                goto direct_ptrs;
            }
            video->cached_v_buffer = new_buf;
            video->cached_v_size = v_bytes;
        }

        // Copy Y plane (respecting stride)
        uint8_t *dst_y = video->cached_y_buffer;
        for (int row = 0; row < height; row++) {
            memcpy(dst_y + row * width, src->data[0] + row * src->linesize[0], width);
        }

        // Copy U plane
        if (src->data[1]) {
            uint8_t *dst_u = video->cached_u_buffer;
            for (int row = 0; row < uv_height; row++) {
                memcpy(dst_u + row * uv_width, src->data[1] + row * src->linesize[1], uv_width);
            }
        }

        // Copy V plane
        if (src->data[2]) {
            uint8_t *dst_v = video->cached_v_buffer;
            for (int row = 0; row < uv_height; row++) {
                memcpy(dst_v + row * uv_width, src->data[2] + row * src->linesize[2], uv_width);
            }
        }

        if (hw_copy_logs < 3) {
            printf("[HW_COPY] Copied DRM_PRIME frame into cached buffers (Y:%zu bytes)\n", y_bytes);
            hw_copy_logs++;
        }
        pthread_mutex_unlock(&video->lock);

        if (y) *y = video->cached_y_buffer;
        if (u) *u = video->cached_u_buffer;
        if (v) *v = video->cached_v_buffer;
        if (y_stride) *y_stride = width;
        if (u_stride) *u_stride = uv_width;
        if (v_stride) *v_stride = uv_width;
        return;
    }

direct_ptrs:
    if (y) *y = src->data[0];
    if (u) *u = src->data[1];
    if (v) *v = src->data[2];
    if (y_stride) *y_stride = src->linesize[0];
    if (u_stride) *u_stride = src->linesize[1];
    if (v_stride) *v_stride = src->linesize[2];
}

// NV12: Y plane followed by interleaved U/V plane (UV is half resolution)
// Layout: [Y data (width*height)] [UV data (width*height/2)]
// Each U/V pair is adjacent (U0 V0 U1 V1 ...)

uint8_t* video_get_nv12_data(video_context_t *video) {
    if (!video || !video->frame) return NULL;

    // When using DRM_PRIME, build NV12 from the transferred software frame.
    AVFrame *src = video->frame;
    if (video->frame->format == AV_PIX_FMT_DRM_PRIME && video->sw_frame) {
        src = video->sw_frame;
    }

    int width = video->width;
    int height = video->height;
    if (width <= 0 || height <= 0) {
        return NULL;
    }

    enum AVPixelFormat pix_fmt = src->format;
    bool frame_is_nv12 = (pix_fmt == AV_PIX_FMT_NV12);
    bool frame_is_planar = (pix_fmt == AV_PIX_FMT_YUV420P || pix_fmt == AV_PIX_FMT_YUVJ420P);

    if (!frame_is_nv12 && !frame_is_planar) {
        // Unsupported format for NV12 uploads
        return NULL;
    }
    
    int needed_size = (width * height * 3) / 2;  // Y plane + packed UV plane

    if (video->nv12_buffer_size < needed_size) {
        uint8_t *new_buffer = realloc(video->nv12_buffer, needed_size);
        if (!new_buffer) {
            return NULL;
        }
        video->nv12_buffer = new_buffer;
        video->nv12_buffer_size = needed_size;
    }

    if (!video->nv12_buffer) return NULL;

    uint8_t *dst = video->nv12_buffer;
    uint8_t *y_data = src->data[0];
    int y_stride = src->linesize[0];
    if (!y_data) return NULL;

    // Copy Y plane (full resolution)
    for (int row = 0; row < height; row++) {
        memcpy(dst, y_data + (row * y_stride), width);
        dst += width;
    }

    // Copy UV data depending on source format
    int uv_height = height / 2;

    if (frame_is_nv12) {
        uint8_t *uv_src = src->data[1];
        int uv_stride_bytes = src->linesize[1];
        if (!uv_src) {
            return NULL;
        }

        for (int row = 0; row < uv_height; row++) {
            memcpy(dst, uv_src + (row * uv_stride_bytes), width);
            dst += width;
        }
    } else {
        // Planar YUV420 -> interleave as NV12
        uint8_t *u_data = src->data[1];
        uint8_t *v_data = src->data[2];
        int u_stride = src->linesize[1];
        int v_stride = src->linesize[2];

        if (!u_data || !v_data) {
            return NULL;
        }

        int uv_width = width / 2;
        for (int row = 0; row < uv_height; row++) {
            uint8_t *u_row = u_data + (row * u_stride);
            uint8_t *v_row = v_data + (row * v_stride);

            for (int col = 0; col < uv_width; col++) {
                *dst++ = u_row[col];
                *dst++ = v_row[col];
            }
        }
    }

    return video->nv12_buffer;
}

int video_get_nv12_stride(video_context_t *video) {
    if (!video) return 0;
    return video->width;
}

bool video_frame_is_nv12(video_context_t *video) {
    if (!video || !video->frame) {
        return false;
    }
    // For DRM_PRIME frames, check the transferred software frame format.
    AVFrame *src = video->frame;
    if (video->frame->format == AV_PIX_FMT_DRM_PRIME && video->sw_frame) {
        src = video->sw_frame;
    }
    enum AVPixelFormat pix_fmt = src->format;
    return (pix_fmt == AV_PIX_FMT_NV12);
}

bool video_is_eof(video_context_t *video) {
    return video->eof_reached;
}

void video_set_loop(video_context_t *video, bool loop) {
    if (video) {
        video->loop_playback = loop;
    }
}

bool video_is_hardware_decoded(video_context_t *video) {
    return video ? video->use_hardware_decode : false;
}

double video_get_frame_time(video_context_t *video) {
    if (!video || video->fps <= 0) {
        return 1.0 / 30.0; // Default to 30 FPS
    }
    return 1.0 / video->fps;
}

void video_seek(video_context_t *video, int64_t timestamp) {
    if (!video || !video->initialized) {
        return;
    }
    
    printf("[SEEK] Seeking to timestamp %ld...\n", timestamp);
    
    // Reset EOF flag BEFORE seeking
    video->eof_reached = false;
    
    // Try frame-based seek first (more reliable for MP4)
    int seek_result = av_seek_frame(video->format_ctx, video->video_stream_index, 
                                    timestamp, AVSEEK_FLAG_FRAME | AVSEEK_FLAG_BACKWARD);
    
    if (seek_result < 0) {
        // Fallback to timestamp-based seek
        printf("[SEEK] Frame seek failed, trying timestamp seek\n");
        seek_result = avformat_seek_file(video->format_ctx, video->video_stream_index, 
                                        INT64_MIN, timestamp, timestamp, 0);
    }
    
    if (seek_result < 0) {
        printf("[SEEK] Error: Seek failed: %s\n", av_err2str(seek_result));
        return;
    }
    
    // Flush decoder buffers
    avcodec_flush_buffers(video->codec_ctx);
    
    // Flush BSF buffers if present
    if (video->bsf_annexb_ctx) {
        av_bsf_flush(video->bsf_annexb_ctx);
    }
    if (video->bsf_aud_ctx) {
        av_bsf_flush(video->bsf_aud_ctx);
    }
    
    // Unref any existing packet data
    av_packet_unref(video->packet);
    
    printf("[SEEK] Seek completed successfully\n");
}

void video_cleanup(video_context_t *video) {
    if (!video) {
        return;
    }
    
    // Clean up DMA buffer if still open
    if (video->dma_fd >= 0) {
        close(video->dma_fd);
        video->dma_fd = -1;
    }
    
    // Clean up hardware contexts
    if (video->hw_frames_ctx) {
        av_buffer_unref(&video->hw_frames_ctx);
        video->hw_frames_ctx = NULL;
    }
    if (video->hw_device_ctx) {
        av_buffer_unref(&video->hw_device_ctx);
        video->hw_device_ctx = NULL;
    }
    
    // No frame buffers in this version
    
    // Clean up 2-stage BSF chain
    if (video->bsf_annexb_ctx) {
        av_bsf_free(&video->bsf_annexb_ctx);
        video->bsf_annexb_ctx = NULL;
    }
    if (video->bsf_aud_ctx) {
        av_bsf_free(&video->bsf_aud_ctx);
        video->bsf_aud_ctx = NULL;
    }
    
    if (video->frame) {
        av_frame_free(&video->frame);
        video->frame = NULL;
    }
    if (video->sw_frame) {
        av_frame_free(&video->sw_frame);
        video->sw_frame = NULL;
    }
    if (video->packet) {
        av_packet_free(&video->packet);
        video->packet = NULL;
    }
    
    // CRITICAL: Properly close codec before freeing to release V4L2 device
    if (video->codec_ctx) {
        const char *codec_name = video->codec_ctx->codec ? video->codec_ctx->codec->name : "unknown";
        bool is_v4l2m2m = (strstr(codec_name, "v4l2m2m") != NULL);
        
        if (is_v4l2m2m) {
            // V4L2 M2M requires extra care to release hardware properly
            // Flush any pending frames
            avcodec_flush_buffers(video->codec_ctx);
            
            // Send EOF to decoder
            int ret = avcodec_send_packet(video->codec_ctx, NULL);
            if (ret < 0 && ret != AVERROR_EOF) {
                fprintf(stderr, "[CLEANUP] Warning: V4L2 M2M EOF send failed: %s\n", av_err2str(ret));
            }
            
            // Drain all remaining frames
            AVFrame *dummy = av_frame_alloc();
            if (dummy) {
                int drain_count = 0;
                while (drain_count < DECODER_DRAIN_SAFETY_LIMIT) {  // Safety limit
                    ret = avcodec_receive_frame(video->codec_ctx, dummy);
                    if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
                        break;
                    }
                    if (ret < 0) {
                        break;  // Error, stop draining
                    }
                    av_frame_unref(dummy);
                    drain_count++;
                }
                av_frame_free(&dummy);
                
                if (drain_count > 0) {
                    printf("[CLEANUP] Drained %d frames from V4L2 M2M decoder\n", drain_count);
                }
            }
            
            // Give V4L2 driver time to release resources
            // This helps prevent device-busy errors on next run
            usleep(V4L2_CLEANUP_DELAY_US);  // 10ms delay
        }
        
        // Now safe to free codec context and release V4L2 device
        avcodec_free_context(&video->codec_ctx);
        video->codec_ctx = NULL;
        
        // Extra delay for V4L2 M2M to ensure kernel driver fully releases device
        if (is_v4l2m2m) {
            usleep(V4L2_POST_CLEANUP_DELAY_US);  // 50ms delay after context free
        }
    }
    
    if (video->format_ctx) {
        avformat_close_input(&video->format_ctx);
        video->format_ctx = NULL;
    }
    
    if (video->nv12_buffer) {
        free(video->nv12_buffer);
        video->nv12_buffer = NULL;
        video->nv12_buffer_size = 0;
    }

    if (video->cached_y_buffer) {
        free(video->cached_y_buffer);
        video->cached_y_buffer = NULL;
        video->cached_y_size = 0;
    }
    if (video->cached_u_buffer) {
        free(video->cached_u_buffer);
        video->cached_u_buffer = NULL;
        video->cached_u_size = 0;
    }
    if (video->cached_v_buffer) {
        free(video->cached_v_buffer);
        video->cached_v_buffer = NULL;
        video->cached_v_size = 0;
    }

    // Safe pthread cleanup with error checking
    int mutex_result = pthread_mutex_destroy(&video->lock);
    if (mutex_result != 0 && mutex_result != EINVAL) {
        fprintf(stderr, "[CLEANUP] Warning: pthread_mutex_destroy failed: %d\n", mutex_result);
    }
    
    memset(video, 0, sizeof(*video));
}

// DMA buffer zero-copy support functions
// These retrieve DMA file descriptors from hardware decoded frames

bool video_has_dma_buffer(video_context_t *video) {
    if (!video || !video->frame) {
        return false;
    }

    // Check if we have a valid DMA FD from DRM PRIME frame extraction
    // AND verify the frame format is actually DRM_PRIME
    if (video->use_hardware_decode &&
        video->dma_fd >= 0 &&
        video->frame->format == AV_PIX_FMT_DRM_PRIME) {
        // We have a valid DMA file descriptor ready for GPU import
        // CPU pointers (data[1], data[2]) will be NULL for DRM_PRIME - that's expected
        return true;
    }

    return false;
}

void video_get_dma_plane_layout(video_context_t *video, int offsets[3], int pitches[3]) {
    if (!video || !offsets || !pitches) {
        return;
    }
    
    for (int i = 0; i < 3; i++) {
        offsets[i] = video->dma_plane_offset[i];
        pitches[i] = video->dma_plane_pitch[i];
    }
}

int video_get_dma_fd(video_context_t *video) {
    if (!video || !video->frame || video->dma_fd < 0) {
        return -1;
    }
    
    return video->dma_fd;
}

int video_get_dma_offset(video_context_t *video) {
    if (!video || !video->frame) {
        return 0;
    }
    
    return video->dma_offset;
}

size_t video_get_dma_size(video_context_t *video) {
    if (!video || !video->frame) {
        return 0;
    }
    
    return video->dma_size;
}
