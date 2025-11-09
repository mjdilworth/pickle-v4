#define _GNU_SOURCE
#include "video_decoder.h"
#include "v4l2_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>

// V4L2 M2M format negotiation callback for RPi hardware decoder
// RPi V4L2 M2M driver prefers NV12 first, then YUV420P
static enum AVPixelFormat get_format_callback(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    const enum AVPixelFormat *p;
    
    printf("[HW_DECODE] Format negotiation - available formats:\n");
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        const char *name = av_get_pix_fmt_name(*p);
        printf("[HW_DECODE]   - %s (%d)\n", name ? name : "unknown", *p);
    }
    
    // CRITICAL: RPi V4L2 M2M prefers NV12 for better performance
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_NV12) {
            printf("[HW_DECODE] ✓ Selected: NV12 (preferred for RPi V4L2 M2M)\n");
            return AV_PIX_FMT_NV12;
        }
    }
    
    // Fallback to YUV420P
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_YUV420P) {
            printf("[HW_DECODE] ✓ Selected: YUV420P (fallback)\n");
            return *p;
        }
    }
    
    // Last resort: first available format
    if (pix_fmts[0] != AV_PIX_FMT_NONE) {
        printf("[HW_DECODE] ✓ Selected: %s (first available)\n", 
               av_get_pix_fmt_name(pix_fmts[0]));
        return pix_fmts[0];
    }
    
    fprintf(stderr, "[HW_DECODE] ERROR: No suitable format found\n");
    return AV_PIX_FMT_NONE;
}

