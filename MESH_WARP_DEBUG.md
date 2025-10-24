# Mesh Warp Issue - Investigation Summary

## What I've Found

When you press 'M' to toggle mesh warp mode:

1. **Matrix Check**: The transformation matrix remains unchanged (identity) when mesh mode is toggled with default mesh points. This means the video geometry should NOT change.

2. **Mesh Rendering**: When mesh_enabled=true:
   - Mesh grid lines (cyan) are drawn on top
   - 64 mesh control points are rendered as small squares
   - This overlay might be what appears as "inversion"

3. **Input Handling**: Fixed to use dedicated `toggle_mesh_warp` flag instead of array indexing

## Possible Explanations for "Inverts"

### Option A: Visual Artifact from Grid Overlay
- The cyan mesh grid and points render on top of the video
- This might make the display appear distorted or "inverted"
- **Test**: Press 'C' to hide corners/points while mesh is on

### Option B: Matrix Calculation Issue  
- Even though debug shows matrix unchanged, there might be a subtle calculation error
- **Test**: Move a mesh point slightly and see if video actually deforms

### Option C: Rendering State Issue
- Some GL state not being properly restored after mesh rendering
- **Test**: Enable/disable mesh multiple times

## What I Changed

1. Added `toggle_mesh_warp` flag to input_handler_t
2. Updated both stdin and event device handlers to use this flag
3. Updated video_player.c to check and reset this flag
4. Fixed mesh VBO rendering to properly restore GL state
5. Added debug output for matrix recalculation

## Next Steps

Please clarify what "invert" means:
- Does the video image flip upside down or left-right?
- Does the color invert (white becomes black)?
- Does the display get obscured by the mesh grid?
- Something else?

## Testing

Press 'M' to toggle mesh mode on/off and describe what you see.
Press 'C' to toggle corner visibility while in mesh mode.
