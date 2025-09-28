#define _GNU_SOURCE
#include "video_decoder.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>

// Streaming transcoder structures and functions
typedef struct {
    const char *input_file;
    int output_fd;
    bool *should_stop;
} transcoder_thread_args_t;

static int pipe_write_packet(void *opaque, const uint8_t *buf, int buf_size) {
    int fd = (intptr_t)opaque;
    ssize_t written = write(fd, buf, buf_size);
    return (written < 0) ? AVERROR(errno) : written;
}

int video_transcode_for_hardware(const char *input_file, const char *output_file) {
    AVFormatContext *input_ctx = NULL, *output_ctx = NULL;
    AVCodecContext *decoder_ctx = NULL, *encoder_ctx = NULL;
    const AVCodec *decoder, *encoder;
    AVStream *input_stream, *output_stream;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    int ret = 0;
    int frame_count = 0;

    printf("Transcoding %s to hardware-compatible format...\n", input_file);

    // Open input
    if (avformat_open_input(&input_ctx, input_file, NULL, NULL) < 0) {
        fprintf(stderr, "Failed to open input file for transcoding\n");
        return -1;
    }

    if (avformat_find_stream_info(input_ctx, NULL) < 0) {
        fprintf(stderr, "Failed to find stream info\n");
        ret = -1;
        goto cleanup;
    }

    // Find video stream
    int video_stream_idx = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (video_stream_idx < 0) {
        fprintf(stderr, "No video stream found\n");
        ret = -1;
        goto cleanup;
    }

    input_stream = input_ctx->streams[video_stream_idx];
    
    // Set up decoder
    decoder_ctx = avcodec_alloc_context3(decoder);
    if (!decoder_ctx) {
        fprintf(stderr, "Failed to allocate decoder context\n");
        ret = -1;
        goto cleanup;
    }
    
    avcodec_parameters_to_context(decoder_ctx, input_stream->codecpar);
    
    if (avcodec_open2(decoder_ctx, decoder, NULL) < 0) {
        fprintf(stderr, "Failed to open decoder\n");
        ret = -1;
        goto cleanup;
    }

    // Set up output
    avformat_alloc_output_context2(&output_ctx, NULL, NULL, output_file);
    if (!output_ctx) {
        fprintf(stderr, "Failed to create output context\n");
        ret = -1;
        goto cleanup;
    }

    // Try hardware encoder first, fallback to software
    encoder = avcodec_find_encoder_by_name("h264_v4l2m2m");
    if (!encoder) {
        printf("Hardware encoder not available, using software encoder\n");
        encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    } else {
        printf("Using hardware encoder (h264_v4l2m2m)\n");
    }
    
    if (!encoder) {
        fprintf(stderr, "H.264 encoder not found\n");
        ret = -1;
        goto cleanup;
    }
    
    encoder_ctx = avcodec_alloc_context3(encoder);
    if (!encoder_ctx) {
        fprintf(stderr, "Failed to allocate encoder context\n");
        ret = -1;
        goto cleanup;
    }
    
    encoder_ctx->width = decoder_ctx->width;
    encoder_ctx->height = decoder_ctx->height;
    encoder_ctx->time_base = (AVRational){1, 30}; // 30 fps for compatibility
    encoder_ctx->framerate = (AVRational){30, 1};
    encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    
    // Use High profile (compatible with RPi4 hardware decoder)
    encoder_ctx->profile = 100; // H.264 High Profile
    encoder_ctx->level = 42; // Level 4.2
    
    // Quality settings optimized for hardware playback
    encoder_ctx->bit_rate = 2000000; // 2Mbps
    encoder_ctx->gop_size = 30;
    encoder_ctx->max_b_frames = 0; // Disable B-frames for better hardware compatibility
    
    // Hardware decoder friendly settings
    encoder_ctx->refs = 1; // Single reference frame
    encoder_ctx->keyint_min = 30; // Regular keyframes

    if (output_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        encoder_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (avcodec_open2(encoder_ctx, encoder, NULL) < 0) {
        fprintf(stderr, "Failed to open encoder\n");
        ret = -1;
        goto cleanup;
    }

    // Create output stream
    output_stream = avformat_new_stream(output_ctx, NULL);
    if (!output_stream) {
        fprintf(stderr, "Failed to create output stream\n");
        ret = -1;
        goto cleanup;
    }
    
    avcodec_parameters_from_context(output_stream->codecpar, encoder_ctx);
    output_stream->time_base = encoder_ctx->time_base;

    // Open output file
    if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&output_ctx->pb, output_file, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "Failed to open output file\n");
            ret = -1;
            goto cleanup;
        }
    }

    if (avformat_write_header(output_ctx, NULL) < 0) {
        fprintf(stderr, "Failed to write header\n");
        ret = -1;
        goto cleanup;
    }

    // Allocate frame and packet
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) {
        fprintf(stderr, "Failed to allocate frame/packet\n");
        ret = -1;
        goto cleanup;
    }

    // Transcoding loop
    while (av_read_frame(input_ctx, packet) >= 0) {
        if (packet->stream_index == video_stream_idx) {
            // Decode
            if (avcodec_send_packet(decoder_ctx, packet) >= 0) {
                while (avcodec_receive_frame(decoder_ctx, frame) >= 0) {
                    frame->pts = frame_count++;
                    
                    // Encode
                    if (avcodec_send_frame(encoder_ctx, frame) >= 0) {
                        AVPacket *out_packet = av_packet_alloc();
                        while (avcodec_receive_packet(encoder_ctx, out_packet) >= 0) {
                            out_packet->stream_index = 0;
                            av_packet_rescale_ts(out_packet, encoder_ctx->time_base, output_stream->time_base);
                            av_interleaved_write_frame(output_ctx, out_packet);
                        }
                        av_packet_free(&out_packet);
                    }
                    
                    // Progress indicator
                    if (frame_count % 30 == 0) {
                        printf("Transcoded %d frames...\r", frame_count);
                        fflush(stdout);
                    }
                }
            }
        }
        av_packet_unref(packet);
    }

    // Flush encoder
    avcodec_send_frame(encoder_ctx, NULL);
    AVPacket *out_packet = av_packet_alloc();
    while (avcodec_receive_packet(encoder_ctx, out_packet) >= 0) {
        out_packet->stream_index = 0;
        av_packet_rescale_ts(out_packet, encoder_ctx->time_base, output_stream->time_base);
        av_interleaved_write_frame(output_ctx, out_packet);
    }
    av_packet_free(&out_packet);

    av_write_trailer(output_ctx);
    printf("\nTranscoding complete! %d frames processed.\n", frame_count);

