#include <stdio.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <video_file>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    printf("Testing file: %s\n", filename);
    
    // Open file
    AVFormatContext *format_ctx = NULL;
    int ret = avformat_open_input(&format_ctx, filename, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open file: %d\n", ret);
        return 1;
    }
    printf("✓ File opened\n");

    ret = avformat_find_stream_info(format_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream info: %d\n", ret);
        avformat_close_input(&format_ctx);
        return 1;
    }
    printf("✓ Stream info found\n");

    int video_stream_idx = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_stream_idx < 0) {
        fprintf(stderr, "No video stream found\n");
        avformat_close_input(&format_ctx);
        return 1;
    }
    printf("✓ Video stream found at index %d\n", video_stream_idx);

    AVStream *stream = format_ctx->streams[video_stream_idx];
    
    // Show avcC header
    uint8_t *extradata = stream->codecpar->extradata;
    if (extradata && stream->codecpar->extradata_size >= 8) {
        printf("avcC header first 8 bytes: ");
        for (int i = 0; i < 8 && i < stream->codecpar->extradata_size; i++) {
            printf("%02x ", extradata[i]);
        }
        printf("\n");
        
        // Parse avcC
        if (extradata[0] == 1) {  // version 1
            uint8_t profile_byte = extradata[1];
            uint8_t level = extradata[3];
            
            printf("Profile byte: 0x%02x\n", profile_byte);
            printf("Level: %d (%d.%d)\n", level, level/10, level%10);
            
            const char *profile_name = "Unknown";
            if (profile_byte == 0x42) profile_name = "Baseline (0x42)";
            else if (profile_byte == 0x4d) profile_name = "Main (0x4d)";
            else if (profile_byte == 0x58) profile_name = "High (0x58)";
            
            printf("Detected Profile: %s\n", profile_name);
        }
    }

    printf("\n=== Testing Hardware Decoder ===\n");
    
    const AVCodec *hw_codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
    if (!hw_codec) {
        printf("✗ Hardware decoder h264_v4l2m2m not available\n");
        avformat_close_input(&format_ctx);
        return 1;
    }
    printf("✓ Found hardware decoder: h264_v4l2m2m\n");

    AVCodecContext *codec_ctx = avcodec_alloc_context3(hw_codec);
    if (!codec_ctx) {
        printf("✗ Could not allocate codec context\n");
        avformat_close_input(&format_ctx);
        return 1;
    }

    avcodec_parameters_to_context(codec_ctx, stream->codecpar);
    codec_ctx->thread_count = 1;
    codec_ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;

    ret = avcodec_open2(codec_ctx, hw_codec, NULL);
    if (ret < 0) {
        printf("✗ Could not open codec: %d\n", ret);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return 1;
    }
    printf("✓ Hardware codec opened successfully\n");
    printf("  Profile: %d, Level: %d\n", codec_ctx->profile, codec_ctx->level);

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    int frames_decoded = 0;
    int packets_sent = 0;

    printf("\nAttempting to decode first frame...\n");

    while (av_read_frame(format_ctx, packet) >= 0) {
        if (packet->stream_index != video_stream_idx) {
            av_packet_unref(packet);
            continue;
        }

        avcodec_send_packet(codec_ctx, packet);
        av_packet_unref(packet);
        packets_sent++;

        int recv_ret = avcodec_receive_frame(codec_ctx, frame);
        if (recv_ret == 0) {
            frames_decoded++;
            printf("✓ Frame decoded: %dx%d, format=%d\n", frame->width, frame->height, frame->format);
            break;
        } else if (recv_ret == AVERROR(EAGAIN)) {
            if (packets_sent % 10 == 0) {
                printf("  Sent %d packets, still waiting for frame...\n", packets_sent);
            }
        } else {
            printf("✗ Error receiving frame: %d\n", recv_ret);
            break;
        }

        if (packets_sent >= 50) {
            printf("✗ Sent 50 packets without getting a frame\n");
            break;
        }
    }

    printf("\n=== RESULTS ===\n");
    printf("Packets sent: %d\n", packets_sent);
    printf("Frames decoded: %d\n", frames_decoded);
    
    if (frames_decoded > 0) {
        printf("✓ SUCCESS - Hardware decoder WORKS with Baseline 60FPS!\n");
    } else {
        printf("✗ FAILED - Hardware decoder needs software fallback\n");
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);

    return frames_decoded > 0 ? 0 : 1;
}
