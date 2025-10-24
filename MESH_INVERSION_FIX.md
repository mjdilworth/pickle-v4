# Mesh Warp Inversion Issue - FIXED

## Problem
When pressing 'M' to enable mesh warp mode, the video would:
1. Go full-screen
2. Appear upside-down
3. Return to normal when 'M' is pressed again

## Root Cause
The mesh grid was always initialized to identity coordinates ([-1,1]) during startup, regardless of what the current keystone corners were. 

When a saved keystone configuration was loaded:
- Keystone corners would be at custom positions (e.g., trapezoid correction)
- Mesh corners remained at identity
- Toggling mesh mode would switch between the two transformations
- Result: Visual "inversion" as the perspective changed

## Solution
Modified `keystone_toggle_mesh_warp()` to:
1. When mesh mode is ENABLED: Initialize mesh grid corners to match current keystone corners
2. Use bilinear interpolation to set interior mesh points based on the corners
3. This ensures NO visual jump when toggling mesh mode

## Implementation
When mesh mode is enabled:
```c
// Set mesh corners to match current keystone corners
mesh.grid[0][0] = corners[TL]
mesh.grid[0][7] = corners[TR]
mesh.grid[7][7] = corners[BR]
mesh.grid[7][0] = corners[BL]

// Interpolate interior points
for (y, x): interpolate from corners using bilinear blending
```

## Result
- Press 'M' to toggle mesh mode: video stays in same position
- Mesh grid overlays on top without changing perspective
- User can move individual mesh points to adjust warping
- Press 'M' again: returns to corner-only mode smoothly

## Testing
The fix has been verified with loaded keystone configuration:
- Mesh corners now match keystone corners when toggled
- No more visual inversion or full-screen effect
- Smooth transition between mesh and corner modes
