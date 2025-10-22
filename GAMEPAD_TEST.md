# Gamepad Testing Guide

## Current Status

The gamepad is now detected using the joystick API (`/dev/input/js0`). I've added debug output to help diagnose the X button issue.

## Testing the X Button

Run the player with debug enabled:
```bash
./pickle --debug-gamepad test_video.mp4
```

### What to Look For:

1. **Button Detection**: When you press X, you should see:
   ```
   [GAMEPAD] Button pressed: X (button 2)
   ```

2. **Corner Cycling**: Immediately after, you should see:
   ```
   Cycling corners: current=<number>
   Cycled from corner <n> to <n+1>
   ```

3. **If Nothing Happens**: 
   - The button might have a different number on your controller
   - The flag might not be set correctly
   - The corner cycling code might not be reached

## Manual Button Test

To see what button number your X button actually reports:
```bash
/tmp/test_js
```

Then press the X button (the top button, labeled X). Note the button number it reports.

## Expected Button Mapping

For 8BitDo Zero 2:
- A (right button) = 0
- B (bottom button) = 1  
- X (top button) = 2
- Y (left button) = 3
- L1 = 4
- R1 = 5
- SELECT = 6
- START = 7
- HOME = 8

If your X button reports a different number, we need to update the mapping in `input_handler.h`.