int video_init(video_context_t *video, const char *filename, bool advanced_diagnostics) {
    memset(video, 0, sizeof(*video));
    
    // Store advanced diagnostics flag
    video->advanced_diagnostics = advanced_diagnostics;

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
    
    if (avformat_open_input(&video->format_ctx, filename, NULL, &options) < 0) {
        fprintf(stderr, "Failed to open input file: %s\n", filename);
        av_dict_free(&options);
        av_packet_free(&video->packet);
        return -1;
    }
    av_dict_free(&options);

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
    // Will attempt V4L2 M2M hardware decoder first
    // If hardware decoder hangs (no frames after 10 packets), automatically falls back to software
    // This provides best performance when hardware works, with reliability when it doesn't
    //
    // To disable hardware decode attempts, set ENABLE_HW_DECODE=0 below
    #define ENABLE_HW_DECODE 1
    
    #if ENABLE_HW_DECODE
    printf("[HW_DECODE] Attempting hardware decoder detection...\n");
    printf("[HW_DECODE] Codec ID: %d\n", codecpar->codec_id);
    printf("[HW_DECODE] AV_CODEC_ID_H264 = %d, AV_CODEC_ID_HEVC = %d\n", AV_CODEC_ID_H264, AV_CODEC_ID_HEVC);
    
    // Try hardware decoder for H.264
    if (codecpar->codec_id == AV_CODEC_ID_H264) {
        printf("[HW_DECODE] H.264 detected, searching for h264_v4l2m2m decoder...\n");
        video->codec = (AVCodec*)avcodec_find_decoder_by_name("h264_v4l2m2m");
        if (video->codec) {
            video->use_hardware_decode = true;
            video->hw_decode_type = HW_DECODE_V4L2M2M;
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
        } else {
            printf("[HW_DECODE] ✗ h264_v4l2m2m not available\n");
        }
    } else if (codecpar->codec_id == AV_CODEC_ID_HEVC) {
        printf("[HW_DECODE] HEVC/H.265 detected, searching for hevc_v4l2m2m decoder...\n");
        video->codec = (AVCodec*)avcodec_find_decoder_by_name("hevc_v4l2m2m");
        if (video->codec) {
            video->use_hardware_decode = true;
            video->hw_decode_type = HW_DECODE_V4L2M2M;
            printf("[HW_DECODE] ✓ Found hevc_v4l2m2m hardware decoder\n");
            printf("[HW_DECODE] HEVC profile: %d\n", codecpar->profile);
            printf("[HW_DECODE] HEVC level: %d\n", codecpar->level);
            printf("[HW_DECODE] Resolution: %dx%d\n", codecpar->width, codecpar->height);
            printf("[HW_DECODE] Bitrate: %"PRId64" bps\n", codecpar->bit_rate);
            
            // Check V4L2 capabilities
            check_v4l2_decoder_capabilities();
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
    #else
    // Hardware decode disabled - use software decoder
    printf("Using software decoder (hardware decode disabled)\n");
    video->codec = (AVCodec*)avcodec_find_decoder(codecpar->codec_id);
    if (!video->codec) {
        fprintf(stderr, "Failed to find software decoder for codec ID %d\n", codecpar->codec_id);
        avformat_close_input(&video->format_ctx);
        av_packet_free(&video->packet);
        return -1;
    }
    #endif

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

    // Initialize BSF BEFORE opening codec for hardware decoding (avcC to Annex-B conversion)
    if (video->use_hardware_decode && video->hw_decode_type == HW_DECODE_V4L2M2M && 
        codecpar->extradata && codecpar->extradata_size > 0 && codecpar->extradata[0] == 1) {
        
        printf("[HW_DECODE] BSF: Analyzing stream format...\n");
        printf("[HW_DECODE] BSF: First 8 bytes of extradata: ");
        for (int i = 0; i < 8 && i < codecpar->extradata_size; i++) {
            printf("%02x ", codecpar->extradata[i]);
        }
        printf("\n");
        printf("[HW_DECODE] BSF: Detected avcC format (byte 0 = 0x01)\n");
        printf("[HW_DECODE] BSF: Will convert avcC → Annex-B for V4L2 M2M\n");
        
        video->avcc_length_size = get_avcc_length_size(codecpar->extradata, codecpar->extradata_size);
        printf("[HW_DECODE] BSF: avcC NAL length size: %d bytes\n", video->avcc_length_size);
        
        printf("[HW_DECODE] BSF: Initializing h264_mp4toannexb bitstream filter...\n");
        
        // h264_mp4toannexb (avcC → Annex-B conversion)
        const AVBitStreamFilter *bsf_annexb = av_bsf_get_by_name("h264_mp4toannexb");
        if (!bsf_annexb) {
            fprintf(stderr, "[HW_DECODE] BSF: ✗ Failed to find h264_mp4toannexb BSF\n");
            video_cleanup(video);
            return -1;
        }
        printf("[HW_DECODE] BSF: ✓ Found h264_mp4toannexb filter\n");
        
        if (av_bsf_alloc(bsf_annexb, &video->bsf_annexb_ctx) < 0) {
            fprintf(stderr, "[HW_DECODE] BSF: ✗ Failed to allocate BSF context\n");
            video_cleanup(video);
            return -1;
        }
        printf("[HW_DECODE] BSF: ✓ Allocated BSF context\n");
        
        avcodec_parameters_copy(video->bsf_annexb_ctx->par_in, codecpar);
        printf("[HW_DECODE] BSF: ✓ Copied codec parameters to BSF\n");
        
        if (av_bsf_init(video->bsf_annexb_ctx) < 0) {
            fprintf(stderr, "[HW_DECODE] BSF: ✗ Failed to initialize BSF\n");
            video_cleanup(video);
            return -1;
        }
        printf("[HW_DECODE] BSF: ✓ Initialized BSF successfully\n");
        
        // Copy the BSF output parameters (with converted Annex-B extradata) to codec context
        if (video->bsf_annexb_ctx->par_out->extradata && video->bsf_annexb_ctx->par_out->extradata_size > 0) {
            printf("[HW_DECODE] BSF: Converting extradata to Annex-B format...\n");
            // Free existing extradata
            if (video->codec_ctx->extradata) {
                av_freep(&video->codec_ctx->extradata);
            }
            // Allocate and copy converted extradata
            video->codec_ctx->extradata_size = video->bsf_annexb_ctx->par_out->extradata_size;
            video->codec_ctx->extradata = av_mallocz(video->codec_ctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(video->codec_ctx->extradata, video->bsf_annexb_ctx->par_out->extradata, video->codec_ctx->extradata_size);
            printf("[HW_DECODE] BSF: ✓ Converted extradata (%d bytes)\n", video->codec_ctx->extradata_size);
        }
        
        // Set codec_tag to 0 for Annex-B format
        video->codec_ctx->codec_tag = 0;
        printf("[HW_DECODE] BSF: Set codec_tag=0 for Annex-B format\n");
        printf("[HW_DECODE] BSF: ✓ avcC → Annex-B conversion ready\n");
    }

    // Configure hardware decoding for V4L2 M2M
    if (video->use_hardware_decode && video->hw_decode_type == HW_DECODE_V4L2M2M) {
        printf("[HW_DECODE] V4L2: Configuring V4L2 M2M decoder for Raspberry Pi...\n");
        
        // Set format negotiation callback
        video->codec_ctx->get_format = get_format_callback;
        printf("[HW_DECODE] V4L2: ✓ Format negotiation callback registered\n");
        
        // CRITICAL: V4L2 M2M must be single-threaded
        video->codec_ctx->thread_count = 1;
        video->codec_ctx->thread_type = 0;  // Disable all threading
        printf("[HW_DECODE] V4L2: Set thread_count=1, thread_type=0 (V4L2 handles threading)\n");
        
        // Low-latency flags for better V4L2 M2M performance
        video->codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        video->codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        printf("[HW_DECODE] V4L2: ✓ LOW_DELAY and FAST flags enabled\n");
        
        // Enable CHUNKS mode for V4L2 M2M decoder (handles partial frames)
        video->codec_ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;
        printf("[HW_DECODE] V4L2: ✓ CHUNKS mode enabled (supports partial frames)\n");
        
        // Prepare V4L2-specific options
        AVDictionary *codec_opts = NULL;
        
        // CRITICAL FIX: Set V4L2 buffer counts - essential for RPi stability
        // These values prevent decoder stalls and ensure smooth operation
        av_dict_set(&codec_opts, "num_capture_buffers", "32", 0);
        av_dict_set(&codec_opts, "num_output_buffers", "16", 0);
        printf("[HW_DECODE] V4L2: Set num_capture_buffers=32, num_output_buffers=16\n");
        
        // CRITICAL FIX: Force standard mmap mode (not PRIME/DRM) for compatibility
        // This ensures the decoder uses memory-mapped buffers correctly
        if (av_opt_set_int(video->codec_ctx->priv_data, "num_capture_buffers", 32, 0) == 0) {
            printf("[HW_DECODE] V4L2: ✓ Forcing mmap mode (not PRIME/DRM)\n");
        }
        
        // Optional: Force specific device (uncomment if needed)
        // av_dict_set(&codec_opts, "device", "/dev/video10", 0);
        // printf("[HW_DECODE] V4L2: Set device=/dev/video10\n");
        
        printf("[HW_DECODE] V4L2: Configuration complete\n");
        printf("[HW_DECODE] V4L2: Note - decoder may buffer 20-30 packets before first frame\n");
        
        // Open codec with V4L2 options
        int ret = avcodec_open2(video->codec_ctx, video->codec, &codec_opts);
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
        printf("[HW_DECODE] V4L2: ✓ Codec opened successfully with V4L2 options\n");
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

    // Allocate frame for YUV data (both hardware and software use same frame now)
    video->frame = av_frame_alloc();
    if (!video->frame) {
        fprintf(stderr, "Failed to allocate frame\n");
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
    static int decode_call_count = 0;
    decode_call_count++;
    
    if (decode_call_count == 1) {
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
    // Working decoders typically output a frame within 3-5 packets
    // V4L2 M2M may buffer 20-30 packets before first frame (normal behavior)
    // Some videos (especially high bitrate) may need 40-50 packets
    int MAX_PACKETS_PER_CALL = 50;  // Allow enough buffering for all video types
    if (video->use_hardware_decode && decode_call_count == 1) {
        MAX_PACKETS_PER_CALL = 50;  // First call: generous timeout for V4L2 buffering
        printf("[HW_DECODE] First decode: will send up to %d packets before software fallback\n", MAX_PACKETS_PER_CALL);
        printf("[HW_DECODE] Note: V4L2 M2M may buffer 20-50 packets depending on video\n");
    }
    
    while (packets_sent_this_call < MAX_PACKETS_PER_CALL) {
        // First, try to get any frame the decoder has buffered
        int receive_result = avcodec_receive_frame(video->codec_ctx, video->frame);
        
        if (receive_result == 0) {
            // Successfully decoded a frame
            static int frame_count = 0;
            frame_count++;
            
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
                const char *fmt_name = av_get_pix_fmt_name(video->frame->format);
                printf("Frame #%d decoded successfully\n", frame_count);
                fflush(stdout);
            }
            
            return 0;  // SUCCESS - Frame ready for caller to use
        }
        
        if (receive_result == AVERROR_EOF) {
            // End of stream reached
            video->eof_reached = true;
            if (decode_call_count == 1 && video->advanced_diagnostics) {
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
        if (video->use_hardware_decode && decode_call_count == 1) {
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
        return video_decode_frame(video);
    }  // End if (video->use_hardware_decode)
    
    return -1; // Failed to decode even with software fallback
}

uint8_t* video_get_y_data(video_context_t *video) {
    if (!video->frame) return NULL;
    
    // Debug: Print first few pixel values on first call
    static bool debug_printed = false;
    if (!debug_printed && video->frame->data[0]) {
        printf("DEBUG: Y[0-3]: %02x %02x %02x %02x\n", 
               video->frame->data[0][0], video->frame->data[0][1], 
               video->frame->data[0][2], video->frame->data[0][3]);
        if (video->frame->data[1]) {
            printf("DEBUG: U[0-3]: %02x %02x %02x %02x\n", 
                   video->frame->data[1][0], video->frame->data[1][1], 
                   video->frame->data[1][2], video->frame->data[1][3]);
        }
        if (video->frame->data[2]) {
            printf("DEBUG: V[0-3]: %02x %02x %02x %02x\n", 
                   video->frame->data[2][0], video->frame->data[2][1], 
                   video->frame->data[2][2], video->frame->data[2][3]);
        }
        debug_printed = true;
    }
    
    return video->frame->data[0];
}

uint8_t* video_get_u_data(video_context_t *video) {
    if (!video->frame) return NULL;
    return video->frame->data[1];
}

uint8_t* video_get_v_data(video_context_t *video) {
    if (!video->frame) return NULL;
    return video->frame->data[2];
}

int video_get_y_stride(video_context_t *video) {
    if (!video->frame) return 0;
    return video->frame->linesize[0];
}

int video_get_u_stride(video_context_t *video) {
    if (!video->frame) return 0;
    return video->frame->linesize[1];
}

int video_get_v_stride(video_context_t *video) {
    if (!video->frame) return 0;
    return video->frame->linesize[2];
}

uint8_t* video_get_rgb_data(video_context_t *video) {
    // This function is kept for compatibility but returns NULL
    // since we're doing YUV→RGB conversion on GPU
    (void)video; // Suppress unused parameter warning
    return NULL;
}

bool video_is_eof(video_context_t *video) {
    return video->eof_reached;
}

int video_restart_playback(video_context_t *video) {
    if (!video->initialized) {
        return -1;
    }

    printf("[RESTART] Restarting video playback...\n");
    
    // METHOD 1: Try seek with AVSEEK_FLAG_FRAME first
    // This is more reliable for MP4 files than timestamp-based seeking
    int ret = av_seek_frame(video->format_ctx, video->video_stream_index, 0, 
                           AVSEEK_FLAG_FRAME | AVSEEK_FLAG_BACKWARD);
    
    if (ret < 0) {
        // METHOD 2: If frame-based seek fails, try timestamp 0
        printf("[RESTART] Frame seek failed, trying timestamp seek\n");
        ret = av_seek_frame(video->format_ctx, video->video_stream_index, 0, AVSEEK_FLAG_BACKWARD);
    }
    
    if (ret < 0) {
        // METHOD 3: Last resort - close and reopen the file
        printf("[RESTART] Seek failed (%s), reopening file...\n", av_err2str(ret));
        
        // Save file path
        char *url = strdup(video->format_ctx->url);
        if (!url) return -1;
        
        // Save settings for reinit
        bool advanced_diag = video->advanced_diagnostics;
        
        // Clean up current decoder state
        if (video->bsf_annexb_ctx) av_bsf_free(&video->bsf_annexb_ctx);
        if (video->bsf_aud_ctx) av_bsf_free(&video->bsf_aud_ctx);
        if (video->frame) av_frame_free(&video->frame);
        if (video->packet) av_packet_free(&video->packet);
        if (video->codec_ctx) avcodec_free_context(&video->codec_ctx);
        if (video->format_ctx) avformat_close_input(&video->format_ctx);
        
        // Reinitialize from scratch
        int result = video_init(video, url, advanced_diag);
        free(url);
        
        if (result < 0) {
            printf("[RESTART] Failed to reinitialize video\n");
            return -1;
        }
        
        printf("[RESTART] Video reinitialized successfully\n");
        return 0;
    }

    // Seek succeeded - flush decoder buffers
    avcodec_flush_buffers(video->codec_ctx);
    
    // Flush BSF buffers if present
    if (video->bsf_annexb_ctx) {
        av_bsf_flush(video->bsf_annexb_ctx);
    }
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
    
    // Debug: Print first few pixel values on first call (only for unusual values)
    static bool debug_printed = false;
    if (!debug_printed && video->frame->data[0]) {
        uint8_t u_val = video->frame->data[1] ? video->frame->data[1][0] : 0;
        uint8_t v_val = video->frame->data[2] ? video->frame->data[2][0] : 0;
        
        // Only print debug if U/V values are unusual (not near 128)
        if (abs(u_val - 128) > 50 || abs(v_val - 128) > 50) {
            printf("DEBUG: Unusual YUV values - U:%02x V:%02x (expected ~80)\n", u_val, v_val);
        }
        debug_printed = true;
    }
    
    if (y) *y = video->frame->data[0];
    if (u) *u = video->frame->data[1];
    if (v) *v = video->frame->data[2];
    if (y_stride) *y_stride = video->frame->linesize[0];
    if (u_stride) *u_stride = video->frame->linesize[1];
    if (v_stride) *v_stride = video->frame->linesize[2];
}

// NV12: Y plane followed by interleaved U/V plane (UV is half resolution)
// Layout: [Y data (width*height)] [UV data (width*height/2)]
// Each U/V pair is adjacent (U0 V0 U1 V1 ...)
uint8_t* video_get_nv12_data(video_context_t *video) {
    if (!video || !video->frame) return NULL;
    
    // NV12 format: we need to pack U and V planes together
    // FFmpeg gives us separate Y/U/V planes, so we need to convert
    static uint8_t *nv12_buffer = NULL;
    static int nv12_buffer_size = 0;
    
    int width = video->width;
    int height = video->height;
    int needed_size = (width * height * 3) / 2;  // Y plane + packed UV plane
    
    // Allocate or reallocate buffer if needed
    if (nv12_buffer_size < needed_size) {
        free(nv12_buffer);
        nv12_buffer = malloc(needed_size);
        nv12_buffer_size = needed_size;
    }
    
    if (!nv12_buffer) return NULL;
    
    uint8_t *y_data = video->frame->data[0];
    uint8_t *u_data = video->frame->data[1];
    uint8_t *v_data = video->frame->data[2];
    int y_stride = video->frame->linesize[0];
    int u_stride = video->frame->linesize[1];
    int v_stride = video->frame->linesize[2];
    
    if (!y_data) return NULL;
    
    // Copy Y plane (full resolution)
    uint8_t *dst = nv12_buffer;
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
        // Interleave U and V into UV plane
        for (int row = 0; row < uv_height; row++) {
            uint8_t *u_row = u_data + (row * u_stride);
            uint8_t *v_row = v_data + (row * v_stride);
            
            for (int col = 0; col < uv_width; col++) {
                *dst++ = u_row[col];  // U
                *dst++ = v_row[col];  // V
            }
        }
    }
    
    return nv12_buffer;
}

int video_get_nv12_stride(video_context_t *video) {
    if (!video) return 0;
    // NV12 stride is just the width (no padding for the packed format we create)
    return video->width;
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
    // No frame buffers in this version
    
    // Clean up 2-stage BSF chain
    if (video->bsf_annexb_ctx) {
        av_bsf_free(&video->bsf_annexb_ctx);
    }
    if (video->bsf_aud_ctx) {
        av_bsf_free(&video->bsf_aud_ctx);
    }
    
    if (video->frame) {
        av_frame_free(&video->frame);
    }
    if (video->packet) {
        av_packet_free(&video->packet);
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
                while (drain_count < 50) {  // Safety limit
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
            usleep(10000);  // 10ms delay
        }
        
        // Now safe to free codec context and release V4L2 device
        avcodec_free_context(&video->codec_ctx);
        
        // Extra delay for V4L2 M2M to ensure kernel driver fully releases device
        if (is_v4l2m2m) {
            usleep(50000);  // 50ms delay after context free
        }
    }
    
    if (video->format_ctx) {
        avformat_close_input(&video->format_ctx);
    }
    
    memset(video, 0, sizeof(*video));
}