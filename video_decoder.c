#define _GNU_SOURCE
#include "video_decoder.h"
#include "v4l2_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <libavutil/buffer.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>

typedef struct {
    const uint8_t *data;
    int length;
    int type;
} annexb_nal_t;

typedef struct {
    bool has_aud;
    bool has_sps;
    bool has_pps;
    bool has_sei;
    bool has_idr;
    bool has_slice;
} au_metadata_t;

typedef struct {
    const uint8_t *data;
    int size;
    int bit_pos;
} bit_reader_t;

static int annexb_start_code_size(const uint8_t *data, int remaining);
static int annexb_nal_type(const uint8_t *data, int length);
static int annexb_split_nals(const uint8_t *data, int size, annexb_nal_t *out, int max_nals);

static void bit_reader_init(bit_reader_t *br, const uint8_t *data, int size)
{
    br->data = data;
    br->size = size;
    br->bit_pos = 0;
}

static int bit_reader_remaining(const bit_reader_t *br)
{
    return (br->size * 8) - br->bit_pos;
}

static int bit_reader_read_bit(bit_reader_t *br, uint32_t *bit)
{
    if (!br || !bit || bit_reader_remaining(br) <= 0) {
        return -1;
    }

    int byte_index = br->bit_pos >> 3;
    int bit_index = 7 - (br->bit_pos & 7);
    *bit = (br->data[byte_index] >> bit_index) & 0x01;
    br->bit_pos++;
    return 0;
}

static int bit_reader_read_bits(bit_reader_t *br, int count, uint32_t *value)
{
    if (!br || !value || count <= 0 || bit_reader_remaining(br) < count || count > 32) {
        return -1;
    }

    uint32_t result = 0;
    for (int i = 0; i < count; i++) {
        uint32_t bit;
        if (bit_reader_read_bit(br, &bit) < 0) {
            return -1;
        }
        result = (result << 1) | bit;
    }

    *value = result;
    return 0;
}

static int bit_reader_read_ue(bit_reader_t *br, uint32_t *value)
{
    if (!br || !value) {
        return -1;
    }

    int leading_zero_bits = 0;
    uint32_t bit = 0;

    while (1) {
        if (bit_reader_read_bit(br, &bit) < 0) {
            return -1;
        }
        if (bit == 0) {
            leading_zero_bits++;
            if (leading_zero_bits > 31) {
                return -1;
            }
        } else {
            break;
        }
    }

    if (leading_zero_bits == 0) {
        *value = 0;
        return 0;
    }

    uint32_t suffix = 0;
    if (bit_reader_read_bits(br, leading_zero_bits, &suffix) < 0) {
        return -1;
    }

    *value = ((1u << leading_zero_bits) - 1u) + suffix;
    return 0;
}

static int h264_extract_rbsp(const uint8_t *data, int size, uint8_t **rbsp_out)
{
    if (!data || size <= 0 || !rbsp_out) {
        return AVERROR(EINVAL);
    }

    uint8_t *rbsp = av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!rbsp) {
        return AVERROR(ENOMEM);
    }

    int rbsp_size = 0;
    for (int i = 0; i < size; i++) {
        if (i + 2 < size && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x03) {
            rbsp[rbsp_size++] = 0x00;
            rbsp[rbsp_size++] = 0x00;
            i += 2;
        } else {
            rbsp[rbsp_size++] = data[i];
        }
    }

    memset(rbsp + rbsp_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    *rbsp_out = rbsp;
    return rbsp_size;
}

static int h264_get_first_mb_in_slice(const uint8_t *nal_data, int nal_size, int *first_mb_out)
{
    if (!nal_data || nal_size <= 0) {
        return AVERROR(EINVAL);
    }

    int start_len = annexb_start_code_size(nal_data, nal_size);
    if (start_len <= 0 || start_len >= nal_size) {
        return AVERROR_INVALIDDATA;
    }

    const uint8_t *payload = nal_data + start_len;
    int payload_size = nal_size - start_len;
    if (payload_size <= 0) {
        return AVERROR_INVALIDDATA;
    }

    uint8_t nal_header = payload[0];
    int nal_type = nal_header & 0x1F;
    if (nal_type != 1 && nal_type != 5) {
        return AVERROR_INVALIDDATA;
    }

    const uint8_t *rbsp_src = payload + 1;
    int rbsp_src_size = payload_size - 1;
    if (rbsp_src_size <= 0) {
        return AVERROR_INVALIDDATA;
    }

    uint8_t *rbsp = NULL;
    int rbsp_size = h264_extract_rbsp(rbsp_src, rbsp_src_size, &rbsp);
    if (rbsp_size < 0) {
        return rbsp_size;
    }

    bit_reader_t br;
    bit_reader_init(&br, rbsp, rbsp_size);

    uint32_t first_mb = 0;
    int ret = bit_reader_read_ue(&br, &first_mb);
    av_free(rbsp);

    if (ret < 0) {
        return AVERROR_INVALIDDATA;
    }

    if (first_mb_out) {
        *first_mb_out = (int)first_mb;
    }

    return 0;
}

static bool h264_slice_starts_new_picture(video_context_t *video, const annexb_nal_t *nal)
{
    (void)video;
    if (!nal || !nal->data || nal->length <= 0) {
        return false;
    }

    if (nal->type != 1 && nal->type != 5) {
        return false;
    }

    int first_mb = 0;
    if (h264_get_first_mb_in_slice(nal->data, nal->length, &first_mb) < 0) {
        return false;
    }

    return first_mb == 0;
}

static int annexb_start_code_size(const uint8_t *data, int remaining)
{
    if (!data || remaining < 3) {
        return 0;
    }

    if (remaining >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) {
        return 4;
    }

    if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) {
        return 3;
    }

    return 0;
}

static int annexb_nal_type(const uint8_t *data, int length)
{
    int start_code = annexb_start_code_size(data, length);
    if (start_code == 0 || start_code >= length) {
        return -1;
    }

    return data[start_code] & 0x1F;
}

static int annexb_split_nals(const uint8_t *data, int size, annexb_nal_t *out, int max_nals)
{
    if (!data || !out || max_nals <= 0) {
        return 0;
    }

    int count = 0;
    int index = 0;

    while (index < size && count < max_nals) {
        int start_len = annexb_start_code_size(data + index, size - index);
        if (start_len == 0) {
            index++;
            continue;
        }

        int nal_start = index;
        index += start_len;

        int next = index;
        while (next < size) {
            int next_len = annexb_start_code_size(data + next, size - next);
            if (next_len > 0) {
                break;
            }
            next++;
        }

        int nal_length = next - nal_start;
        if (nal_length > 0) {
            out[count].data = data + nal_start;
            out[count].length = nal_length;
            out[count].type = annexb_nal_type(out[count].data, out[count].length);
            count++;
        }

        index = next;
    }

    return count;
}

