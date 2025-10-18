#include "v4l2_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/opt.h>

// Initialize BSF chain for H.264 optimization
int init_h264_bsf_chain(bsf_chain_t *chain, AVCodecParameters *codecpar)
{
    int ret;
    memset(chain, 0, sizeof(*chain));

    // Setup only h264_mp4toannexb BSF - converts to Annex-B and prepends SPS/PPS
    const AVBitStreamFilter *mp4toannexb = av_bsf_get_by_name("h264_mp4toannexb");
    if (!mp4toannexb) {
        fprintf(stderr, "h264_mp4toannexb bitstream filter not found\n");
        return -1;
    }

    ret = av_bsf_alloc(mp4toannexb, &chain->mp4toannexb_ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to allocate h264_mp4toannexb context\n");
        return ret;
    }

    // Set input parameters directly from codec
    ret = avcodec_parameters_copy(chain->mp4toannexb_ctx->par_in, codecpar);
    if (ret < 0) {
        fprintf(stderr, "Failed to copy codec parameters to mp4toannexb filter\n");
        av_bsf_free(&chain->mp4toannexb_ctx);
        return ret;
    }

    ret = av_bsf_init(chain->mp4toannexb_ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to initialize mp4toannexb filter\n");
        av_bsf_free(&chain->mp4toannexb_ctx);
        return ret;
    }

    // Second: h264_metadata with aud=insert - adds Access Unit Delimiters for frame boundaries
    const AVBitStreamFilter *h264_metadata = av_bsf_get_by_name("h264_metadata");
    if (!h264_metadata) {
        fprintf(stderr, "h264_metadata bitstream filter not found\n");
        av_bsf_free(&chain->mp4toannexb_ctx);
        return -1;
    }

    ret = av_bsf_alloc(h264_metadata, &chain->metadata_ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to allocate h264_metadata context\n");
        av_bsf_free(&chain->mp4toannexb_ctx);
        return ret;
    }

    // Copy parameters from mp4toannexb output to metadata input
    ret = avcodec_parameters_copy(chain->metadata_ctx->par_in, chain->mp4toannexb_ctx->par_out);
    if (ret < 0) {
        fprintf(stderr, "Failed to copy parameters between BSFs\n");
        av_bsf_free(&chain->mp4toannexb_ctx);
        av_bsf_free(&chain->metadata_ctx);
        return ret;
    }

    // Set aud=insert option for h264_metadata
    ret = av_opt_set(chain->metadata_ctx->priv_data, "aud", "insert", 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to set aud=insert option for h264_metadata\n");
        av_bsf_free(&chain->mp4toannexb_ctx);
        av_bsf_free(&chain->metadata_ctx);
        return ret;
    }

    ret = av_bsf_init(chain->metadata_ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to initialize h264_metadata filter\n");
        av_bsf_free(&chain->mp4toannexb_ctx);
        av_bsf_free(&chain->metadata_ctx);
        return ret;
    }

    printf("[INFO] Initialized H.264 BSF chain: h264_mp4toannexb -> h264_metadata(aud=insert)\n");
    chain->ready = true;
    return 0;
}

// Process a packet through the complete BSF chain
// This handles the full pipeline: h264_mp4toannexb -> h264_metadata
int filter_packet_through_bsf(bsf_chain_t *chain, AVPacket *in_pkt, AVPacket *out_pkt)
{
    int ret;
    AVPacket *temp_pkt = av_packet_alloc();
    if (!temp_pkt) {
        return AVERROR(ENOMEM);
    }

    // Skip BSF processing if chain not initialized
    if (!chain || !chain->ready) {
        // Just clone the input packet to output
        av_packet_ref(out_pkt, in_pkt);
        av_packet_free(&temp_pkt);
        return 0;
    }

    // First BSF: h264_mp4toannexb
    ret = av_bsf_send_packet(chain->mp4toannexb_ctx, in_pkt);
    if (ret < 0) {
        fprintf(stderr, "Error sending packet to mp4toannexb filter: %s\n", av_err2str(ret));
        av_packet_free(&temp_pkt);
        return ret;
    }

    ret = av_bsf_receive_packet(chain->mp4toannexb_ctx, temp_pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        fprintf(stderr, "Error receiving packet from mp4toannexb filter: %s\n", av_err2str(ret));
        av_packet_free(&temp_pkt);
        return ret;
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        // No output packet available yet, or end of stream
        av_packet_free(&temp_pkt);
        return ret;
    }

    // Second BSF: h264_metadata
    ret = av_bsf_send_packet(chain->metadata_ctx, temp_pkt);
    av_packet_unref(temp_pkt); // We don't need temp_pkt's data anymore
    
    if (ret < 0) {
        fprintf(stderr, "Error sending packet to metadata filter: %s\n", av_err2str(ret));
        av_packet_free(&temp_pkt);
        return ret;
    }

    ret = av_bsf_receive_packet(chain->metadata_ctx, out_pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        fprintf(stderr, "Error receiving packet from metadata filter: %s\n", av_err2str(ret));
    }

    av_packet_free(&temp_pkt);
    return ret;
}

