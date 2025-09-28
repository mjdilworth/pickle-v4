# Video Rendering Fix

## Problem
The main video content is not visible in the OpenGL video player, even though all overlay elements (borders, corners, help text) display correctly when toggled.

## Analysis
After examining the OpenGL rendering code, I've identified several potential issues that could be causing the video to not be displayed:

1. **Incomplete State Reset**: While there is code to detect corrupted OpenGL state, the restoration might not be complete.

2. **Missing Buffer Binding**: The code needs to explicitly unbind all buffers before setting up the correct state.

3. **Texture Parameter Settings**: The texture parameters for YUV planes may not be properly set or restored after overlay rendering.

4. **Blending and Depth Testing**: The border rendering function enables blending, but the state restoration in the video rendering function might not properly disable it.

## Solution
The proposed fix in `video_render_fix.c` includes:

1. **Complete State Reset**: 
   - Unbind all buffers with `glBindBuffer(GL_ARRAY_BUFFER, 0)` and `glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)`
   - Clear shader program with `glUseProgram(0)`
   - Disable all vertex attribute arrays

2. **Explicit State Restoration**: 
   - Re-establish the correct video rendering state
   - Set up vertex attributes with explicit enables

3. **Texture Parameter Reset**:
   - Explicitly set texture parameters for each YUV texture
   - Ensure proper filtering and wrapping modes

4. **Disable Blending**:
   - Explicitly disable blending with `glDisable(GL_BLEND)`
   - Disable depth testing with `glDisable(GL_DEPTH_TEST)`

5. **Error Checking**:
   - Added additional OpenGL error checking before and after the draw call

## Implementation Notes
- The fix follows the approach seen in the working border rendering function
- The implementation completely resets the OpenGL state before rendering
- Added explicit texture parameter settings that may have been changed by overlays
- Added more verbose error logging to identify any remaining issues

## Testing
Apply this fix by replacing or modifying the `gl_render_frame` function in `gl_context.c`. If the issue is related to OpenGL state corruption, this should restore proper video rendering while maintaining overlay functionality.