cleanup:
    if (packet) av_packet_free(&packet);
    if (frame) av_frame_free(&frame);
    if (decoder_ctx) avcodec_free_context(&decoder_ctx);
    if (encoder_ctx) avcodec_free_context(&encoder_ctx);
    if (input_ctx) avformat_close_input(&input_ctx);
    if (output_ctx) {
        if (!(output_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&output_ctx->pb);
        avformat_free_context(output_ctx);
    }
    return ret;
}

void* streaming_transcoder_thread(void *arg) {
    transcoder_thread_args_t *args = (transcoder_thread_args_t*)arg;
    
    AVFormatContext *input_ctx = NULL, *output_ctx = NULL;
    AVCodecContext *decoder_ctx = NULL, *encoder_ctx = NULL;
    const AVCodec *decoder, *encoder;
    AVStream *input_stream, *output_stream;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    int frame_count = 0;

    printf("Starting streaming transcoding of %s...\n", args->input_file);

    // Open input
    if (avformat_open_input(&input_ctx, args->input_file, NULL, NULL) < 0) {
        fprintf(stderr, "Streaming transcoder: Failed to open input file\n");
        return NULL;
    }

    if (avformat_find_stream_info(input_ctx, NULL) < 0) {
        fprintf(stderr, "Streaming transcoder: Failed to find stream info\n");
        goto cleanup;
    }

    // Find video stream
    int video_stream_idx = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (video_stream_idx < 0) {
        fprintf(stderr, "Streaming transcoder: No video stream found\n");
        goto cleanup;
    }

    input_stream = input_ctx->streams[video_stream_idx];
    
    // Set up decoder
    decoder_ctx = avcodec_alloc_context3(decoder);
    if (!decoder_ctx) {
        fprintf(stderr, "Streaming transcoder: Failed to allocate decoder context\n");
        goto cleanup;
    }
    
    avcodec_parameters_to_context(decoder_ctx, input_stream->codecpar);
    
    if (avcodec_open2(decoder_ctx, decoder, NULL) < 0) {
        fprintf(stderr, "Streaming transcoder: Failed to open decoder\n");
        goto cleanup;
    }

    // Create output context for pipe (use mpegts for streaming)
    avformat_alloc_output_context2(&output_ctx, NULL, "mpegts", NULL);
    if (!output_ctx) {
        fprintf(stderr, "Streaming transcoder: Failed to create output context\n");
        goto cleanup;
    }

    // Use hardware encoder for better hardware decoder compatibility
    encoder = avcodec_find_encoder_by_name("h264_v4l2m2m");
    if (!encoder) {
        printf("Streaming transcoder: Hardware encoder not available, using libx264 with hardware-friendly settings\n");
        encoder = avcodec_find_encoder_by_name("libx264");
        if (!encoder) {
            encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
        }
    } else {
        printf("Streaming transcoder: Using hardware encoder for maximum compatibility\n");
    }
    
    if (!encoder) {
        fprintf(stderr, "Streaming transcoder: H.264 encoder not found\n");
        goto cleanup;
    }
    
    encoder_ctx = avcodec_alloc_context3(encoder);
    if (!encoder_ctx) {
        fprintf(stderr, "Streaming transcoder: Failed to allocate encoder context\n");
        goto cleanup;
    }
    
    encoder_ctx->width = decoder_ctx->width;
    encoder_ctx->height = decoder_ctx->height;
    encoder_ctx->time_base = (AVRational){1, 30};
    encoder_ctx->framerate = (AVRational){30, 1};
    encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder_ctx->profile = 100; // High profile
    encoder_ctx->level = 42;
    encoder_ctx->bit_rate = 2000000;
    encoder_ctx->gop_size = 30;
    encoder_ctx->max_b_frames = 0;
    encoder_ctx->refs = 1;
    encoder_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; // Important for streaming

    if (avcodec_open2(encoder_ctx, encoder, NULL) < 0) {
        fprintf(stderr, "Streaming transcoder: Failed to open encoder\n");
        goto cleanup;
    }

    // Create output stream
    output_stream = avformat_new_stream(output_ctx, NULL);
    if (!output_stream) {
        fprintf(stderr, "Streaming transcoder: Failed to create output stream\n");
        goto cleanup;
    }
    
    avcodec_parameters_from_context(output_stream->codecpar, encoder_ctx);
    output_stream->time_base = encoder_ctx->time_base;

    // Set up custom IO for pipe
    unsigned char *avio_buffer = av_malloc(4096);
    if (!avio_buffer) {
        fprintf(stderr, "Streaming transcoder: Failed to allocate AVIO buffer\n");
        goto cleanup;
    }
    
    AVIOContext *avio_ctx = avio_alloc_context(avio_buffer, 4096, 1, 
                                              (void*)(intptr_t)args->output_fd, 
                                              NULL, pipe_write_packet, NULL);
    if (!avio_ctx) {
        av_free(avio_buffer);
        fprintf(stderr, "Streaming transcoder: Failed to create AVIO context\n");
        goto cleanup;
    }
    
    output_ctx->pb = avio_ctx;

    // Write header
    if (avformat_write_header(output_ctx, NULL) < 0) {
        fprintf(stderr, "Streaming transcoder: Failed to write header\n");
        goto cleanup;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) {
        fprintf(stderr, "Streaming transcoder: Failed to allocate frame/packet\n");
        goto cleanup;
    }

    // Main transcoding loop
    while (!*(args->should_stop) && av_read_frame(input_ctx, packet) >= 0) {
        if (packet->stream_index == video_stream_idx) {
            if (avcodec_send_packet(decoder_ctx, packet) >= 0) {
                while (avcodec_receive_frame(decoder_ctx, frame) >= 0) {
                    frame->pts = frame_count++;
                    
                    if (avcodec_send_frame(encoder_ctx, frame) >= 0) {
                        AVPacket *out_packet = av_packet_alloc();
                        while (avcodec_receive_packet(encoder_ctx, out_packet) >= 0) {
                            out_packet->stream_index = 0;
                            av_packet_rescale_ts(out_packet, encoder_ctx->time_base, output_stream->time_base);
                            av_interleaved_write_frame(output_ctx, out_packet);
                            
                            // Force flush the pipe to make data available immediately
                            if (avio_ctx) {
                                avio_flush(avio_ctx);
                            }
                        }
                        av_packet_free(&out_packet);
                    }
                    
                    // Progress update every second
                    if (frame_count % 30 == 0) {
                        printf("Streaming transcode: %d frames processed\r", frame_count);
                        fflush(stdout);
                    }
                }
            }
        }
        av_packet_unref(packet);
    }

    // Flush encoder
    avcodec_send_frame(encoder_ctx, NULL);
    AVPacket *out_packet = av_packet_alloc();
    while (avcodec_receive_packet(encoder_ctx, out_packet) >= 0) {
        out_packet->stream_index = 0;
        av_packet_rescale_ts(out_packet, encoder_ctx->time_base, output_stream->time_base);
        av_interleaved_write_frame(output_ctx, out_packet);
    }
    av_packet_free(&out_packet);

    av_write_trailer(output_ctx);
    printf("\nStreaming transcoding complete: %d frames\n", frame_count);

cleanup:
    if (packet) av_packet_free(&packet);
    if (frame) av_frame_free(&frame);
    if (decoder_ctx) avcodec_free_context(&decoder_ctx);
    if (encoder_ctx) avcodec_free_context(&encoder_ctx);
    if (input_ctx) avformat_close_input(&input_ctx);
    if (output_ctx) {
        if (output_ctx->pb) {
            avio_flush(output_ctx->pb);
            av_freep(&output_ctx->pb->buffer);
            avio_context_free(&output_ctx->pb);
        }
        avformat_free_context(output_ctx);
    }
    
    close(args->output_fd);
    return NULL;
}