// Flush any remaining packets from the BSF chain
// Free resources used by the BSF chain
void free_bsf_chain(bsf_chain_t *chain)
{
    if (!chain) return;
    
    if (chain->metadata_ctx) {
        av_bsf_free(&chain->metadata_ctx);
    }
    if (chain->mp4toannexb_ctx) {
        av_bsf_free(&chain->mp4toannexb_ctx);
    }

    chain->ready = false;
}

// Check if NAL unit is an IDR frame (type 5)
bool is_h264_idr_nal(const uint8_t *data, size_t size)
{
    // Need at least 5 bytes (4 for start code, 1 for NAL header)
    if (!data || size < 5) return false;
    
    // Check for Annex-B start code
    if (data[0] == 0x00 && data[1] == 0x00 && 
        ((data[2] == 0x00 && data[3] == 0x01) || (data[2] == 0x01))) {
        
        // Determine start code size (3 or 4 bytes)
        int start_code_size = (data[2] == 0x01) ? 3 : 4;
        
        // Get NAL type from header byte (bits 0-4)
        uint8_t nal_type = data[start_code_size] & 0x1F;
        
        // NAL type 5 = IDR slice
        return (nal_type == 5);
    }
    
    return false;
}

// Check if NAL unit is an Access Unit Delimiter (type 9)
bool is_h264_aud_nal(const uint8_t *data, size_t size)
{
    // Need at least 5 bytes (4 for start code, 1 for NAL header)
    if (!data || size < 5) return false;
    
    // Check for Annex-B start code
    if (data[0] == 0x00 && data[1] == 0x00 && 
        ((data[2] == 0x00 && data[3] == 0x01) || (data[2] == 0x01))) {
        
        // Determine start code size (3 or 4 bytes)
        int start_code_size = (data[2] == 0x01) ? 3 : 4;
        
        // Get NAL type from header byte (bits 0-4)
        uint8_t nal_type = data[start_code_size] & 0x1F;
        
        // NAL type 9 = Access Unit Delimiter
        return (nal_type == 9);
    }
    
    return false;
}

// Check if an access unit (packet) contains at least one IDR frame
// Returns true if the packet contains at least one IDR NAL unit (NAL type 5)
bool au_has_idr(const AVPacket *packet)
{
    if (!packet || !packet->data || packet->size < 5)
        return false;
    
    // Scan the entire packet for Annex-B NAL units
    const uint8_t *data = packet->data;
    size_t remaining = packet->size;
    size_t pos = 0;
    
    // Look for start codes (0x000001 or 0x00000001)
    while (pos + 5 <= remaining) {
        // Find start code
        if ((pos + 4 <= remaining && 
             data[pos] == 0 && data[pos+1] == 0 && 
             data[pos+2] == 0 && data[pos+3] == 1) ||
            (pos + 3 <= remaining && 
             data[pos] == 0 && data[pos+1] == 0 && 
             data[pos+2] == 1)) {
            
            // Determine start code length (3 or 4 bytes)
            int start_code_len = (data[pos+2] == 1) ? 3 : 4;
            
            // Get NAL header position
            size_t nal_pos = pos + start_code_len;
            
            if (nal_pos < remaining) {
                // Extract NAL type from first byte (bits 0-4)
                uint8_t nal_type = data[nal_pos] & 0x1F;
                
                // If this is an IDR (NAL type 5), we found one
                if (nal_type == 5) {
                    return true;
                }
            }
            
            // Skip past this start code and NAL header to look for more NALs
            pos += start_code_len + 1;
        } else {
            // No start code here, move forward
            pos++;
        }
    }
    
    // No IDR NALs found in this packet
    return false;
}

// Structure to represent a NAL unit
typedef struct {
    uint8_t type;      // NAL unit type (bits 0-4)
    size_t offset;     // Offset in the original buffer
    size_t size;       // Size including start code
    int start_code_len;// Length of start code (3 or 4)
} nal_unit_t;

// NOTE: find_nal_units function was removed because:
// 1. It was unused in the current implementation
// 2. Its functionality is provided by the BSF chain
// 3. We're now letting libavcodec handle NAL parsing and processing

// Reorder NAL units in a packet to ensure decoder-friendly order
// Standard canonical order: AUD → SPS → PPS → [SEI] → IDR/Slice
/* 
 * NOTE: This function is no longer used.
 * We trust the BSF chain (h264_mp4toannexb → filter_units → h264_metadata) to produce
 * properly formatted output without additional reordering.
 */

/* 
 * DISABLED: reorder_h264_nal_units function
 * 
 * This function is intentionally disabled as it modifies packet data after BSF chain.
 * Instead, we use the filter_units BSF to remove SEI NALs and rely on the BSF chain 
 * (h264_mp4toannexb → filter_units → h264_metadata) to properly format the H.264
 * bitstream for hardware decoders.
 * 
 * Lifecycle of packets:
 * 1. Demuxer provides one AU per AVPacket
 * 2. BSF chain processes and optimizes the bitstream
 * 3. Output packet goes DIRECTLY to decoder WITHOUT any further modification
 * 
 * This ensures we don't corrupt the carefully processed bitstream or create
 * invalid packet references.
 */
