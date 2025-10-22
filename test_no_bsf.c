#include <stdio.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

int main() {
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    const AVCodec *codec = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    int ret;
    
    printf("Opening file with avcC in extradata...\n");
    ret = avformat_open_input(&fmt_ctx, "../content/rpi4-e.mp4", NULL, NULL);
    if (ret < 0) return 1;
    
    avformat_find_stream_info(fmt_ctx, NULL);
    
    int video_stream_idx = -1;
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }
    
    AVCodecParameters *codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
    codec = avcodec_find_decoder(codecpar->codec_id);
    printf("Codec: %s\n", codec->name);
    printf("Extradata size: %d bytes\n", codecpar->extradata_size);
    
    codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codecpar);
    
    // Try WITHOUT codec_tag = 0, let it use default
    printf("NOT setting codec_tag=0, letting it use default: %d\n", codec_ctx->codec_tag);
    
    codec_ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;
    codec_ctx->thread_count = 1;
    
    printf("Opening codec (with avcC extradata)...\n");
    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0) {
        printf("Error: %s\n", av_err2str(ret));
        return 1;
    }
    
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    
    printf("Decoding without BSF chain...\n");
    int pkt_count = 0;
    int frame_count = 0;
    
    while (av_read_frame(fmt_ctx, packet) >= 0 && pkt_count < 20) {
        if (packet->stream_index != video_stream_idx) {
            av_packet_unref(packet);
            continue;
        }
        
        pkt_count++;
        printf("Packet %d: size=%d ", pkt_count, packet->size);
        
        ret = avcodec_send_packet(codec_ctx, packet);
        if (ret < 0) printf("send error: %s ", av_err2str(ret));
        
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