int video_init_streaming_transcode(video_context_t *video, const char *filename) {
    printf("Initializing streaming transcoding for %s\n", filename);
    
    // Mark as transcoded stream to force hardware decoding
    video->is_transcoded_stream = true;
    
    // Create temporary file for streaming output
    char temp_path[256];
    snprintf(temp_path, sizeof(temp_path), "/tmp/pickle_stream_%d.ts", getpid());
    
    // Set up streaming transcoder
    video->transcoder.transcoding_active = true;
    video->transcoder.should_stop = false;
    
    // We'll use a simple fork/exec approach instead of threading
    
    // Store temp file path for cleanup
    video->temp_file = strdup(temp_path);
    
    // Start simple file-based transcoding in background
    pid_t transcoder_pid = fork();
    if (transcoder_pid == 0) {
        // Child process: run ffmpeg transcoding
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), 
                 "ffmpeg -i '%s' -c:v h264_v4l2m2m -b:v 2M -g 30 -pix_fmt yuv420p "
                 "-profile:v high -level 4.2 -f mpegts '%s' -y 2>/dev/null || "
                 "ffmpeg -i '%s' -c:v libx264 -preset ultrafast -profile:v high -level 4.2 "
                 "-pix_fmt yuv420p -b:v 2M -bf 0 -g 30 -f mpegts '%s' -y",
                 filename, temp_path, filename, temp_path);
        
        printf("Background transcoding: %s\n", cmd);
        system(cmd);
        exit(0);
    }
    
    // Give transcoder time to start and write initial data
    printf("Waiting for transcoding to start...\n");
    
    // Wait for file to have some data (up to 5 seconds)
    struct stat st;
    int wait_count = 0;
    while (wait_count < 50) { // 5 seconds max
        usleep(100000); // 100ms
        if (stat(temp_path, &st) == 0 && st.st_size > 1024) {
            printf("Transcoded data available (%ld bytes), starting playback\n", st.st_size);
            break;
        }
        wait_count++;
    }
    
    if (wait_count >= 50) {
        printf("Timeout waiting for transcoding data\n");
        unlink(temp_path);
        free(video->temp_file);
        video->temp_file = NULL;
        return -1;
    }
    
    // Initialize video decoder with the temp file
    memset(&video->format_ctx, 0, sizeof(video->format_ctx));
    
    // Standard video initialization with the stream file
    video->packet = av_packet_alloc();
    if (!video->packet) {
        fprintf(stderr, "Failed to allocate packet\n");
        return -1;
    }

    // Open the streaming file
    if (avformat_open_input(&video->format_ctx, video->temp_file, NULL, NULL) < 0) {
        fprintf(stderr, "Failed to open streaming file: %s\n", video->temp_file);
        av_packet_free(&video->packet);
        return -1;
    }

    // Continue with standard video initialization...
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

    // Set up hardware decoder for the transcoded stream (should be compatible now)
    AVCodecParameters *codecpar = video->format_ctx->streams[video->video_stream_index]->codecpar;
    
    video->use_hardware_decode = false;
    video->hw_decode_type = HW_DECODE_NONE;
    
    // Try hardware decoder for the transcoded stream
    if (codecpar->codec_id == AV_CODEC_ID_H264) {
        video->codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
        if (video->codec) {
            video->use_hardware_decode = true;
            video->hw_decode_type = HW_DECODE_V4L2M2M;
            printf("Using V4L2 M2M hardware decoder for transcoded stream\n");
        }
    }
    
    // Fall back to software if needed
    if (!video->codec) {
        printf("Using software decoder for transcoded stream\n");
        video->codec = avcodec_find_decoder(codecpar->codec_id);
        if (!video->codec) {
            fprintf(stderr, "Unsupported codec\n");
            avformat_close_input(&video->format_ctx);
            av_packet_free(&video->packet);
            return -1;
        }
    }

    // Continue with codec setup
    video->codec_ctx = avcodec_alloc_context3(video->codec);
    if (!video->codec_ctx) {
        fprintf(stderr, "Failed to allocate codec context\n");
        avformat_close_input(&video->format_ctx);
        av_packet_free(&video->packet);
        return -1;
    }

    if (avcodec_parameters_to_context(video->codec_ctx, codecpar) < 0) {
        fprintf(stderr, "Failed to copy codec parameters to decoder context\n");
        avcodec_free_context(&video->codec_ctx);
        avformat_close_input(&video->format_ctx);
        av_packet_free(&video->packet);
        return -1;
    }

    // Hardware decoder configuration
    if (video->use_hardware_decode && video->hw_decode_type == HW_DECODE_V4L2M2M) {
        video->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        video->codec_ctx->thread_count = 1;
        printf("V4L2 M2M configured for YUV420P output\n");
    } else {
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

    // Allocate frame
    video->frame = av_frame_alloc();
    if (!video->frame) {
        fprintf(stderr, "Failed to allocate frame\n");
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

    printf("Streaming transcoded video initialized: %dx%d, %.2f fps\n", 
           video->width, video->height, video->fps);

    if (!video->use_hardware_decode) {
        printf("Note: Using software YUV decode for transcoded stream, GPU will handle YUV→RGB conversion\n");
    } else {
        printf("Hardware decoding enabled for transcoded stream, GPU will handle YUV→RGB conversion\n");
    }

    video->initialized = true;
    video->eof_reached = false;

    return 0;
}

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
    
    // Check for H.264 hardware compatibility and auto-transcode if needed
    bool hw_compatible = true;
    
    if (codecpar->codec_id == AV_CODEC_ID_H264) {
        // Check for incompatible profiles (skip for transcoded streams)
        if (codecpar->profile == 77 && !video->is_transcoded_stream) { // Main profile
            printf("H.264 Main profile (77) detected - incompatible with hardware decoder\n");
            
            // Try streaming transcoding first for immediate playback
            printf("Attempting streaming transcoding for immediate playback...\n");
            
            if (video_init_streaming_transcode(video, filename) == 0) {
                printf("Streaming transcoding initialized - playback starting immediately!\n");
                return 0; // Return success - video is ready for immediate playback
            } else {
                printf("Streaming transcoding failed, falling back to full transcoding...\n");
                
                // Fall back to original full transcoding method
                char temp_file[] = "/tmp/pickle_hw_XXXXXX";
                int fd = mkstemp(temp_file);
                if (fd != -1) {
                    close(fd);
                    // Add .mp4 extension
                    char temp_file_mp4[256];
                    snprintf(temp_file_mp4, sizeof(temp_file_mp4), "%s.mp4", temp_file);
                    rename(temp_file, temp_file_mp4);
                    strcpy(temp_file, temp_file_mp4);
                    
                    if (video_transcode_for_hardware(filename, temp_file) == 0) {
                    printf("Using transcoded file for hardware acceleration\n");
                    
                    // Cleanup current context and reinitialize with transcoded file
                    avformat_close_input(&video->format_ctx);
                    if (avformat_open_input(&video->format_ctx, temp_file, NULL, NULL) < 0) {
                        fprintf(stderr, "Failed to open transcoded file\n");
                        unlink(temp_file);
                        av_packet_free(&video->packet);
                        return -1;
                    }
                    
                    // Re-get stream info from transcoded file
                    if (avformat_find_stream_info(video->format_ctx, NULL) < 0) {
                        fprintf(stderr, "Failed to find stream info in transcoded file\n");
                        unlink(temp_file);
                        avformat_close_input(&video->format_ctx);
                        av_packet_free(&video->packet);
                        return -1;
                    }
                    
                    // Re-find video stream
                    video->video_stream_index = -1;
                    for (unsigned int i = 0; i < video->format_ctx->nb_streams; i++) {
                        if (video->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                            video->video_stream_index = i;
                            break;
                        }
                    }
                    
                    codecpar = video->format_ctx->streams[video->video_stream_index]->codecpar;
                    hw_compatible = true; // Transcoded file should be hardware compatible
                    
                    // Store temp filename for cleanup
                    video->temp_file = strdup(temp_file);
                    } else {
                        printf("Transcoding failed, will use software decoding\n");
                        unlink(temp_file);
                        hw_compatible = false;
                    }
                } else {
                    printf("Could not create temporary file, using software decoding\n");
                    hw_compatible = false;
                }
            }
        }
        
        if (hw_compatible || video->is_transcoded_stream) {
            video->codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
            if (video->codec) {
                video->use_hardware_decode = true;
                video->hw_decode_type = HW_DECODE_V4L2M2M;
                if (video->is_transcoded_stream) {
                    printf("Using V4L2 M2M hardware decoder for transcoded H.264 stream\n");
                } else {
                    printf("Using V4L2 M2M hardware decoder for H.264 (profile: %d)\n", codecpar->profile);
                }
            }
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
    // Clean up streaming transcoder
    if (video->transcoder.transcoding_active) {
        printf("Stopping streaming transcoder...\n");
        video->transcoder.should_stop = true;
        
        // Wait for transcoder thread to finish
        pthread_join(video->transcoder.transcoder_thread, NULL);
        
        // Close pipes
        if (video->transcoder.pipe_fd[0] >= 0) close(video->transcoder.pipe_fd[0]);
        if (video->transcoder.pipe_fd[1] >= 0) close(video->transcoder.pipe_fd[1]);
        
        video->transcoder.transcoding_active = false;
    }
    
    // Clean up temporary transcoded file if it exists
    if (video->temp_file) {
        printf("Cleaning up temporary file: %s\n", video->temp_file);
        unlink(video->temp_file);
        free(video->temp_file);
        video->temp_file = NULL;
    }
    
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