static bool reorder_first_access_unit_packet(AVPacket *pkt)
{
    if (!pkt || !pkt->data || pkt->size <= 0) {
        return false;
    }

    const int MAX_NALS = 64;
    annexb_nal_t nals[MAX_NALS];
    int nal_count = annexb_split_nals(pkt->data, pkt->size, nals, MAX_NALS);

    if (nal_count <= 0) {
        return false;
    }

    bool has_idr = false;
    for (int i = 0; i < nal_count; i++) {
        if (nals[i].type == 5) {
            has_idr = true;
            break;
        }
    }

    if (!has_idr) {
        return false;
    }

    const annexb_nal_t *ordered[MAX_NALS];
    int ordered_count = 0;

    // Access Unit Delimiters first (type 9)
    for (int i = 0; i < nal_count; i++) {
        if (nals[i].type == 9) {
            ordered[ordered_count++] = &nals[i];
        }
    }

    // SPS (type 7)
    for (int i = 0; i < nal_count; i++) {
        if (nals[i].type == 7) {
            ordered[ordered_count++] = &nals[i];
        }
    }

    // PPS (type 8)
    for (int i = 0; i < nal_count; i++) {
        if (nals[i].type == 8) {
            ordered[ordered_count++] = &nals[i];
        }
    }

    // SEI (type 6)
    for (int i = 0; i < nal_count; i++) {
        if (nals[i].type == 6) {
            ordered[ordered_count++] = &nals[i];
        }
    }

    // IDR slices (type 5)
    for (int i = 0; i < nal_count; i++) {
        if (nals[i].type == 5) {
            ordered[ordered_count++] = &nals[i];
        }
    }

    // Remaining NAL types preserve original order
    for (int i = 0; i < nal_count; i++) {
        int type = nals[i].type;
        if (type != 9 && type != 7 && type != 8 && type != 6 && type != 5) {
            ordered[ordered_count++] = &nals[i];
        }
    }

    if (ordered_count != nal_count) {
        return false;
    }

    bool already_ordered = true;
    for (int i = 0; i < nal_count; i++) {
        if (ordered[i] != &nals[i]) {
            already_ordered = false;
            break;
        }
    }

    if (already_ordered) {
        return false;
    }

    int total_size = 0;
    for (int i = 0; i < ordered_count; i++) {
        total_size += ordered[i]->length;
    }

    if (total_size != pkt->size) {
        return false;
    }

    AVBufferRef *buffer = av_buffer_alloc(total_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!buffer) {
        return false;
    }

    uint8_t *dst = buffer->data;
    int offset = 0;
    for (int i = 0; i < ordered_count; i++) {
        memcpy(dst + offset, ordered[i]->data, ordered[i]->length);
        offset += ordered[i]->length;
    }
    memset(dst + total_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    av_buffer_unref(&pkt->buf);
    pkt->buf = buffer;
    pkt->data = buffer->data;
    pkt->size = total_size;

    return true;
}

static void reset_current_access_unit(video_context_t *video)
{
    if (!video) {
        return;
    }

    video->au_buffer_size = 0;
    video->au_pts = AV_NOPTS_VALUE;
    video->au_dts = AV_NOPTS_VALUE;
    video->au_flags = 0;
    video->au_has_pts = false;
    video->au_has_dts = false;
    video->au_has_aud = false;
    video->au_has_sps = false;
    video->au_has_pps = false;
    video->au_has_sei = false;
    video->au_has_idr = false;
    video->au_has_slice = false;
}

static int ensure_access_unit_capacity(video_context_t *video, int additional)
{
    if (!video || additional <= 0) {
        return 0;
    }

    int required = video->au_buffer_size + additional;
    if (required <= video->au_buffer_capacity) {
        return 0;
    }

    int new_capacity = video->au_buffer_capacity > 0 ? video->au_buffer_capacity : 4096;
    while (new_capacity < required) {
        if (new_capacity > INT_MAX / 2) {
            new_capacity = required;
            break;
        }
        new_capacity *= 2;
    }

    uint8_t *new_buffer = av_realloc(video->au_buffer, (size_t)new_capacity + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!new_buffer) {
        return AVERROR(ENOMEM);
    }

    video->au_buffer = new_buffer;
    video->au_buffer_capacity = new_capacity;
    return 0;
}

static int append_to_current_access_unit(video_context_t *video, const uint8_t *data, int size, const AVPacket *source)
{
    if (!video || !data || size <= 0) {
        return 0;
    }

    int ret = ensure_access_unit_capacity(video, size);
    if (ret < 0) {
        return ret;
    }

    if (video->au_buffer_size == 0) {
        video->au_flags = 0;
        video->au_pts = AV_NOPTS_VALUE;
        video->au_dts = AV_NOPTS_VALUE;
        video->au_has_pts = false;
        video->au_has_dts = false;
    }

    memcpy(video->au_buffer + video->au_buffer_size, data, size);
    video->au_buffer_size += size;
    memset(video->au_buffer + video->au_buffer_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    if (source) {
        if (source->pts != AV_NOPTS_VALUE) {
            if (!video->au_has_pts) {
                video->au_pts = source->pts;
                video->au_has_pts = true;
            }
        }

        if (source->dts != AV_NOPTS_VALUE) {
            if (!video->au_has_dts) {
                video->au_dts = source->dts;
                video->au_has_dts = true;
            }
        }

        video->au_flags |= source->flags;
    }

    return 0;
}

static void update_current_access_unit_flags(video_context_t *video, int nal_type)
{
    if (!video) {
        return;
    }

    switch (nal_type) {
        case 1:
            video->au_has_slice = true;
            break;
        case 5:
            video->au_has_idr = true;
            video->au_has_slice = true;
            break;
        case 6:
            video->au_has_sei = true;
            break;
        case 7:
            video->au_has_sps = true;
            break;
        case 8:
            video->au_has_pps = true;
            break;
        case 9:
            video->au_has_aud = true;
            break;
        default:
            break;
    }
}

static AVPacket* finalize_current_access_unit(video_context_t *video, au_metadata_t *meta_out)
{
    if (!video || !video->au_buffer || video->au_buffer_size <= 0) {
        return NULL;
    }

    if (meta_out) {
        meta_out->has_aud = video->au_has_aud;
        meta_out->has_sps = video->au_has_sps;
        meta_out->has_pps = video->au_has_pps;
        meta_out->has_sei = video->au_has_sei;
        meta_out->has_idr = video->au_has_idr;
        meta_out->has_slice = video->au_has_slice;
    }

    AVBufferRef *buffer = av_buffer_alloc(video->au_buffer_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!buffer) {
        return NULL;
    }

    memcpy(buffer->data, video->au_buffer, video->au_buffer_size);
    memset(buffer->data + video->au_buffer_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        av_buffer_unref(&buffer);
        return NULL;
    }

    pkt->buf = buffer;
    pkt->data = buffer->data;
    pkt->size = video->au_buffer_size;
    pkt->pts = video->au_has_pts ? video->au_pts : AV_NOPTS_VALUE;
    pkt->dts = video->au_has_dts ? video->au_dts : AV_NOPTS_VALUE;
    pkt->flags = video->au_flags;

    reset_current_access_unit(video);
    return pkt;
}

static int process_completed_access_unit(video_context_t *video,
                                         AVPacket **stored_sps_pps,
                                         bool *first_idr_found,
                                         int decode_call_count,
                                         int *packet_count)
{
    if (!video) {
        return 0;
    }

    au_metadata_t meta = {0};
    AVPacket *au_pkt = finalize_current_access_unit(video, &meta);
    if (!au_pkt) {
        return 0;
    }

    if (packet_count) {
        (*packet_count)++;
    }
    int packet_index = packet_count ? *packet_count : 0;

    if (packet_index <= 10) {
        printf("[DEBUG] Access Unit #%d: size=%d, AUD=%d, SPS=%d, PPS=%d, IDR=%d, Slice=%d, SEI=%d\n",
               packet_index, au_pkt->size, meta.has_aud, meta.has_sps, meta.has_pps,
               meta.has_idr, meta.has_slice, meta.has_sei);
    }

    if ((meta.has_sps || meta.has_pps) && !meta.has_idr && !meta.has_slice) {
        if (*stored_sps_pps) {
            av_packet_free(stored_sps_pps);
        }
        *stored_sps_pps = av_packet_clone(au_pkt);
        printf("[INFO] Stored SPS/PPS access unit for later prefixing\n");
        av_packet_free(&au_pkt);
        return 0;
    }

    if (first_idr_found && !*first_idr_found) {
        // Assert that first AU has both SPS/PPS and IDR
        if (!meta.has_idr || (!meta.has_sps && !meta.has_pps && !*stored_sps_pps)) {
            if (packet_index <= 20) {
                if (!meta.has_idr) {
                    printf("[DEBUG] Dropping access unit #%d (waiting for first IDR)\n", packet_index);
                } else {
                    printf("[DEBUG] Buffering access unit #%d (waiting for SPS/PPS before first IDR)\n", packet_index);
                }
            }
            av_packet_free(&au_pkt);
            return 0;
        }

        printf("[INFO] Found first IDR in access unit #%d\n", packet_index);
        *first_idr_found = true;
        video->first_idr_seen = true;

        // Only prepend SPS/PPS if the access unit doesn't already have them
        if (*stored_sps_pps && !meta.has_sps && !meta.has_pps) {
            printf("[INFO] Prefixing stored SPS/PPS before IDR access unit\n");
            AVPacket *combined = av_packet_alloc();
            if (combined) {
                combined->size = (*stored_sps_pps)->size + au_pkt->size;
                AVBufferRef *buf = av_buffer_alloc(combined->size + AV_INPUT_BUFFER_PADDING_SIZE);
                if (buf) {
                    memcpy(buf->data, (*stored_sps_pps)->data, (*stored_sps_pps)->size);
                    memcpy(buf->data + (*stored_sps_pps)->size, au_pkt->data, au_pkt->size);
                    memset(buf->data + combined->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                    combined->buf = buf;
                    combined->data = buf->data;
                    combined->pts = au_pkt->pts;
                    combined->dts = au_pkt->dts;
                    combined->flags = au_pkt->flags;
                    av_packet_free(&au_pkt);
                    au_pkt = combined;
                    printf("[INFO] Created combined access unit with SPS/PPS + IDR: size=%d\n", au_pkt->size);
                } else {
                    av_packet_free(&combined);
                }
            }
        } else if (meta.has_sps || meta.has_pps) {
            printf("[INFO] IDR access unit #%d already has SPS/PPS, not prepending\n", packet_index);
        }

        av_packet_free(stored_sps_pps);
        *stored_sps_pps = NULL;
    }

    if (!video->first_au_reordered && meta.has_idr) {
        bool reordered = reorder_first_access_unit_packet(au_pkt);
        if (reordered) {
            printf("[INFO] Reordered first access unit NAL order to AUD/SPS/PPS/SEI/IDR\n");
        } else {
            printf("[DEBUG] First access unit already in desired NAL order\n");
        }
        video->first_au_reordered = true;
    }

    if (meta.has_idr) {
        au_pkt->flags |= AV_PKT_FLAG_KEY;
    }

    int send_result = avcodec_send_packet(video->codec_ctx, au_pkt);
    static int consecutive_invaliddata_errors = 0;
    static int last_reported_error_count = 0;
    
    if (send_result == AVERROR_INVALIDDATA) {
        // Monitor INVALIDDATA errors after IDR frames
        consecutive_invaliddata_errors++;
        
        // Report warnings when consecutive errors occur
        if (*first_idr_found && 
            consecutive_invaliddata_errors >= 5 && 
            (consecutive_invaliddata_errors % 5 == 0) && 
            consecutive_invaliddata_errors != last_reported_error_count) {
            printf("[WARNING] Decoder has rejected %d access units with INVALIDDATA since last successful decode\n", 
                   consecutive_invaliddata_errors);
            last_reported_error_count = consecutive_invaliddata_errors;
        }
        
        if (packet_index <= 20) {
            printf("[WARN] Decoder rejected access unit #%d with INVALIDDATA (partial AU suspected)\n", packet_index);
        }
    } else if (send_result == AVERROR(EAGAIN)) {
        if (decode_call_count <= 5) {
            printf("[INFO] Decoder signaled EAGAIN while sending access unit #%d\n", packet_index);
        }
        av_packet_free(&au_pkt);

        int receive_result = avcodec_receive_frame(video->codec_ctx, video->frame);
        if (receive_result == 0) {
            printf("[SUCCESS] Got frame after draining decoder!\n");
            return 1;
        }
        return 0;
    } else if (send_result < 0) {
        if (decode_call_count <= 5) {
            printf("[ERROR] Failed to send access unit: %s\n", av_err2str(send_result));
        }
    } else {
        // Reset error counter on successful send
        consecutive_invaliddata_errors = 0;
        
        if (packet_index <= 10) {
            printf("[SUCCESS] Decoder accepted access unit #%d\n", packet_index);
        }
    }

    av_packet_free(&au_pkt);

    int receive_result = avcodec_receive_frame(video->codec_ctx, video->frame);
    if (receive_result == 0) {
        printf("[SUCCESS] Got frame from decoder!\n");
        return 1;
    } else if (receive_result == AVERROR(EAGAIN)) {
        if (packet_index <= 10) {
            printf("[INFO] Decoder needs more access units\n");
        }
        return 0;
    } else if (receive_result == AVERROR_EOF) {
        video->eof_reached = true;
        return -1;
    } else if (receive_result < 0) {
        if (decode_call_count <= 5) {
            printf("[ERROR] Error receiving frame: %s\n", av_err2str(receive_result));
        }
    }

    return 0;
}

static int ingest_metadata_packet(video_context_t *video,
                                  AVPacket *packet,
                                  AVPacket **stored_sps_pps,
                                  bool *first_idr_found,
                                  int decode_call_count,
                                  int *packet_count)
{
    if (!video || !packet || packet->size <= 0) {
        return 0;
    }

    const int MAX_NALS = 128;
    annexb_nal_t nals[MAX_NALS];
    int nal_count = annexb_split_nals(packet->data, packet->size, nals, MAX_NALS);
    if (nal_count <= 0) {
        return 0;
    }

    for (int i = 0; i < nal_count; i++) {
        const int nal_type = nals[i].type;

        if (nal_type == 9) { // AUD
            // AUD marks the start of a new access unit
            // If we have accumulated slices, flush the previous AU first
            if (video->au_has_slice) {
                int au_result = process_completed_access_unit(video, stored_sps_pps, first_idr_found, decode_call_count, packet_count);
                if (au_result != 0) {
                    return au_result;
                }
            }

            // Start new AU with the AUD
            int ret = append_to_current_access_unit(video, nals[i].data, nals[i].length, packet);
            if (ret < 0) {
                return ret;
            }

            update_current_access_unit_flags(video, nal_type);
            continue;
        }

        if (nal_type == 7 || nal_type == 8 || nal_type == 6) { // SPS, PPS, SEI
            // These can appear before or within an AU
            int ret = append_to_current_access_unit(video, nals[i].data, nals[i].length, packet);
            if (ret < 0) {
                return ret;
            }

            update_current_access_unit_flags(video, nal_type);
            continue;
        }

        if (nal_type == 1 || nal_type == 5) { // VCL slice NALs
            bool starts_new_picture = h264_slice_starts_new_picture(video, &nals[i]);

            // Only flush if this slice starts a new picture AND we already have slices
            if (video->au_has_slice && starts_new_picture && !video->au_has_aud) {
                // No AUD present but new picture detected - flush previous AU
                int au_result = process_completed_access_unit(video, stored_sps_pps, first_idr_found, decode_call_count, packet_count);
                if (au_result != 0) {
                    return au_result;
                }
            }

            // Add slice to current AU (accumulate all slices of the same picture)
            int ret = append_to_current_access_unit(video, nals[i].data, nals[i].length, packet);
            if (ret < 0) {
                return ret;
            }

            update_current_access_unit_flags(video, nal_type);
            continue;
        }

        // Default: append other NAL types without special handling
        int ret = append_to_current_access_unit(video, nals[i].data, nals[i].length, packet);
        if (ret < 0) {
            return ret;
        }

        update_current_access_unit_flags(video, nal_type);
    }

    // Don't flush here - wait for next AUD or picture boundary
    return 0;
}

static int flush_pending_access_unit(video_context_t *video,
                                     AVPacket **stored_sps_pps,
                                     bool *first_idr_found,
                                     int decode_call_count,
                                     int *packet_count)
{
    if (!video || video->au_buffer_size <= 0) {
        return 0;
    }

    return process_completed_access_unit(video, stored_sps_pps, first_idr_found, decode_call_count, packet_count);
}

// This function selects an appropriate pixel format from the list offered by the decoder
// It accepts multiple hardware-friendly formats (DRM_PRIME, NV12, YUV420P) without forcing a specific one
// This flexibility allows the V4L2 hardware decoder to work with its preferred format
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
    video_context_t *video = ctx->opaque;
    const enum AVPixelFormat *p;
    
    // Look for any hardware formats we can use directly - accepting multiple options
    // Order of preference: DRM_PRIME > NV12 > YUV420P, but any of these will work
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_DRM_PRIME || 
            *p == AV_PIX_FMT_NV12 ||
            *p == AV_PIX_FMT_YUV420P) {
            
            // Store this format for later reference
            video->hw_pix_fmt = *p;
            printf("[INFO] Selected hardware-compatible pixel format: %s\n", av_get_pix_fmt_name(video->hw_pix_fmt));
            return *p;
        }
    }
    
    // Fallback: take whatever format is first in the list
    video->hw_pix_fmt = pix_fmts[0];
    printf("[WARN] None of our preferred formats available. Using fallback: %s\n", 
           av_get_pix_fmt_name(video->hw_pix_fmt));
    
    // Debug: print all available formats offered by the decoder
    printf("[DEBUG] Available pixel formats offered by decoder:\n");
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        printf("[DEBUG]   - %s\n", av_get_pix_fmt_name(*p));
    }
    
    return pix_fmts[0];
}

