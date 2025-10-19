# AUD Insertion Investigation

## Issue
The h264_metadata BSF with `aud=insert` option is not producing AUD (0x09) NALs at the start of packets.

## Hypotheses to Test

### 1. Option Name or Value Might Be Different
- Possible: `aud` vs `aud_insert` vs `insert_aud`
- Possible: value should be `1`, `true`, or something else

### 2. h264_metadata Might Not Work At Packet Level
- Some BSFs operate on frames, not individual packets
- AUD might be inserted frame-by-frame, not per-packet
- May only work in specific FFmpeg contexts

### 3. RPi V4L2 M2M May Not Need Explicit AUD
- Hardware may handle AU boundaries natively
- Decoder might work fine without AUD NALs
- AUD might be optional for V4L2 M2M

### 4. BSF Chaining Order Matters
- First h264_mp4toannexb (converts format)
- Then h264_metadata (adds metadata)
- May need different order or configuration

### 5. Extradata Might Be Preventing AUD Insertion
- avcC→Annex-B extradata with SPS/PPS
- h264_metadata might treat packets differently when extradata is present

## Suggested Next Steps

1. **Try Different Option Names:**
   ```c
   av_opt_set(bsf_aud_ctx, "aud", "insert", 0);
   av_opt_set(bsf_aud_ctx, "insert", "aud", 0);  
   av_opt_set_int(bsf_aud_ctx, "aud", 1, 0);
   ```

2. **Check FFmpeg BSF Documentation:**
   ```bash
   ffmpeg -bsfs | grep -A5 h264_metadata
   ffprobe -bsfs
   ```

3. **Test Without mp4toannexb Stage:**
   - Run only h264_metadata on avcC input
   - See if AUD insertion works without double-filtering

4. **Manual AUD Insertion:**
   - If BSF approach fails, manually prepend AUD NAL
   - 0x00 0x00 0x00 0x01 0x09 (4-byte start code + AUD type)

5. **Verify RPi Doesn't Need AUD:**
   - Current setup already decoding frames successfully
   - Test if explicit AUD improves performance or stability
   - May be unnecessary for V4L2 M2M

## Performance Impact
- Current system working without AUD (first frame in 2-3 packets)
- AUD insertion might be optimization, not requirement
- Focus on validation and testing rather than forcing AUD

