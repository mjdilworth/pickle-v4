# Configuration Save/Load Implementation

## What Gets Saved

The keystone configuration now saves and restores:

1. **Corner Positions** (4 corners with X, Y coordinates)
2. **Show Corners** (whether corner markers are visible)
3. **Show Border** (whether border lines are visible)  
4. **Show Help** (whether help overlay is visible)

## File Format

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

## When Settings Are Saved

- **Automatically on exit** - Settings save when you quit the player
- **Press 'S' key** - Manual save at any time during playback
- **Status message** - You'll see "Keystone settings saved" confirmation

## When Settings Are Loaded

- **On startup** - Settings load automatically from `pickle_keystone.conf`
- **Status message** - You'll see either:
  - "Loaded saved keystone settings from pickle_keystone.conf"
  - "No saved keystone settings found, using defaults"

## Backward Compatibility

Old format config files (4 lines with just X Y coordinates) will fail to load and the player will use defaults. The new format will be written on next save.

## Testing

To verify it's working:

```bash
# 1. Start the player
./pickle test_video.mp4

# 2. Press '1' to select corner 1
# 3. Use arrow keys to move it
# 4. Press 'C' to hide corners
# 5. Press 'B' to hide border
# 6. Press 'S' to save (or just quit with 'Q')

# 7. Check the config file
cat pickle_keystone.conf

# 8. Start again - your settings should be restored
./pickle test_video.mp4
```

The corner positions AND overlay visibility states should persist!
