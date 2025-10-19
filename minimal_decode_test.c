#include <stdio.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <video>\n", argv[0]);
        return 1;
    }
    
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    const AVCodec *codec = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    int ret;
    
    printf("Opening %s...\n", argv[1]);
    ret = avformat_open_input(&fmt_ctx, argv[1], NULL, NULL);
    if (ret < 0) {
        printf("Error opening file\n");
        return 1;
    }
    
    avformat_find_stream_info(fmt_ctx, NULL);
    
    int video_stream_idx = -1;
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }
    
    printf("Found video stream %d\n", video_stream_idx);
    
    AVCodecParameters *codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
    
    // Try hardware decoder
    codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
    if (!codec) {
        printf("h264_v4l2m2m not available, using software\n");
        codec = avcodec_find_decoder(codecpar->codec_id);
    }
    
    printf("Using codec: %s\n", codec->name);
    
    codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codecpar);
    codec_ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;
    codec_ctx->thread_count = 1;
    
    printf("Extradata size: %d bytes, codec_tag: %d\n", codecpar->extradata_size, codec_ctx->codec_tag);
    
    printf("Opening codec...\n");
    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0) {
        printf("Error: %s\n", av_err2str(ret));
        return 1;
    }
    
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    
    printf("Decoding with warmup (max 64 packets)...\n");
    int pkt_count = 0;
    int frame_count = 0;
    int packets_processed = 0;
    const int max_packets = 64;
    
    while (av_read_frame(fmt_ctx, packet) >= 0 && packets_processed < max_packets) {
        if (packet->stream_index != video_stream_idx) {
            av_packet_unref(packet);
            continue;
        }
        
        packets_processed++;
        pkt_count++;
        printf("Packet %d: size=%d ", packets_processed, packet->size);
        
        ret = avcodec_send_packet(codec_ctx, packet);
        if (ret < 0) {
            printf("send error: %s\n", av_err2str(ret));
            av_packet_unref(packet);
            continue;
        }
        
        while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
            frame_count++;
            printf("-> Frame %d", frame_count);
        }
        printf("\n");
        
        av_packet_unref(packet);
        
        if (frame_count > 0) break;  // Stop after first frame
    }
    
    printf("Result: %d packets read, %d frames decoded\n", pkt_count, frame_count);
    
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    
    return frame_count > 0 ? 0 : 1;
}
