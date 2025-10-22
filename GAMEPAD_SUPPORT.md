# Gamepad Support - 8BitDo Zero 2

## Overview

Pickle video player now supports the 8BitDo Zero 2 gamepad via Bluetooth. The gamepad provides an ergonomic alternative to keyboard controls for adjusting keystone correction.

## Gamepad Detection

The player automatically detects connected gamepads at startup using the Linux joystick API (`/dev/input/js0`). This provides better compatibility and simpler event handling than the evdev interface.

You'll see one of these messages:

- `Found gamepad device: /dev/input/js0 (8BitDo Zero 2 gamepad)` - Gamepad detected
- `Gamepad input enabled` - Gamepad ready for use
- `No gamepad detected (keyboard/terminal input only)` - No gamepad found, keyboard still works

**Note**: Keyboard and gamepad inputs work simultaneously, so you can use both at the same time.

## 8BitDo Zero 2 Controls

### Corner Selection & Movement
- **X Button**: Cycle through corners (1→2→3→4→1)
- **D-pad**: Move selected corner (precise digital control)
- **Left Analog Stick**: Move selected corner (smooth analog control with 25% deadzone)

### Keystone Adjustments
- **L1 (Left Bumper)**: Decrease movement step size (minimum 0.005, decrements by 0.01)
- **R1 (Right Bumper)**: Increase movement step size (maximum 0.20, increments by 0.01)
- **SELECT**: Reset keystone to defaults

### Display Toggles
- **START**: Toggle keystone corners visibility (show/hide numbered markers)
- **B Button**: Toggle help overlay
- **HOME (Guide)**: Toggle border outline

### Application Control
- **START + SELECT (hold 2 seconds)**: Quit application

## Step Size Control

The movement step size determines how much each corner moves with D-pad/stick input:

- **Default**: 0.05 units per input
- **Range**: 0.005 (fine) to 0.20 (coarse)
- **Adjustment**: L1 decreases, R1 increases
- **Display**: Printed to console when changed

Use smaller step sizes for fine-tuning alignment, larger for quick adjustments.

## Bluetooth Pairing

To pair your 8BitDo Zero 2:

1. **Put gamepad in pairing mode**: Hold START for 3 seconds (LED flashes)
2. **Scan for devices**: `bluetoothctl` → `scan on`
3. **Pair**: `pair XX:XX:XX:XX:XX:XX` (replace with your device MAC)
4. **Trust**: `trust XX:XX:XX:XX:XX:XX`
5. **Connect**: `connect XX:XX:XX:XX:XX:XX`

The gamepad will auto-connect on subsequent startups if trusted.

## Dual Input Support

- Keyboard and gamepad work simultaneously
- All keyboard shortcuts remain functional
- Gamepad-specific features (cycle corner, step size) only available via gamepad
- Terminal input mode and gamepad can be used together

## Technical Details

### Input Device Detection

The player scans `/dev/input/event*` devices for:
- Device names containing: "8BitDo", "Gamepad", "Xbox", "Controller"
- Devices with button capabilities (BTN_SOUTH/BTN_A)
- Devices with axis capabilities (ABS_X or ABS_HAT0X)

### Button Mappings (evdev codes)

```
GAMEPAD_BTN_A      = BTN_SOUTH  (304)
GAMEPAD_BTN_B      = BTN_EAST   (305)
GAMEPAD_BTN_X      = BTN_NORTH  (307)
GAMEPAD_BTN_Y      = BTN_WEST   (308)
GAMEPAD_BTN_L1     = BTN_TL     (310)
GAMEPAD_BTN_R1     = BTN_TR     (311)
GAMEPAD_BTN_SELECT = BTN_SELECT (314)
GAMEPAD_BTN_START  = BTN_START  (315)
GAMEPAD_BTN_HOME   = BTN_MODE   (316)
```

### Axis Mappings

```
GAMEPAD_AXIS_LEFT_X  = ABS_X     (0)   - Left stick horizontal
GAMEPAD_AXIS_LEFT_Y  = ABS_Y     (1)   - Left stick vertical
GAMEPAD_AXIS_DPAD_X  = ABS_HAT0X (16)  - D-pad horizontal (-1/0/1)
GAMEPAD_AXIS_DPAD_Y  = ABS_HAT0Y (17)  - D-pad vertical (-1/0/1)
```

## Troubleshooting

### Gamepad Not Detected

1. Check Bluetooth connection: `bluetoothctl devices`
2. Verify device permissions: `ls -l /dev/input/event*`
3. Test with `evtest`: `sudo evtest /dev/input/eventX`
4. Ensure gamepad is in the correct mode (not keyboard mode)

### Input Lag

- Ensure Bluetooth is not congested (move closer to Pi)
- Check for RF interference from WiFi (use 5GHz band if possible)
- Verify battery level (low battery causes input lag)

### Wrong Button Mappings

Different controller modes may use different button codes. The 8BitDo Zero 2 should be in "X-input" or "D-input" mode (START+X or START+A at power-on).

## Files Modified

- `input_handler.h` - Added gamepad state structures and button definitions
- `input_handler.c` - Gamepad detection, event processing
- `video_player.c` - Gamepad action handling (cycle corner, step size)
- `keystone.h` / `keystone.c` - Step size adjustment functions

## Future Enhancements

- Support for other gamepads (Xbox, PS4, generic)
- Configurable button mappings
- Analog stick sensitivity adjustment
- Rumble/vibration feedback (if supported)
