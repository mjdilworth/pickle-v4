# Keystone Correction Controls

## Quick Reference

| Key | Action |
|-----|--------|
| `1-4` | Select corner (visual numbering on screen) |
| `Arrow keys` | Move selected corner |
| `S` | Save current keystone settings |
| `R` | Reset keystone to defaults |
| `C` | Toggle corner markers on/off |
| `B` | Toggle border lines on/off |
| `H` | Toggle help overlay on/off |
| `Q` or `ESC` | Quit application |

## Configuration

Settings are automatically:
- **Loaded** on startup from `pickle_keystone.conf` (if it exists)
- **Saved** on exit (automatically)
- **Saved** manually by pressing `S` key

The configuration file stores:
- Corner positions (4 corners)
- Display states (corners visible, border visible, help visible)

Your keystone correction and UI preferences persist between runs.

### Config File Format

The config file (`pickle_keystone.conf`) is a simple text format:
```ini
# Pickle Keystone Configuration v1.0
show_corners=1
show_border=1
show_help=1
# Corner positions (x y)
corner0=-1.000000 1.000000
corner1=1.000000 1.000000
corner2=1.000000 -1.000000
corner3=-1.000000 -1.000000
```

**Note:** The save format is backward compatible - old config files will still load but won't preserve overlay states.

## Corner Numbering

Corners are numbered visually on screen:
- Corner 1: Bottom-left
- Corner 2: Bottom-right  
- Corner 3: Top-right
- Corner 4: Top-left

## Usage Tips

1. **Select a corner** by pressing `1`, `2`, `3`, or `4`
   - The selected corner will turn bright green
   - Other corners remain red

2. **Move the corner** with arrow keys
   - ↑ = move up
   - ↓ = move down
   - ← = move left
   - → = move right

3. **Save your work** by pressing `S`
   - Settings auto-save on exit too

4. **Start over** by pressing `R` to reset to defaults

5. **Visual aids**:
   - Press `C` to toggle corner markers
   - Press `B` to toggle border lines
   - Press `H` to toggle help text

## Troubleshooting

If you see "Permission denied" errors:
```bash
# Stop display manager:
sudo systemctl stop lightdm

# Then run:
./pickle your_video.mp4

# Or use sudo:
sudo ./pickle your_video.mp4
```
