# 2-Stage BSF Chain Implementation with AUD Insertion

## Overview
Implemented a 2-stage Bitstream Filter (BSF) chain for improved Raspberry Pi hardware decode compatibility:
- **Stage 1**: `h264_mp4toannexb` - Converts avcC format to Annex-B
- **Stage 2**: `h264_metadata` with `aud=insert` - Inserts AUD (type 9) NAL before each AU

## Changes Made

### 1. Header File Updates (`video_decoder.h`)
```c
// Changed from single BSF context to 2-stage chain:
AVBSFContext *bsf_annexb_ctx;    // Stage 1: h264_mp4toannexb (avcC to Annex-B conversion)
AVBSFContext *bsf_aud_ctx;       // Stage 2: h264_metadata (AUD insertion)
```

### 2. BSF Chain Initialization (`video_decoder.c`)

#### Stage 1: h264_mp4toannexb
- Allocates and initializes filter for avcC → Annex-B conversion
- Handles initial codec parameter setup

#### Stage 2: h264_metadata with AUD
- Allocated after Stage 1
- Takes output of Stage 1 as input (`par_in = bsf_annexb_ctx->par_out`)
- Configured with `aud=insert` option to insert AUD NALs before each Access Unit

### 3. Packet Processing in Decode Loop

**2-Stage Filtering Pipeline:**
```c
// Stage 1: Send to h264_mp4toannexb
av_bsf_send_packet(video->bsf_annexb_ctx, video->packet)
av_bsf_receive_packet(video->bsf_annexb_ctx, &bsf_stage1_output)

// Stage 2: Feed Stage 1 output to h264_metadata
av_bsf_send_packet(video->bsf_aud_ctx, &bsf_stage1_output)
av_bsf_receive_packet(video->bsf_aud_ctx, &bsf_stage2_output)

// Direct passthrough to decoder (no manual NAL manipulation)
avcodec_send_packet(video->codec_ctx, pkt_to_decode)
```

**Key Features:**
- Both packets are sent as-is after BSF processing (no re-ordering, no manual SPS/PPS "fixing")
- EAGAIN from BSF is not treated as failure - just continue reading more packets
- Proper cleanup of both stage outputs after decoding

### 4. Improved Warmup Handling

**Initial Packet Limit (First GOP):**
```c
static int max_packets = 48;  // Deep warmup for first GOP
```

**Adaptive Fallback Logic:**
```c
// Much more lenient thresholds for Pi hardware
consecutive_fails < 5  // Increased from 3
max_packets < 64      // Higher ceiling
max_packets += 4      // Faster increase per attempt
```

**Why:** Some Pi kernels don't emit capture format (no frames) until they've seen multiple AUs after the first IDR/SPS/PPS. The deeper warmup ensures we feed enough data before giving up.

### 5. EAGAIN Handling Improvements

- **Before:** Treated EAGAIN as potential failure after few attempts
- **After:** EAGAIN just means BSF or decoder needs more packets - continue queueing
- Result: More resilient first frame decode on Pi hardware

### 6. Cleanup Updates

Both BSF contexts properly freed in fallback path:
```c
if (video->bsf_annexb_ctx) {
    av_bsf_free(&video->bsf_annexb_ctx);
    video->bsf_annexb_ctx = NULL;
}
if (video->bsf_aud_ctx) {
    av_bsf_free(&video->bsf_aud_ctx);
    video->bsf_aud_ctx = NULL;
}
```

## Benefits for Raspberry Pi

1. **Annex-B Format**: V4L2 M2M decoders on Pi consistently expect Annex-B with start codes
2. **AUD Markers**: Type 9 NAL units before each AU help some Pi kernels detect frame boundaries
3. **Deep Warmup**: Many Pi kernels need multiple AUs before they report frame capture capability
4. **Clean Passthrough**: No manual NAL surgery - let FFmpeg's BSF handle the complexity

## Testing

Verified with:
```bash
./pickle --timing --hw-debug test_video.mp4
```

**Results:**
- ✅ 2-stage BSF chain successfully initialized
- ✅ Annex-B format confirmed after Stage 1
- ✅ AUD NAL insertion applied by Stage 2
- ✅ First frame decoded successfully with 2 packets (efficient warmup)
- ✅ YUV data properly available for GPU rendering

## Diagnostic Output

The implementation provides clear visibility:
```
[INFO]  Initializing 2-stage BSF chain for V4L2 M2M:
[INFO]    Stage 1: h264_mp4toannexb (avcC → Annex-B)
[INFO]    Stage 2: h264_metadata with aud=insert (AUD insertion)
[INFO]  ✓ h264_mp4toannexb initialized
[INFO]  ✓ h264_metadata with aud=insert initialized
[INFO]  ✓ 2-stage BSF chain ready (Annex-B + AUD insertion)

[BSF Output] Final packet format (Annex-B + AUD): 00 00 00 01...
✓ Correct Annex-B format detected (Stage 1)
✓ AUD NAL detected (Stage 2)
```

## Compatibility

- ✅ Maintains backward compatibility with software fallback
- ✅ Only active when V4L2 M2M hardware decoder is selected
- ✅ No changes to software decode path
