#!/usr/bin/env bash
set -euo pipefail

CONF="/home/dilly/pickle.conf"
VIDEO_DIR="/home/dilly/Video"
PICKLE="/home/dilly/pickle"
DEBUG=0

[[ "${1:-}" == "--debug" ]] && DEBUG=1

if [[ ! -f "$CONF" ]]; then
    echo "Config file not found: $CONF"
    exit 1
fi

# Print config in debug mode
if [[ $DEBUG -eq 1 ]]; then
    echo "=== Contents of $CONF ==="
    cat "$CONF"
    echo "=========================="
    echo
fi

# Build playlist array
ARGS=()

# Parse key=value pairs, looking for video*=yes
while IFS='=' read -r key value; do
    # Strip whitespace
    key=$(echo "$key" | xargs)
    value=$(echo "$value" | xargs)

    # Check if key starts with "video" and value is "yes"
    if [[ "$key" =~ ^video.*$ ]] && [[ "$value" == "yes" ]]; then
        video_file="$VIDEO_DIR/${key}.mp4"
        ARGS+=("$video_file")
    fi
done < "$CONF"

if (( ${#ARGS[@]} == 0 )); then
    echo "ERROR: No videos enabled in pickle.conf (no video*=yes entries found)"
    exit 1
fi

# Build final command
CMD=( "$PICKLE" -l )

# Add --hw flag if 2 or more videos are enabled
if (( ${#ARGS[@]} >= 2 )); then
    CMD+=( "--hw" )
fi

CMD+=( "${ARGS[@]}" )

if [[ $DEBUG -eq 1 ]]; then
    echo "DEBUG MODE: Pickle would start with:"
    printf '  %q' "${CMD[@]}"
    echo
    exit 0
fi

# Ensure all files exist before running
for f in "${ARGS[@]}"; do
    if [[ ! -f "$f" ]]; then
        echo "ERROR: Missing video file: $f"
        exit 1
    fi
done

echo "Starting Pickle with:"
printf '  %q' "${CMD[@]}"
echo
exec "${CMD[@]}"
```

**Debug output will now show:**
```
=== Contents of /home/dilly/pickle.conf ===
video1=yes
video2=no
video3=yes
==========================

DEBUG MODE: Pickle would start with:
  /home/dilly/pickle -l /home/dilly/Video/video1.mp4 /home/dilly/Video/video3.mp4