# Input Logging Fix

## Problem
The application was constantly logging key press messages to the console, creating excessive output that cluttered the terminal.

## Root Cause
Debug printf statements were left in the input handler code for both:
1. Terminal input mode (stdin fallback)
2. Event device input mode

These printed messages for every key press including:
- Number keys (1, 2, 3, 4)
- Arrow keys (up, down, left, right)
- Toggle keys (C, B, H)
- Action keys (R, P)

## Fix Applied
Commented out non-essential printf statements in `input_handler.c`:

### Terminal Input Section (lines ~309-347)
- Removed logs for keys: 1, 2, 3, 4, C, B, H, R, P
- Kept: ESC/quit message

### Event Device Section (lines ~372-400)
- Removed logs for keys: 1, 2, 3, 4, C, arrow keys, R
- Kept: ESC/quit message

## What's Still Logged
- Initialization messages (device detection, mode selection)
- Quit requests ("Quit requested")
- Critical errors

## Testing
Run the application - it should now operate quietly:
```bash
./pickle /path/to/video.mp4
```

Key presses still work normally, just without the verbose logging.
