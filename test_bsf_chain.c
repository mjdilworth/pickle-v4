#include <stdio.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>

int main() {
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    const AVCodec *codec = NULL;
    AVBSFContext *bsf_annexb = NULL, *bsf_aud = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    int ret;
    
    printf("Opening file...\n");
    ret = avformat_open_input(&fmt_ctx, "../content/rpi4-e.mp4", NULL, NULL);
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
    
    printf("Setting up BSF chain...\n");
    
    // Stage 1: h264_mp4toannexb
    const AVBitStreamFilter *filter1 = av_bsf_get_by_name("h264_mp4toannexb");
    av_bsf_alloc(filter1, &bsf_annexb);
    avcodec_parameters_copy(bsf_annexb->par_in, fmt_ctx->streams[video_stream_idx]->codecpar);
    av_bsf_init(bsf_annexb);
    printf("✓ Stage 1 init'd\n");
    
    // Stage 2: h264_metadata
    const AVBitStreamFilter *filter2 = av_bsf_get_by_name("h264_metadata");
    av_bsf_alloc(filter2, &bsf_aud);
    avcodec_parameters_copy(bsf_aud->par_in, bsf_annexb->par_out);
    av_opt_set(bsf_aud, "aud", "insert", 0);
    av_bsf_init(bsf_aud);
    printf("✓ Stage 2 init'd\n");
    
    // Open decoder  
    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, bsf_aud->par_out);
    codec_ctx->codec_tag = 0;
    codec_ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;
    codec_ctx->thread_count = 1;
    
    printf("Opening decoder...\n");
    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0) {
        printf("Error: %s\n", av_err2str(ret));
        return 1;
    }
    
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    
    printf("Processing packets with BSF chain...\n");
    int pkt_count = 0;
    int frame_count = 0;
    
    while (av_read_frame(fmt_ctx, packet) >= 0 && pkt_count < 100) {
        if (packet->stream_index != video_stream_idx) {
            av_packet_unref(packet);
            continue;
        }
        
        pkt_count++;
        
        // Stage 1
        AVPacket stage1_out = {0};
        av_bsf_send_packet(bsf_annexb, packet);
        if (av_bsf_receive_packet(bsf_annexb, &stage1_out) < 0) {
            printf("P%d: Stage 1 needs more data\n", pkt_count);
            av_packet_unref(packet);
            continue;
        }
        
        // Stage 2
        AVPacket stage2_out = {0};
        av_bsf_send_packet(bsf_aud, &stage1_out);
        if (av_bsf_receive_packet(bsf_aud, &stage2_out) < 0) {
            printf("P%d: Stage 2 needs more data\n", pkt_count);
            av_packet_unref(&stage1_out);
            av_packet_unref(packet);
            continue;
        }
        
        printf("Packet %d: size=%d ", pkt_count, stage2_out.size);
        
        // Check for AUD
        if (stage2_out.size >= 5) {
            uint8_t nal_type = stage2_out.data[4] & 0x1f;
            printf("NAL=0x%02x ", nal_type);
        }
        
        // Send to decoder
        ret = avcodec_send_packet(codec_ctx, &stage2_out);
        
        while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
            frame_count++;
            printf("-> Frame %d", frame_count);
        }
        printf("\n");
        
        av_packet_unref(&stage2_out);
        av_packet_unref(&stage1_out);
        av_packet_unref(packet);
    }
    
    printf("Total: %d packets, %d frames\n", pkt_count, frame_count);
    
    av_frame_free(&frame);
    av_packet_free(&packet);
    av_bsf_free(&bsf_aud);
    av_bsf_free(&bsf_annexb);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    
    return 0;
}
