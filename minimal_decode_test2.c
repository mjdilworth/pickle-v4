#include <stdio.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    const AVCodec *codec = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    
    avformat_open_input(&fmt_ctx, argv[1], NULL, NULL);
    avformat_find_stream_info(fmt_ctx, NULL);
    
    int video_stream_idx = 0;
    AVCodecParameters *codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
    
    codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
    if (!codec) codec = avcodec_find_decoder(codecpar->codec_id);
    
    codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codecpar);
    codec_ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;
    codec_ctx->thread_count = 1;
    
    printf("Opening codec (avcC format, no BSF)...\n");
    avcodec_open2(codec_ctx, codec, NULL);
    
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    
    printf("Decoding WITHOUT warmup limit...\n");
    int frame_count = 0;
    int pkt_count = 0;
    
    while (av_read_frame(fmt_ctx, packet) >= 0 && frame_count == 0) {
        if (packet->stream_index != video_stream_idx) {
            av_packet_unref(packet);
            continue;
        }
        
        pkt_count++;
        printf("Packet %d: size=%d ", pkt_count, packet->size);
        
        avcodec_send_packet(codec_ctx, packet);
        
        while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
            frame_count++;
            printf("-> Frame %d", frame_count);
        }
        printf("\n");
        
        av_packet_unref(packet);
    }
    
    printf("Result: %d packets, %d frames\n", pkt_count, frame_count);
    
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    
    return 0;
}
