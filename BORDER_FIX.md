## Border Toggle Fix Summary

### Problem
When toggling the border overlay during video playback, the video would disappear due to OpenGL state corruption.

### Root Cause
The border rendering function was changing OpenGL state (shader programs, VBOs) but not properly restoring the main video rendering state afterward.

### Solution
Enhanced the state restoration in `video_player.c`:

1. **Complete Buffer Reset**: Before restoring main video state, we first unbind all buffers:
   ```c
   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
   ```

2. **Explicit State Restoration**: Then explicitly bind the correct video rendering buffers:
   ```c
   glUseProgram(app->gl->program);
   glBindBuffer(GL_ARRAY_BUFFER, app->gl->vbo);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, app->gl->ebo);
   ```

3. **Vertex Attribute Restoration**: Restore the vertex attributes for main video rendering:
   ```c
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
   glEnableVertexAttribArray(1);
   ```

### Files Modified
- `video_player.c`: Enhanced state restoration after overlay rendering
- `keystone.c`: Cleaned up debug output in toggle function

### Status
✅ Border toggle now works without causing video to disappear
✅ Performance optimized by removing excessive debug output
✅ OpenGL state management fixed