int video_init(video_context_t *video, const char *filename) {
    memset(video, 0, sizeof(*video));
    reset_current_access_unit(video);

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
        // Store video context pointer in codec context for the get_format callback
        video->codec_ctx->opaque = video;
        
        // Set our get_format callback to accept multiple hardware-friendly formats
        video->codec_ctx->get_format = get_hw_format;
        
        // Don't force a specific pixel format - let the decoder choose its preferred format
        // This improves compatibility with different hardware decoder variants
        video->codec_ctx->thread_count = 1; // V4L2 handles threading internally
        printf("V4L2 M2M configured - accepting ANY of DRM_PRIME, NV12, or YUV420P formats\n");
        
        // IMPROVED APPROACH: Set up BSF chain for H.264
        if (codecpar->codec_id == AV_CODEC_ID_H264) {
            printf("[INFO] Setting up H.264 BSF chain for optimal V4L2 M2M compatibility\n");
            
            // Debug: Check extradata before BSF setup
            if (codecpar->extradata && codecpar->extradata_size > 0) {
                printf("[DEBUG] Stream has extradata: %d bytes\n", codecpar->extradata_size);
                printf("[DEBUG] Extradata first 16 bytes: ");
                for (int i = 0; i < 16 && i < codecpar->extradata_size; i++) {
                    printf("%02x ", codecpar->extradata[i]);
                }
                printf("\n");
                
                // Check if extradata looks like MP4 format (starts with version and length size minus one)
                if (codecpar->extradata_size > 6 && codecpar->extradata[0] == 0x01) {
                    printf("[DEBUG] Extradata appears to be in MP4/AVCC format (starts with 0x01)\n");
                    printf("[DEBUG]   Profile: %02x, Compat: %02x, Level: %02x\n",
                           codecpar->extradata[1], codecpar->extradata[2], codecpar->extradata[3]);
                }
            } else {
                printf("[WARN] No extradata found in stream - SPS/PPS may need to be extracted from packets\n");
            }
            
            if (init_h264_bsf_chain(&video->bsf_chain, codecpar) == 0) {
                video->use_bsf_chain = true;
                video->first_idr_seen = false; // We'll set this when we find the first IDR
                video->first_au_reordered = false;
                printf("[INFO] Using BSF: h264_mp4toannexb\n");
                printf("[INFO] h264_mp4toannexb will convert AVCC to Annex-B and extract SPS/PPS\n");
                
                // Initialize H.264 parser to ensure complete access units
                video->parser = av_parser_init(AV_CODEC_ID_H264);
                if (video->parser) {
                    video->use_parser = true;
                    printf("[INFO] H.264 parser initialized to ensure complete access units\n");
                    // Set parser flags for proper handling
                    video->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
                } else {
                    video->use_parser = false;
                    printf("[WARN] Failed to initialize H.264 parser, may get incomplete access units\n");
                }
            } else {
                // If BSF setup fails, we can't use hardware decoding properly
                video->use_bsf_chain = false;
                video->use_parser = false;
                printf("[WARN] BSF chain setup failed, hardware decoding may not work correctly\n");
            }
        }
    } else {
        // Software decoding settings
        video->codec_ctx->thread_count = 4;
        video->codec_ctx->thread_type = FF_THREAD_FRAME;
    }
    
    // Force Annex-B mode (remove avcC extradata) for better hardware decoder compatibility
    if (video->use_hardware_decode && video->codec_ctx->extradata && video->codec_ctx->extradata_size > 0) {
        printf("[INFO] Forcing Annex-B mode - removing avcC extradata for hardware decoder\n");
        // Save extradata for later use if needed
        uint8_t *saved_extradata = av_mallocz(video->codec_ctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (saved_extradata) {
            memcpy(saved_extradata, video->codec_ctx->extradata, video->codec_ctx->extradata_size);
            
            // Remove extradata from codec context
            av_freep(&video->codec_ctx->extradata);
            video->codec_ctx->extradata = NULL;
            video->codec_ctx->extradata_size = 0;
            
            // Set codec flags to expect Annex-B format
            video->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            video->codec_ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;
        }
    }

    // Open codec
    if (avcodec_open2(video->codec_ctx, video->codec, NULL) < 0) {
        fprintf(stderr, "Failed to open codec\n");
        avcodec_free_context(&video->codec_ctx);
        avformat_close_input(&video->format_ctx);
        av_packet_free(&video->packet);
        return -1;
    }

    // For hardware decoding, manually send extradata as first packet if present
    if (video->use_hardware_decode && codecpar->extradata && codecpar->extradata_size > 0 && video->use_bsf_chain) {
        printf("[INFO] Processing extradata through BSF for SPS/PPS extraction\n");
        
        // The h264_mp4toannexb BSF needs a proper packet with copied data
        AVPacket *init_pkt = av_packet_alloc();
        if (init_pkt) {
            // Allocate buffer and copy extradata (BSF needs its own copy)
            uint8_t *data = av_malloc(codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (data) {
                memcpy(data, codecpar->extradata, codecpar->extradata_size);
                memset(data + codecpar->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                
                init_pkt->data = data;
                init_pkt->size = codecpar->extradata_size;
                init_pkt->flags = AV_PKT_FLAG_KEY;
                
                // Send through BSF to extract and convert SPS/PPS
                printf("[DEBUG] Sending %d bytes of extradata to BSF\n", init_pkt->size);
                int ret = av_bsf_send_packet(video->bsf_chain.mp4toannexb_ctx, init_pkt);
                if (ret < 0) {
                    printf("[WARN] Failed to send extradata to BSF: %s\n", av_err2str(ret));
                } else {
                    // Receive and send all output packets from BSF
                    AVPacket *out = av_packet_alloc();
                    int pkt_count = 0;
                    
                    while (out && av_bsf_receive_packet(video->bsf_chain.mp4toannexb_ctx, out) == 0) {
                        printf("[DEBUG] BSF produced packet %d from extradata: size=%d\n", pkt_count, out->size);
                        
                        // Analyze what the BSF produced
                        if (out->size >= 5) {
                            printf("[DEBUG]   First 5 bytes: %02x %02x %02x %02x %02x\n",
                                   out->data[0], out->data[1], out->data[2], out->data[3], out->data[4]);
                            
                            // Check NAL type
                            int nal_offset = 0;
                            if (out->data[0] == 0 && out->data[1] == 0) {
                                if (out->data[2] == 1) nal_offset = 3;
                                else if (out->data[2] == 0 && out->data[3] == 1) nal_offset = 4;
                                
                                if (nal_offset > 0 && out->size > nal_offset) {
                                    uint8_t nal_type = out->data[nal_offset] & 0x1f;
                                    const char *type_name = "Unknown";
                                    switch(nal_type) {
                                        case 7: type_name = "SPS"; break;
                                        case 8: type_name = "PPS"; break;
                                        default: break;
                                    }
                                    printf("[DEBUG]   NAL type: %d (%s)\n", nal_type, type_name);
                                }
                            }
                        }
                        
                        // Send to decoder
                        ret = avcodec_send_packet(video->codec_ctx, out);
                        if (ret < 0 && ret != AVERROR(EAGAIN)) {
                            printf("[WARN] Decoder rejected initialization packet %d: %s\n", pkt_count, av_err2str(ret));
                        } else if (ret == 0) {
                            printf("[INFO] Decoder accepted initialization packet %d\n", pkt_count);
                        }
                        
                        av_packet_unref(out);
                        pkt_count++;
                    }
                    
                    if (pkt_count == 0) {
                        printf("[WARN] BSF did not produce any packets from extradata\n");
                        
                        // Try to manually construct SPS/PPS packets from AVCC extradata
                        if (codecpar->extradata_size > 8 && codecpar->extradata[0] == 0x01) {
                            printf("[INFO] Attempting manual SPS/PPS extraction from AVCC format\n");
                            
                            uint8_t *p = codecpar->extradata + 5; // Skip AVCC header
                            int remaining = codecpar->extradata_size - 5;
                            
                            // Number of SPS NALUs
                            if (remaining >= 1) {
                                int num_sps = (*p & 0x1f);
                                p++;
                                remaining--;
                                
                                printf("[DEBUG] Number of SPS: %d\n", num_sps);
                                
                                // Process each SPS
                                for (int i = 0; i < num_sps && remaining >= 2; i++) {
                                    uint16_t sps_size = (p[0] << 8) | p[1];
                                    p += 2;
                                    remaining -= 2;
                                    
                                    if (remaining >= sps_size) {
                                        printf("[DEBUG] SPS %d: size=%d\n", i, sps_size);
                                        
                                        // Create Annex-B packet with SPS
                                        AVPacket *sps_packet = av_packet_alloc();
                                        if (sps_packet) {
                                            uint8_t *annex_b = av_malloc(sps_size + 4 + AV_INPUT_BUFFER_PADDING_SIZE);
                                            if (annex_b) {
                                                annex_b[0] = 0x00;
                                                annex_b[1] = 0x00;
                                                annex_b[2] = 0x00;
                                                annex_b[3] = 0x01;
                                                memcpy(annex_b + 4, p, sps_size);
                                                memset(annex_b + 4 + sps_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                                                
                                                av_packet_from_data(sps_packet, annex_b, sps_size + 4);
                                                
                                                ret = avcodec_send_packet(video->codec_ctx, sps_packet);
                                                printf("[INFO] Sent SPS to decoder: %s\n", 
                                                       (ret == 0) ? "accepted" : av_err2str(ret));
                                                
                                                av_packet_free(&sps_packet);
                                            } else {
                                                av_packet_free(&sps_packet);
                                            }
                                        }
                                        
                                        p += sps_size;
                                        remaining -= sps_size;
                                    }
                                }
                                
                                // Number of PPS NALUs
                                if (remaining >= 1) {
                                    int num_pps = *p;
                                    p++;
                                    remaining--;
                                    
                                    printf("[DEBUG] Number of PPS: %d\n", num_pps);
                                    
                                    // Process each PPS
                                    for (int i = 0; i < num_pps && remaining >= 2; i++) {
                                        uint16_t pps_size = (p[0] << 8) | p[1];
                                        p += 2;
                                        remaining -= 2;
                                        
                                        if (remaining >= pps_size) {
                                            printf("[DEBUG] PPS %d: size=%d\n", i, pps_size);
                                            
                                            // Create Annex-B packet with PPS
                                            AVPacket *pps_packet = av_packet_alloc();
                                            if (pps_packet) {
                                                uint8_t *annex_b = av_malloc(pps_size + 4 + AV_INPUT_BUFFER_PADDING_SIZE);
                                                if (annex_b) {
                                                    annex_b[0] = 0x00;
                                                    annex_b[1] = 0x00;
                                                    annex_b[2] = 0x00;
                                                    annex_b[3] = 0x01;
                                                    memcpy(annex_b + 4, p, pps_size);
                                                    memset(annex_b + 4 + pps_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                                                    
                                                    av_packet_from_data(pps_packet, annex_b, pps_size + 4);
                                                    
                                                    ret = avcodec_send_packet(video->codec_ctx, pps_packet);
                                                    printf("[INFO] Sent PPS to decoder: %s\n", 
                                                           (ret == 0) ? "accepted" : av_err2str(ret));
                                                    
                                                    av_packet_free(&pps_packet);
                                                } else {
                                                    av_packet_free(&pps_packet);
                                                }
                                            }
                                            
                                            p += pps_size;
                                            remaining -= pps_size;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    
                    av_packet_free(&out);
                }
                
                // Clean up - the packet now owns the data
                av_packet_unref(init_pkt);
            }
            av_packet_free(&init_pkt);
        }
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
    
    // Initialize frame buffer for smooth playback
    pthread_mutex_init(&video->buffer_mutex, NULL);
    video->buffer_write_index = 0;
    video->buffer_read_index = 0;
    video->buffered_frame_count = 0;
    
    for (int i = 0; i < MAX_BUFFERED_FRAMES; i++) {
        video->frame_buffer[i] = av_frame_alloc();
        if (!video->frame_buffer[i]) {
            fprintf(stderr, "Failed to allocate buffer frame %d\n", i);
            video_cleanup(video);
            return -1;
        }
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
    /*
     * This function implements a robust H.264 decoding approach optimized for hardware decoders:
     * 1. BSF chain: h264_mp4toannexb -> h264_metadata(aud=insert)
     *    - h264_mp4toannexb converts MP4 to Annex-B and extracts SPS/PPS
     *    - h264_metadata adds AUD (Access Unit Delimiters) for frame boundaries
     * 2. FFmpeg H.264 parser ensures complete access units (AUs)
     * 3. IDR gating: Drop everything until first IDR, then send all packets
     * 4. Header prefixing: Ensure SPS/PPS are sent with first IDR
     * 5. Error handling: INVALIDDATA errors are expected and ignored
     * 6. EOS handling: Only flush at true end of stream
     */
    static int decode_call_count = 0;
    static int packet_count = 0;
    static bool first_idr_found = false;  // Track if we've found the first IDR
    static AVPacket *stored_sps_pps = NULL;  // Store SPS/PPS for prefixing
    decode_call_count++;
    
    if (decode_call_count <= 5) {
        printf("video_decode_frame() call #%d\n", decode_call_count);
    }
    
    if (!video->initialized || video->eof_reached) {
        return -1;
    }

    // First try to receive a frame from what we've already sent
    int receive_result = avcodec_receive_frame(video->codec_ctx, video->frame);
    if (receive_result == 0) {
        // Already had a frame ready
        if (decode_call_count <= 10) {
            printf("[SUCCESS] Got frame from decoder!\n");
        }
        return 0;
    } else if (receive_result != AVERROR(EAGAIN)) {
        // EOF or other error - not just "need more input"
        if (receive_result == AVERROR_EOF) {
            video->eof_reached = true;
        }
        return -1;
    }
    
    // We need to feed more packets to the decoder
    
    // If not using BSF chain, use simple packet reading logic
    if (!video->use_bsf_chain) {
        // Simple non-BSF loop
        for (;;) {
            // Read packet from input
            if (av_read_frame(video->format_ctx, video->packet) < 0) {
                // EOF or error - flush decoder
                video->eof_reached = true;
                avcodec_send_packet(video->codec_ctx, NULL);
                receive_result = avcodec_receive_frame(video->codec_ctx, video->frame);
                return (receive_result == 0) ? 0 : -1;
            }
            
            // Skip non-video packets
            if (video->packet->stream_index != video->video_stream_index) {
                av_packet_unref(video->packet);
                continue;
            }
            
            // Send packet to decoder
            int send_result = avcodec_send_packet(video->codec_ctx, video->packet);
            av_packet_unref(video->packet);
            
            if (send_result == 0) {
                // Packet accepted - try to get a frame
                receive_result = avcodec_receive_frame(video->codec_ctx, video->frame);
                if (receive_result == 0) {
                    // Got a frame
                    return 0;
                } else if (receive_result != AVERROR(EAGAIN)) {
                    // Error
                    return -1;
                }
                // EAGAIN - need more packets, continue loop
            }
        }
        
        // Should never reach here
        return -1;
    }
    
    // Using BSF chain for hardware decoding
    // Chain: packet -> h264_mp4toannexb -> h264_metadata -> decoder
    
    // Main packet processing loop
    for (;;) {
        // Read next packet from input
        int read_result = av_read_frame(video->format_ctx, video->packet);
        if (read_result < 0) {
            // EOF or read error - flush all stages
            video->eof_reached = true;

            // Flush BSF
            av_bsf_send_packet(video->bsf_chain.mp4toannexb_ctx, NULL);
            
            // Process any packets from BSF flush
            for (;;) {
                AVPacket *out = av_packet_alloc();
                int r2 = av_bsf_receive_packet(video->bsf_chain.mp4toannexb_ctx, out);
                if (r2 == AVERROR(EAGAIN) || r2 == AVERROR_EOF) {
                    av_packet_free(&out);
                    break;
                }
                if (r2 < 0) { 
                    av_packet_free(&out);
                    break;
                }
                int process_result = ingest_metadata_packet(video, out, &stored_sps_pps,
                                                            &first_idr_found, decode_call_count,
                                                            &packet_count);
                av_packet_free(&out);

                if (process_result < 0) {
                    if (decode_call_count <= 5) {
                        printf("[ERROR] Failed to process BSF flush packet: %s\n", av_err2str(process_result));
                    }
                    return -1;
                }

                if (process_result > 0) {
                    return 0;
                }
            }

            int flush_result = flush_pending_access_unit(video, &stored_sps_pps,
                                                         &first_idr_found, decode_call_count,
                                                         &packet_count);
            if (flush_result > 0) {
                return 0;
            } else if (flush_result < 0) {
                return -1;
            }

            if (stored_sps_pps) {
                av_packet_free(&stored_sps_pps);
            }

            // Only at EOS: Final decoder flush
            printf("[INFO] End of stream reached - sending NULL packet to flush decoder\n");
            int decoder_flush_result = avcodec_send_packet(video->codec_ctx, NULL);
            if (decoder_flush_result < 0) {
                printf("[ERROR] Failed to send flush packet to decoder: %s\n", av_err2str(decoder_flush_result));
                return -1;
            }
            
            // Drain all remaining frames
            receive_result = avcodec_receive_frame(video->codec_ctx, video->frame);
            if (receive_result == 0) {
                printf("[INFO] Got frame from decoder flush\n");
                return 0; // Got a frame from flush
            } else {
                printf("[INFO] No more frames in decoder after flush\n");
                return -1;
            }
        }
        
        // Skip non-video packets
        if (video->packet->stream_index != video->video_stream_index) {
            av_packet_unref(video->packet);
            continue;
        }
        
        // Debug input packet
        if (packet_count < 5) {
            printf("[DEBUG] Input packet #%d: size=%d, pts=%ld, flags=0x%x (keyframe=%d)\n",
                   packet_count, video->packet->size, video->packet->pts, 
                   video->packet->flags, (video->packet->flags & AV_PKT_FLAG_KEY) ? 1 : 0);
        }
        
        // Process through h264_mp4toannexb BSF
        int bsf_send_result = av_bsf_send_packet(video->bsf_chain.mp4toannexb_ctx, video->packet);
        av_packet_unref(video->packet); // Done with input packet
        
        if (bsf_send_result < 0) {
            if (decode_call_count <= 5) {
                printf("[ERROR] Failed to send packet to mp4toannexb BSF: %s\n", av_err2str(bsf_send_result));
            }
            continue;
        }
            
        // Process mp4toannexb output
        for (;;) {
            AVPacket *annexb_pkt = av_packet_alloc();
            if (!annexb_pkt) {
                printf("[ERROR] Failed to allocate packet\n");
                break;
            }
            
            int r2 = av_bsf_receive_packet(video->bsf_chain.mp4toannexb_ctx, annexb_pkt);
            if (r2 == AVERROR(EAGAIN)) {
                av_packet_free(&annexb_pkt);
                break;
            }
            if (r2 == AVERROR_EOF || r2 < 0) {
                av_packet_free(&annexb_pkt);
                break;
            }
            
            // Send through h264_metadata BSF for AUD insertion
            int metadata_send = av_bsf_send_packet(video->bsf_chain.metadata_ctx, annexb_pkt);
            av_packet_free(&annexb_pkt);
            
            if (metadata_send < 0) {
                if (decode_call_count <= 5) {
                    printf("[ERROR] Failed to send packet to h264_metadata BSF: %s\n", av_err2str(metadata_send));
                }
                continue;
            }
            
            // Process h264_metadata output
            for (;;) {
                AVPacket *out = av_packet_alloc();
                if (!out) {
                    printf("[ERROR] Failed to allocate output packet\n");
                    break;
                }

                int r3 = av_bsf_receive_packet(video->bsf_chain.metadata_ctx, out);
                if (r3 == AVERROR(EAGAIN)) {
                    av_packet_free(&out);
                    break;
                }
                if (r3 == AVERROR_EOF || r3 < 0) {
                    av_packet_free(&out);
                    break;
                }
                
                // Use the H.264 parser to ensure complete access units
                if (video->use_parser) {
                    uint8_t *parser_data = NULL;
                    int parser_size = 0;
                    av_parser_parse2(
                        video->parser,
                        video->codec_ctx,
                        &parser_data, &parser_size,
                        out->data, out->size,
                        out->pts, out->dts,
                        0);
                        
                    if (parser_size > 0) {
                        // Parser produced a complete access unit
                        printf("[DEBUG] Parser produced complete AU of size %d bytes\n", parser_size);
                        
                        // Replace packet data with parser output
                        AVPacket *parsed_packet = av_packet_alloc();
                        if (parsed_packet) {
                            if (av_packet_copy_props(parsed_packet, out) >= 0) {
                                uint8_t *new_data = av_mallocz(parser_size + AV_INPUT_BUFFER_PADDING_SIZE);
                                if (new_data) {
                                    memcpy(new_data, parser_data, parser_size);
                                    av_packet_from_data(parsed_packet, new_data, parser_size);
                                    
                                    // Process the parsed packet
                                    int process_result = ingest_metadata_packet(video, parsed_packet, &stored_sps_pps,
                                                                   &first_idr_found, decode_call_count,
                                                                   &packet_count);
                                    
                                    av_packet_free(&parsed_packet);
                                    av_packet_free(&out);
                                    
                                    if (process_result != 0) {
                                        return process_result;
                                    }
                                    continue; // Continue to next packet
                                }
                            }
                            av_packet_free(&parsed_packet);
                        }
                    }
                }
                
                // If not using parser or parser didn't produce output, process the original packet
                int process_result = ingest_metadata_packet(video, out, &stored_sps_pps,
                                                            &first_idr_found, decode_call_count,
                                                            &packet_count);
                av_packet_free(&out);

                if (process_result < 0) {
                    if (decode_call_count <= 5) {
                        printf("[ERROR] Failed to process BSF output: %s\n", av_err2str(process_result));
                    }
                    return -1;
                }

                if (process_result > 0) {
                    return 0;
                }
            }
        }
    }

    // Should never reach here
    return -1;
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
    
    // Reset IDR gate flag when seeking - we need to wait for a new IDR
    // after a seek operation before we can start decoding properly
    video->first_idr_seen = false;
    video->first_au_reordered = false;
    reset_current_access_unit(video);

    printf("Video playback restarted - will wait for next IDR frame\n");
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
    // Initialize to NULL/0 in case of failure
    if (y) *y = NULL;
    if (u) *u = NULL;
    if (v) *v = NULL;
    if (y_stride) *y_stride = 0;
    if (u_stride) *u_stride = 0;
    if (v_stride) *v_stride = 0;
    
    if (!video || !video->frame) {
        return;
    }
    
    // Check if this is a hardware frame that needs special handling
    if (video->use_hardware_decode) {
        // Get hardware frame format
        enum AVPixelFormat hw_format = video->frame->format;
        
        // Debug log first hardware frame
        static bool hw_format_logged = false;
        if (!hw_format_logged) {
            printf("[INFO] Hardware frame format: %s (%d)\n", 
                   av_get_pix_fmt_name(hw_format), hw_format);
            hw_format_logged = true;
        }
        
        // Handle DRM_PRIME format specially
        if (hw_format == AV_PIX_FMT_DRM_PRIME) {
            // For DRM_PRIME, we need to pass special pointers to the GL layer
            // Our GL renderer can handle this directly
            
            // Set the pointers to special values that gl_render_frame will recognize
            if (y) *y = (uint8_t*)1; // Special marker for DRM_PRIME
            if (u) *u = NULL;
            if (v) *v = NULL;
            
            // Set stride information from the frame
            if (y_stride) *y_stride = video->frame->linesize[0];
            if (u_stride) *u_stride = video->frame->linesize[1];
            if (v_stride) *v_stride = video->frame->linesize[2];
            
            // Set special DRM_PRIME marker
            return;
        }
        // NV12 format (Y plane + interleaved UV plane)
        else if (hw_format == AV_PIX_FMT_NV12) {
            // NV12 has Y in plane 0 and interleaved UV in plane 1
            if (y) *y = video->frame->data[0];
            if (u) *u = video->frame->data[1]; // UV interleaved
            if (v) *v = NULL; // No separate V plane
            
            // Set stride information
            if (y_stride) *y_stride = video->frame->linesize[0];
            if (u_stride) *u_stride = video->frame->linesize[1];
            if (v_stride) *v_stride = 0;
            
            return;
        }
    }
    
    // Standard YUV420P format or fallback
    if (y) *y = video->frame->data[0];
    if (u) *u = video->frame->data[1];
    if (v) *v = video->frame->data[2];
    
    // Set stride information
    if (y_stride) *y_stride = video->frame->linesize[0];
    if (u_stride) *u_stride = video->frame->linesize[1];
    if (v_stride) *v_stride = video->frame->linesize[2];
    
    // Debug logging for first frame
    static bool format_logged = false;
    if (!format_logged) {
        printf("[INFO] Frame format: %s (%d)\n", 
               av_get_pix_fmt_name(video->frame->format), video->frame->format);
        format_logged = true;
    }
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
    if (!video) return;
    
    // Print cleanup debug info
    printf("[CLEANUP] Starting video context cleanup\n");
    
    // Clean up frame buffer
    for (int i = 0; i < MAX_BUFFERED_FRAMES; i++) {
        if (video->frame_buffer[i]) {
            av_frame_free(&video->frame_buffer[i]);
            video->frame_buffer[i] = NULL;
        }
    }
    pthread_mutex_destroy(&video->buffer_mutex);
    
    // Clean up BSF chain if used
    if (video->use_bsf_chain) {
        printf("[CLEANUP] Freeing BSF chain resources\n");
        free_bsf_chain(&video->bsf_chain);
    }
    
    // Free the H.264 parser if used
    if (video->parser) {
        printf("[CLEANUP] Freeing H.264 parser\n");
        av_parser_close(video->parser);
        video->parser = NULL;
    }
    
    if (video->frame) {
        printf("[CLEANUP] Freeing video frame\n");
        av_frame_free(&video->frame);
        video->frame = NULL;
    }
    
    if (video->packet) {
        printf("[CLEANUP] Freeing video packet\n");
        av_packet_free(&video->packet);
        video->packet = NULL;
    }
    
    if (video->codec_ctx) {
        printf("[CLEANUP] Freeing codec context\n");
        avcodec_free_context(&video->codec_ctx);
    }
    
    if (video->format_ctx) {
        printf("[CLEANUP] Closing input format context\n");
        avformat_close_input(&video->format_ctx);
    }

    if (video->au_buffer) {
        av_free(video->au_buffer);
        video->au_buffer = NULL;
        video->au_buffer_capacity = 0;
        video->au_buffer_size = 0;
    }
    
    printf("[CLEANUP] Video context cleanup complete\n");
    memset(video, 0, sizeof(*video));
}