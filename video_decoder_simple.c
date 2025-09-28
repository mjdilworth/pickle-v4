#define _GNU_SOURCE
#include "video_decoder.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>

int video_init(video_context_t *video, const char *filename) {
    memset(video, 0, sizeof(*video));

    // Allocate packet
    video->packet = av_packet_alloc();
    if (!video->packet) {
        fprintf(stderr, "Failed to allocate packet\n");
        return -1;
    }

    // Open input file
    if (avformat_open_input(&video->format_ctx, filename, NULL, NULL) < 0) {
        fprintf(stderr, "Failed to open input file: %s\n", filename);
        av_packet_free(&video->packet);
        return -1;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(video->format_ctx, NULL) < 0) {
        fprintf(stderr, "Failed to find stream information\n");
        avformat_close_input(&video->format_ctx);
        av_packet_free(&video->packet);
        return -1;
    }

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
    
    // Simple hardware decoder selection - no profile checking or transcoding
    if (codecpar->codec_id == AV_CODEC_ID_H264) {
        video->codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
        if (video->codec) {
            video->use_hardware_decode = true;
            video->hw_decode_type = HW_DECODE_V4L2M2M;
            printf("Using V4L2 M2M hardware decoder for H.264 (profile: %d)\n", codecpar->profile);
        }
    } else if (codecpar->codec_id == AV_CODEC_ID_HEVC) {
        video->codec = avcodec_find_decoder_by_name("hevc_v4l2m2m");
        if (video->codec) {
            video->use_hardware_decode = true;
            video->hw_decode_type = HW_DECODE_V4L2M2M;
            printf("Using V4L2 M2M hardware decoder for H.265\n");
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
        // Use CPU-accessible YUV format instead of DRM PRIME to avoid transfer issues
        // V4L2 M2M can output to regular YUV420P that we can access directly
        video->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        // Enable multi-threading for better performance
        video->codec_ctx->thread_count = 1; // V4L2 handles threading internally
        printf("V4L2 M2M configured for YUV420P output\n");
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
    static int frames_decoded = 0;
    static int consecutive_failures = 0;
    
    if (!video->initialized || video->eof_reached) {
        return -1;
    }

    while (1) {
        // Read packet
        int ret = av_read_frame(video->format_ctx, video->packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                if (video->loop_playback) {
                    // Restart playback for looping
                    if (video_restart_playback(video) == 0) {
                        continue; // Try reading again from the beginning
                    }
                }
                video->eof_reached = true;
            }
            return ret;
        }

        // Skip non-video packets
        if (video->packet->stream_index != video->video_stream_index) {
            av_packet_unref(video->packet);
            continue;
        }

        // Send packet to decoder
        ret = avcodec_send_packet(video->codec_ctx, video->packet);
        av_packet_unref(video->packet);
        
        if (ret < 0) {
            fprintf(stderr, "Error sending packet to decoder\n");
            return ret;
        }

        // Try to receive frame from decoder
        ret = avcodec_receive_frame(video->codec_ctx, video->frame);
        if (ret == AVERROR(EAGAIN)) {
            consecutive_failures++;
            // If hardware decoder repeatedly fails to produce frames, it might be incompatible
            if (video->use_hardware_decode && consecutive_failures > 100) {
                printf("Hardware decoder appears incompatible, should fallback to software\n");
                return -2; // Special error code for fallback needed
            }
            continue; // Need more packets - this is normal for hardware decoders
        } else if (ret == AVERROR_EOF) {
            if (video->loop_playback) {
                // Restart playback for looping
                if (video_restart_playback(video) == 0) {
                    continue; // Try decoding again from the beginning
                }
            }
            video->eof_reached = true;
            return ret;
        } else if (ret < 0) {
            consecutive_failures++;
            fprintf(stderr, "Error receiving frame from decoder: %s (error: %d)\n", 
                   video->use_hardware_decode ? "hardware" : "software", ret);
            // If hardware decoder consistently fails, suggest fallback
            if (video->use_hardware_decode && consecutive_failures > 10) {
                printf("Hardware decoder failing consistently, should fallback to software\n");
                return -2; // Special error code for fallback needed
            }
            return ret;
        }

        // Successfully decoded frame
        consecutive_failures = 0; // Reset failure counter on success
        frames_decoded++;
        if (frames_decoded == 1) {
            printf("First frame decoded successfully\n");
        }
        return 0;
    }
}

uint8_t* video_get_rgb_data(video_context_t *video) {
    // Legacy function - RGB data is no longer used with GPU YUV conversion
    (void)video; // Suppress unused parameter warning
    return NULL;
}

double video_get_frame_time(video_context_t *video) {
    if (!video->initialized || !video->frame) {
        return 0.0;
    }
    
    AVStream *stream = video->format_ctx->streams[video->video_stream_index];
    return (double)video->frame->pts * av_q2d(stream->time_base);
}

bool video_is_eof(video_context_t *video) {
    return video->eof_reached;
}

void video_seek(video_context_t *video, int64_t timestamp) {
    if (!video->initialized) {
        return;
    }
    
    AVStream *stream = video->format_ctx->streams[video->video_stream_index];
    int64_t seek_target = av_rescale_q(timestamp, AV_TIME_BASE_Q, stream->time_base);
    
    if (av_seek_frame(video->format_ctx, video->video_stream_index, seek_target, AVSEEK_FLAG_BACKWARD) >= 0) {
        avcodec_flush_buffers(video->codec_ctx);
        video->eof_reached = false;
    }
}

void video_get_dimensions(video_context_t *video, int *width, int *height) {
    if (!video->initialized) {
        *width = 0;
        *height = 0;
        return;
    }
    *width = video->width;
    *height = video->height;
}

void video_get_yuv_data(video_context_t *video, uint8_t **y, uint8_t **u, uint8_t **v, 
                       int *y_stride, int *u_stride, int *v_stride) {
    if (!video || !video->initialized || !video->frame) {
        if (y) *y = NULL;
        if (u) *u = NULL;
        if (v) *v = NULL;
        if (y_stride) *y_stride = 0;
        if (u_stride) *u_stride = 0;
        if (v_stride) *v_stride = 0;
        return;
    }
    
    // Extract YUV plane pointers and strides directly from decoded frame
    // Both hardware and software decoders now output to CPU-accessible YUV420P
    if (y) *y = video->frame->data[0];        // Y plane
    if (u) *u = video->frame->data[1];        // U plane  
    if (v) *v = video->frame->data[2];        // V plane
    if (y_stride) *y_stride = video->frame->linesize[0];  // Y stride
    if (u_stride) *u_stride = video->frame->linesize[1];  // U stride
    if (v_stride) *v_stride = video->frame->linesize[2];  // V stride
}

bool video_is_hardware_decoded(video_context_t *video) {
    return video && video->use_hardware_decode;
}

void video_cleanup(video_context_t *video) {
    // Simplified cleanup - no transcoding support
    if (video->sws_ctx) {
        sws_freeContext(video->sws_ctx);
    }
    if (video->hw_frame) {
        av_frame_free(&video->hw_frame);
    }
    if (video->frame) {
        av_frame_free(&video->frame);
    }
    if (video->hw_device_ctx) {
        av_buffer_unref(&video->hw_device_ctx);
    }
    if (video->codec_ctx) {
        avcodec_free_context(&video->codec_ctx);
    }
    if (video->format_ctx) {
        avformat_close_input(&video->format_ctx);
    }
    if (video->packet) {
        av_packet_free(&video->packet);
    }
    
    memset(video, 0, sizeof(*video));
}

void video_set_loop(video_context_t *video, bool loop) {
    if (video) {
        video->loop_playback = loop;
        printf("Video looping %s\n", loop ? "enabled" : "disabled");
    }
}

int video_restart_playback(video_context_t *video) {
    if (!video || !video->format_ctx) {
        return -1;
    }
    
    // Seek to the beginning of the stream
    int ret = av_seek_frame(video->format_ctx, video->video_stream_index, 0, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        fprintf(stderr, "Failed to seek to beginning: %s\n", av_err2str(ret));
        return -1;
    }
    
    // Flush codec buffers
    if (video->codec_ctx) {
        avcodec_flush_buffers(video->codec_ctx);
    }
    
    // Reset EOF flag
    video->eof_reached = false;
    
    printf("Video playback restarted (loop)\n");
    return 0;
}