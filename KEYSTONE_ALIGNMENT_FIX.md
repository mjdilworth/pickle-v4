# Keystone Video and Overlay Alignment Fix

## Problem Identified

The video rendering and overlay indicators (corners/borders) were not properly aligned because the keystone transformation matrix was calculated in the wrong direction.

## Root Cause

### Original (Incorrect) Implementation:
- **Matrix calculation**: Mapped FROM keystone trapezoid TO standard rectangle
  - Source: keystone corner positions (trapezoid)
  - Destination: standard quad (-1,1), (1,1), (1,-1), (-1,-1)
- **Result**: The matrix transformed keystone → rectangle instead of rectangle → keystone
- **Effect**: Video quad stayed rectangular while overlays were drawn at keystone positions, causing misalignment

### Fixed Implementation:
- **Matrix calculation**: Maps FROM standard rectangle TO keystone trapezoid
  - Source: standard quad (-1,1), (1,1), (1,-1), (-1,-1)  
  - Destination: keystone corner positions (trapezoid)
- **Result**: The matrix correctly transforms the video quad from rectangle to the desired keystone shape
- **Effect**: Video and overlays are now properly aligned at the keystone corner positions

## Changes Made

### 1. `keystone.c` - Fixed transformation direction
```c
// OLD (incorrect):
float src_x[4] = {corners[0].x, corners[1].x, corners[2].x, corners[3].x}; // keystone
float dst_x[4] = {-1.0f, 1.0f, 1.0f, -1.0f}; // rectangle
// This created: keystone → rectangle transformation

// NEW (correct):
float src_x[4] = {-1.0f, 1.0f, 1.0f, -1.0f}; // rectangle
float dst_x[4] = {corners[0].x, corners[1].x, corners[2].x, corners[3].x}; // keystone
// This creates: rectangle → keystone transformation
```

### 2. Area calculation updated
Changed degenerate detection to check destination (keystone) area instead of source area:
```c
// Check destination keystone shape area
area += dst_x[i] * dst_y[j] - dst_x[j] * dst_y[i];
```

## How It Works Now

### Rendering Pipeline:
1. **Video quad**: Starts as standard rectangle (-1,-1) to (1,1)
2. **Keystone matrix**: Transforms rectangle TO user-defined trapezoid positions
3. **Overlays**: Drawn directly at keystone corner positions
4. **Result**: Video and overlays are perfectly aligned

### Vertex Shader:
```glsl
vec4 pos = vec4(a_position, 0.0, 1.0); // Standard quad vertex
pos = u_keystone_matrix * pos;          // Transform TO keystone position
gl_Position = u_mvp_matrix * pos;       // Final screen position
```

### Overlay Rendering:
- Corner indicators drawn at `keystone->corners[i].x/y` positions
- Border lines connect the same corner positions
- No transformation needed for overlays (already in screen space)

## Testing

The video quad and corner/border overlays now move together when adjusting keystone corners using keyboard controls (1/2/3/4 to select corner, arrow keys to move).

## Additional Stability Improvements

The fix also includes:
- Smaller movement step (5% vs 10%) for finer control
- Area validation to prevent degenerate configurations
- Improved numerical stability in the linear solver
- Bounds checking to prevent extreme transformations
