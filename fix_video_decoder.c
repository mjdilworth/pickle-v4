// Fix for the video_decoder.c to wait for IDR frames
// This is a modified version of the current hardware decoding start section

        // For hardware decoding, ensure we start from a real IDR frame, not just any MP4 keyframe
        if (video->use_hardware_decode && !video->keyframe_seen) {
            // Check if this is marked as a keyframe (necessary but not sufficient)
            bool is_keyframe = (video->packet->flags & AV_PKT_FLAG_KEY) ? true : false;
            
            // If not even marked as a keyframe, skip immediately
            if (!is_keyframe) {
                printf("Skipping non-keyframe packet while waiting for first IDR\n");
                av_packet_unref(video->packet);
                continue;
            }
            
            // Even if it's marked as a keyframe, check that it actually contains an IDR NAL (type 5/0x65)
            bool found_idr = false;
            
            // Scan packet for IDR NAL units (IDR=5/0x65)
            for (int i = 0; i < video->packet->size - 5; i++) {
                // Look for start codes
                if ((video->packet->data[i] == 0x00 && 
                     video->packet->data[i+1] == 0x00 && 
                     video->packet->data[i+2] == 0x00 && 
                     video->packet->data[i+3] == 0x01) ||
                    (video->packet->data[i] == 0x00 && 
                     video->packet->data[i+1] == 0x00 && 
                     video->packet->data[i+2] == 0x01)) {
                    
                    int start_code_len = (video->packet->data[i+2] == 0x01) ? 3 : 4;
                    int nal_start = i + start_code_len;
                    
                    if (nal_start < video->packet->size) {
                        uint8_t nal_type = video->packet->data[nal_start] & 0x1F;
                        if (nal_type == 5) { // 5 = IDR slice (0x65)
                            found_idr = true;
                            break;
                        }
                    }
                    
                    // Skip to next potential start code
                    i += start_code_len;
                }
            }
            
            // If no IDR NAL found, skip this packet even if it's marked as a keyframe
            if (!found_idr) {
                printf("Skipping keyframe packet without IDR NAL units, waiting for real IDR\n");
                av_packet_unref(video->packet);
                continue;
            } else {
                printf("Found first real IDR frame (NAL type 5/0x65), beginning hardware decoding\n");
                video->keyframe_seen = true;
            }
        }
