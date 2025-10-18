#ifndef V4L2_UTILS_H
#define V4L2_UTILS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>

// BSF (Bitstream Filter) chain for optimizing V4L2 hardware decoding
typedef struct {
    AVBSFContext *mp4toannexb_ctx;        // Convert MP4 style to Annex-B
    AVBSFContext *metadata_ctx;           // h264_metadata filter (aud=insert)
    bool ready;                           // Indicates if BSF chain is initialized
} bsf_chain_t;

// Initialize BSF chain for H.264 conversion and optimization
// This sets up a bitstream filter to ensure V4L2 compatibility:
// h264_mp4toannexb - Convert MP4/MOV format to raw Annex-B and prepends SPS/PPS to keyframes
// Returns: 0 on success, negative on error
int init_h264_bsf_chain(bsf_chain_t *chain, AVCodecParameters *codecpar);

// Filter a packet through the BSF chain
// Maintains Access Unit (AU) boundaries - one input AU will produce one output AU
// The resulting packet will be properly formatted for V4L2 M2M hardware decoding
// Returns: 1 if a packet is available, 0 if no packet yet, negative on error
// Note: Multiple calls may be needed per input packet as BSFs can buffer/delay output
// Important: We preserve MP4 AU boundaries and never split/merge packets after BSF chain
// Stateful decoder support: Gates only until first IDR, then feeds ALL subsequent frames
int filter_packet_through_bsf(bsf_chain_t *chain, AVPacket *in_pkt, AVPacket *out_pkt);

// Free resources used by the BSF chain
void free_bsf_chain(bsf_chain_t *chain);

// NAL type checking utilities
bool is_h264_idr_nal(const uint8_t *data, size_t size);
bool is_h264_aud_nal(const uint8_t *data, size_t size);

// Check if an access unit (packet) contains at least one IDR frame
// Returns: true if the packet contains at least one IDR NAL unit
bool au_has_idr(const AVPacket *packet);

// NAL unit types for H.264
#define NAL_TYPE_SLICE    1   // Coded slice
#define NAL_TYPE_DPA      2   // Coded slice data partition A
#define NAL_TYPE_DPB      3   // Coded slice data partition B
#define NAL_TYPE_DPC      4   // Coded slice data partition C
#define NAL_TYPE_IDR      5   // IDR slice
#define NAL_TYPE_SEI      6   // Supplemental enhancement information
#define NAL_TYPE_SPS      7   // Sequence parameter set
#define NAL_TYPE_PPS      8   // Picture parameter set
#define NAL_TYPE_AUD      9   // Access unit delimiter
#define NAL_TYPE_END_SEQ  10  // End of sequence
#define NAL_TYPE_END_STR  11  // End of stream
#define NAL_TYPE_FILLER   12  // Filler data

// NOTE: This function is no longer used - we trust the BSF chain output
// Reorder NAL units in a packet to ensure canonical, decoder-friendly order
// Standard order: AUD → SPS → PPS → [SEI] → IDR/Slice
// Returns: 0 on success, negative on error
// int reorder_h264_nal_units(AVPacket *packet);

#endif // V4L2_UTILS_H