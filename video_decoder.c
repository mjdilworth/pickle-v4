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
    
    // Try hardware decoder for H.264
    if (codecpar->codec_id == AV_CODEC_ID_H264) {
        video->codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
        if (video->codec) {
            video->use_hardware_decode = true;
            video->hw_decode_type = HW_DECODE_V4L2M2M;
            printf("Using V4L2 M2M hardware decoder for H.264 (profile: %d)\n", codecpar->profile);
            
            // Run diagnostics for V4L2 hardware decoder capabilities
            printf("\n===== V4L2 Hardware Decoder Diagnostics =====\n");
            printf("* Detected H.264 stream, using v4l2m2m hardware decoder\n");
            printf("* H.264 profile: %d (%s)\n", codecpar->profile, 
                   codecpar->profile == 66 ? "Baseline" :
                   codecpar->profile == 77 ? "Main" :
                   codecpar->profile == 100 ? "High" : "Other");
            printf("* H.264 level: %d\n", codecpar->level);
            printf("* Resolution: %dx%d\n", codecpar->width, codecpar->height);
            printf("* Bitrate: %"PRId64" bps\n", codecpar->bit_rate);
            
            // Checking hardware decoder capabilities
            check_v4l2_decoder_capabilities();
        }
    } else if (codecpar->codec_id == AV_CODEC_ID_HEVC) {
        video->codec = avcodec_find_decoder_by_name("hevc_v4l2m2m");
        if (video->codec) {
            video->use_hardware_decode = true;
            video->hw_decode_type = HW_DECODE_V4L2M2M;
            printf("Using V4L2 M2M hardware decoder for H.265\n");
            
            // Run diagnostics for V4L2 hardware decoder capabilities
            printf("\n===== V4L2 Hardware Decoder Diagnostics =====\n");
            printf("* Detected HEVC (H.265) stream, using v4l2m2m hardware decoder\n");
            printf("* HEVC profile: %d\n", codecpar->profile);
            printf("* HEVC level: %d\n", codecpar->level);
            printf("* Resolution: %dx%d\n", codecpar->width, codecpar->height);
            printf("* Bitrate: %"PRId64" bps\n", codecpar->bit_rate);
            
            // Checking hardware decoder capabilities
            check_v4l2_decoder_capabilities();
        }
    }
    
    // Fall back to software decoder if hardware not available
    if (!video->codec) {
        printf("Hardware decoder not available, falling back to software\n");
        video->codec = avcodec_find_decoder(codecpar->codec_id);
        if (!video->codec) {
            fprintf(stderr, "Unsupported codec\n");
            avformat_close_input(&video->format_ctx);
            av_packet_free(&video->packet);
            return -1;
        }
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

    // Configure hardware decoding for V4L2 M2M
    if (video->use_hardware_decode && video->hw_decode_type == HW_DECODE_V4L2M2M) {
        // Don't force pixel format - let the decoder choose the best format
        video->codec_ctx->thread_count = 1; // V4L2 handles threading internally
        printf("V4L2 M2M configured - will auto-detect output format\n");
        
        // CRITICAL FIX: Convert avcC extradata to Annex-B for V4L2 M2M compatibility
        if (codecpar->extradata && codecpar->extradata_size > 0) {
            printf("V4L2 stream analysis: First 8 bytes: ");
            for (int i = 0; i < 8 && i < codecpar->extradata_size; i++) {
                printf("%02x ", codecpar->extradata[i]);
            }
            printf("\n");
            
            // Check if this is avcC format (first byte == 1)
            if (codecpar->extradata[0] == 1) {
                printf("[INFO]  V4L2 stream: Detected avcC format - converting to Annex-B\n");
                
                uint8_t *annexb_extradata = NULL;
                size_t annexb_size = 0;
                
                if (avcc_extradata_to_annexb(codecpar->extradata, codecpar->extradata_size, 
                                           &annexb_extradata, &annexb_size) == 0) {
                    // Replace the original extradata with Annex-B version
                    av_freep(&video->codec_ctx->extradata);
                    video->codec_ctx->extradata = av_malloc(annexb_size + AV_INPUT_BUFFER_PADDING_SIZE);
                    if (video->codec_ctx->extradata) {
                        memcpy(video->codec_ctx->extradata, annexb_extradata, annexb_size);
                        memset(video->codec_ctx->extradata + annexb_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                        video->codec_ctx->extradata_size = annexb_size;
                        printf("[INFO]  V4L2 integration: Successfully converted extradata to Annex-B (%zu bytes)\n", annexb_size);
                        
                        // Store length size for later use in packet conversion
                        video->avcc_length_size = get_avcc_length_size(codecpar->extradata, codecpar->extradata_size);
                        printf("[INFO]  V4L2 integration: NAL length size: %d bytes\n", video->avcc_length_size);
                    } else {
                        printf("[WARN]  V4L2 integration: Failed to allocate memory for converted extradata\n");
                    }
                    free(annexb_extradata);
                } else {
                    printf("[WARN]  V4L2 stream: Failed to convert avcC to Annex-B - may not be compatible with V4L2 decoder\n");
                }
            } else {
                printf("[INFO]  V4L2 stream: Not avcC format - assuming already Annex-B compatible\n");
            }
        } else {
            printf("[INFO]  V4L2 stream: No extradata found\n");
        }
    } else {
        // Software decoding settings
        video->codec_ctx->thread_count = 4;
        video->codec_ctx->thread_type = FF_THREAD_FRAME;
    }

    // Open codec
    if (avcodec_open2(video->codec_ctx, video->codec, NULL) < 0) {
        fprintf(stderr, "Failed to open codec\n");
        avcodec_free_context(&video->codec_ctx);
        avformat_close_input(&video->format_ctx);
        av_packet_free(&video->packet);
        return -1;
    }

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
    video->fps = (double)stream->r_frame_rate.num / stream->r_frame_rate.den;
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

    video->initialized = true;
    video->eof_reached = false;

    // Video decoder initialization complete
    return 0;
}

int video_decode_frame(video_context_t *video) {
    static int decode_call_count = 0;
    decode_call_count++;
    
    if (decode_call_count <= 5) {
        printf("video_decode_frame() call #%d\n", decode_call_count);
        fflush(stdout);
    }
    
    if (!video->initialized || video->eof_reached) {
        return -1;
    }

    // OPTIMIZED: Limit packets per frame to prevent spikes
    static int max_packets = 5; // Start conservative to prevent blocking
    static int consecutive_fails = 0;
    int packets_processed = 0;
    
    // Try to receive a frame first (decoder might have buffered data)
    int receive_result = avcodec_receive_frame(video->codec_ctx, video->frame);
    if (receive_result == 0) {
        // Successfully got a frame from buffer
        if (decode_call_count <= 5) {
            printf("Got buffered frame from decoder\n");
            fflush(stdout);
        }
        return 0;
    }

    // Loop to handle non-video packets and decoder EAGAIN without recursion
    while (packets_processed < max_packets) {
        if (decode_call_count <= 5) {
            printf("Reading frame from format context... (packet %d)\n", packets_processed + 1);
            fflush(stdout);
        }
        
        // Read packet from input
        int read_result = av_read_frame(video->format_ctx, video->packet);
        if (read_result < 0) {
            if (read_result == AVERROR_EOF) {
                video->eof_reached = true;
                printf("End of video stream reached\n");
            } else {
                printf("Error reading packet: %s\n", av_err2str(read_result));
            }
            return -1;
        }

        packets_processed++;

        // Skip non-video packets (continue loop instead of recursion)
        if (video->packet->stream_index != video->video_stream_index) {
            if (decode_call_count <= 5 && packets_processed <= 10) {
                printf("Skipping non-video packet (stream %d)\n", video->packet->stream_index);
                fflush(stdout);
            }
            av_packet_unref(video->packet);
            continue; // Try next packet
        }

        // Enhanced packet analysis
    static int detailed_packet_count = 0;
        
    // Always inspect the first few packets and then periodically if advanced diagnostics are enabled
    bool show_detailed_info = (decode_call_count <= 3 || 
                              (video->advanced_diagnostics && (detailed_packet_count % 50) == 0));
    
    if (show_detailed_info) {
        detailed_packet_count++;            printf("\n----- Packet Analysis #%d -----\n", detailed_packet_count);
            printf("* Size: %d bytes\n", video->packet->size);
            printf("* Flags: 0x%x %s%s%s\n", video->packet->flags,
                  (video->packet->flags & AV_PKT_FLAG_KEY) ? "[KEY] " : "",
                  (video->packet->flags & AV_PKT_FLAG_CORRUPT) ? "[CORRUPT] " : "",
                  (video->packet->flags & AV_PKT_FLAG_DISCARD) ? "[DISCARD] " : "");
            printf("* PTS: %" PRId64 ", DTS: %" PRId64 "\n", 
                  video->packet->pts, video->packet->dts);
            
            // Check the first few bytes of the packet to identify NAL type
            if (video->packet->data && video->packet->size > 5) {
                printf("* Data starts with: ");
                for (int i = 0; i < 8 && i < video->packet->size; i++) {
                    printf("%02x ", video->packet->data[i]);
                }
                printf("\n");
                
                // Try to identify NAL format (Annex-B vs. avcC)
                if (video->packet->size >= 4 && 
                    video->packet->data[0] == 0 && 
                    video->packet->data[1] == 0 &&
                    ((video->packet->data[2] == 0 && video->packet->data[3] == 1) ||
                     (video->packet->data[2] == 1))) {
                    printf("* NAL format appears to be Annex-B (start code detected)\n");
                } else if (video->avcc_length_size > 0) {
                    printf("* NAL format appears to be avcC (length-prefixed)\n");
                    
                    // Try to decode the first NAL unit length
                    uint32_t nal_size = 0;
                    for (int i = 0; i < video->avcc_length_size && i < 4; i++) {
                        nal_size = (nal_size << 8) | video->packet->data[i];
                    }
                    printf("* First NAL unit size: %u bytes\n", nal_size);
                    
                    // Check if size makes sense
                    if (nal_size > 0 && nal_size <= video->packet->size - video->avcc_length_size) {
                        printf("* First NAL unit size appears valid\n");
                    } else {
                        printf("* WARNING: First NAL unit size appears invalid!\n");
                    }
                }
            }
            
            printf("--------------------------\n\n");
            fflush(stdout);
        } else if (decode_call_count <= 5) {
            printf("Sending video packet to decoder... (%d bytes, %s)\n", 
                   video->packet->size,
                   (video->packet->flags & AV_PKT_FLAG_KEY) ? "keyframe" : "non-keyframe");
            fflush(stdout);
        }
        
        // CRITICAL FIX: Convert avcC packet to Annex-B for V4L2 M2M hardware decoder
        if (video->use_hardware_decode && video->hw_decode_type == HW_DECODE_V4L2M2M && 
            video->avcc_length_size > 0 && video->packet->data && video->packet->size > 0) {
            
            int new_size = convert_sample_avcc_to_annexb_inplace(video->packet->data, 
                                                               video->packet->size, 
                                                               video->avcc_length_size);
            if (new_size > 0 && new_size != video->packet->size) {
                video->packet->size = new_size;
                if (show_detailed_info) {
                    printf("V4L2 packet converted: avcC→Annex-B (%d bytes)\n", new_size);
                    
                    // Show a sample of the converted packet
                    printf("After conversion, first 8 bytes: ");
                    for (int i = 0; i < 8 && i < new_size; i++) {
                        printf("%02x ", video->packet->data[i]);
                    }
                    printf("\n");
                }
            }
        }
        
        // Send packet to decoder
        int send_result = avcodec_send_packet(video->codec_ctx, video->packet);
        av_packet_unref(video->packet);
        
        if (send_result < 0) {
            printf("Error sending packet to decoder: %s\n", av_err2str(send_result));
            return -1;
        }

        if (decode_call_count <= 5) {
            printf("Trying to receive frame from decoder...\n");
            fflush(stdout);
        }
        
        // Try to receive decoded frame
        receive_result = avcodec_receive_frame(video->codec_ctx, video->frame);
        if (receive_result == 0) {
            // Successfully decoded a frame
            // OPTIMIZED: Reset failure count and adapt packet limit on success
            consecutive_fails = 0;
            if (packets_processed == 1 && max_packets > 5) {
                max_packets--; // Reduce limit when decoding is easy
            }
            
            // Enhanced frame diagnostics on every 30th frame or during first few frames
            static int frame_count = 0;
            frame_count++;
            if (decode_call_count <= 5 || (video->advanced_diagnostics && (frame_count % 30) == 0)) {
                const char *fmt_name = av_get_pix_fmt_name(video->frame->format);
                
                printf("\n----- Successfully decoded frame #%d -----\n", frame_count);
                printf("* Decoder: %s\n", video->use_hardware_decode ? "Hardware" : "Software");
                printf("* Packets processed: %d\n", packets_processed);
                printf("* Frame format: %s (%d)\n", fmt_name ? fmt_name : "unknown", video->frame->format);
                printf("* Frame size: %dx%d\n", video->frame->width, video->frame->height);
                printf("* Picture type: %c\n", av_get_picture_type_char(video->frame->pict_type));
                printf("* Color space: %s\n", av_color_space_name(video->frame->colorspace));
                printf("* Color range: %s\n", av_color_range_name(video->frame->color_range));
                
                // For V4L2 DRM hardware decoder
                if (video->frame->format == AV_PIX_FMT_DRM_PRIME) {
                    printf("* DRM PRIME frame detected!\n");
                    if (video->frame->data[0]) {
                        printf("* DRM PRIME buffer has data pointer\n");
                    }
                    #ifdef AVDRMFrameDescriptor
                    // Check if we have a DRM descriptor
                    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor*)video->frame->data[0];
                    if (desc) {
                        printf("* DRM PRIME descriptor: %d layers, %d objects\n", 
                               desc->nb_layers, desc->nb_objects);
                    }
                    #endif
                }
                
                printf("------------------------------------------\n\n");
                fflush(stdout);
            }
            
            return 0;
        } else if (receive_result == AVERROR(EAGAIN)) {
            // Need more packets - continue loop
            if (decode_call_count <= 5 && packets_processed <= 10) {
                printf("Decoder needs more packets (EAGAIN) - processed %d so far\n", packets_processed);
                fflush(stdout);
            }
            continue;
        } else {
            printf("Error receiving frame from decoder: %s\n", av_err2str(receive_result));
            return -1;
        }
    }

    // OPTIMIZED: Adaptive packet processing to prevent spikes
    consecutive_fails++;
    if (consecutive_fails < 3) {
        // Gradually increase packet limit for difficult sections
        if (max_packets < 15) max_packets += 2;
        if (decode_call_count <= 5) {
            printf("No frame after %d packets, increasing limit to %d (fails: %d)\n", 
                   packets_processed, max_packets, consecutive_fails);
        }
        return -1; // Try again next frame with higher limit
    }
    
    printf("Warning: Hardware decoder processed %d packets without getting a frame\n", max_packets);
    
    // Try falling back to software decoding if we're currently using hardware
    if (video->use_hardware_decode) {
        printf("\n=================== HARDWARE DECODE FAILURE DIAGNOSTIC ===================\n");
        printf("* Advanced diagnostics: %s\n", video->advanced_diagnostics ? "ENABLED" : "disabled");
        printf("* Hardware decoder type: %s\n", 
               video->hw_decode_type == HW_DECODE_V4L2M2M ? "V4L2 M2M" : 
               "Unknown");
        
        // Print codec context information
        printf("* Codec name: %s\n", video->codec->name);
        printf("* Codec long name: %s\n", video->codec->long_name);
        printf("* Codec context information:\n");
        printf("  - Pixel format: %s (%d)\n", 
               av_get_pix_fmt_name(video->codec_ctx->pix_fmt) ? 
               av_get_pix_fmt_name(video->codec_ctx->pix_fmt) : "unknown", 
               video->codec_ctx->pix_fmt);
        printf("  - Width x Height: %d x %d\n", video->codec_ctx->width, video->codec_ctx->height);
        printf("  - Color space: %s\n", av_color_space_name(video->codec_ctx->colorspace));
        printf("  - Color range: %s\n", av_color_range_name(video->codec_ctx->color_range));
        printf("  - Thread count: %d\n", video->codec_ctx->thread_count);
        
        // For V4L2 decoders, show additional information
        if (video->hw_decode_type == HW_DECODE_V4L2M2M) {
            printf("* V4L2 specific information:\n");
            printf("  - avcC length size: %d bytes\n", video->avcc_length_size);
            printf("  - Extradata size: %d bytes\n", video->codec_ctx->extradata_size);
            
            // Show the first few bytes of extradata if available
            if (video->codec_ctx->extradata && video->codec_ctx->extradata_size > 0) {
                printf("  - Extradata first 16 bytes: ");
                for (int i = 0; i < 16 && i < video->codec_ctx->extradata_size; i++) {
                    printf("%02x ", video->codec_ctx->extradata[i]);
                }
                printf("\n");
            } else {
                printf("  - No extradata available\n");
            }
            
            // Run external command to get v4l2 capabilities
            printf("* Running v4l2-ctl to check decoder capabilities (if available)...\n");
            system("v4l2-ctl --list-devices 2>/dev/null | grep -A 2 'mem2mem' || echo 'v4l2-ctl not available'");
        }
        
        printf("* Falling back to software decoding...\n");
        printf("===========================================================================\n\n");
        
        // Reset EOF flag and seek back to beginning
        video->eof_reached = false;
        av_seek_frame(video->format_ctx, video->video_stream_index, 0, AVSEEK_FLAG_BACKWARD);
        
        // Close current hardware codec context
        avcodec_free_context(&video->codec_ctx);
        
        // Find software decoder
        AVCodecParameters *codecpar = video->format_ctx->streams[video->video_stream_index]->codecpar;
        video->codec = avcodec_find_decoder(codecpar->codec_id);
        if (!video->codec) {
            fprintf(stderr, "No software decoder available\n");
            return -1;
        }
        
        // Allocate new codec context for software decoding
        video->codec_ctx = avcodec_alloc_context3(video->codec);
        if (!video->codec_ctx) {
            fprintf(stderr, "Failed to allocate software codec context\n");
            return -1;
        }
        
        // Copy codec parameters
        if (avcodec_parameters_to_context(video->codec_ctx, codecpar) < 0) {
            fprintf(stderr, "Failed to copy codec parameters for software decoding\n");
            avcodec_free_context(&video->codec_ctx);
            return -1;
        }
        
        // Configure for software decoding
        video->codec_ctx->thread_count = 4;
        video->codec_ctx->thread_type = FF_THREAD_FRAME;
        
        // Open software codec
        if (avcodec_open2(video->codec_ctx, video->codec, NULL) < 0) {
            fprintf(stderr, "Failed to open software codec\n");
            avcodec_free_context(&video->codec_ctx);
            return -1;
        }
        
        printf("Successfully switched to software decoding\n");
        video->use_hardware_decode = false;
        
        // Try decoding again with software decoder
        return video_decode_frame(video);
    }
    
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

    // Seek to the beginning of the video
    if (av_seek_frame(video->format_ctx, video->video_stream_index, 0, AVSEEK_FLAG_BACKWARD) < 0) {
        printf("Warning: Failed to seek to beginning\n");
        return -1;
    }

    // Flush decoder buffers
    avcodec_flush_buffers(video->codec_ctx);

    // Reset EOF flag
    video->eof_reached = false;

    printf("Video playback restarted\n");
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
    
    // Seek to the specified timestamp
    if (av_seek_frame(video->format_ctx, video->video_stream_index, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
        printf("Warning: Failed to seek to timestamp %ld\n", timestamp);
        return;
    }
    
    // Flush decoder buffers
    avcodec_flush_buffers(video->codec_ctx);
    
    // Reset EOF flag
    video->eof_reached = false;
}

void video_cleanup(video_context_t *video) {
    // No frame buffers in this version
    
    if (video->frame) {
        av_frame_free(&video->frame);
    }
    if (video->packet) {
        av_packet_free(&video->packet);
    }
    if (video->codec_ctx) {
        avcodec_free_context(&video->codec_ctx);
    }
    if (video->format_ctx) {
        avformat_close_input(&video->format_ctx);
    }
    
    memset(video, 0, sizeof(*video));
}