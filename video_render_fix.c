// Fix for video rendering issue in gl_context.c
// Modify the gl_render_frame function as follows:

void gl_render_frame(gl_context_t *gl, uint8_t *y_data, uint8_t *u_data, uint8_t *v_data, 
                    int width, int height, int y_stride, int u_stride, int v_stride,
                    display_ctx_t *drm, keystone_context_t *keystone) {
    static int frame_rendered = 0;
    static int last_width = 0, last_height = 0;
    static bool gl_state_set = false;
    
    // Set black clear color for video background
    if (frame_rendered == 0) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // Black background
    }
    
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Set up OpenGL state only once or when needed
    if (!gl_state_set) {
        glViewport(0, 0, drm->width, drm->height);
        gl_state_set = true;
    }
    
    // CRITICAL: Complete buffer unbinding before state restoration
    // This is needed to prevent state leakage from previous operations
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glUseProgram(0);
    
    // Reset vertex attribute arrays
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    
    // Now set up the correct state for video rendering
    glUseProgram(gl->program);
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);
    
    // Set up vertex attributes
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    
    // Explicitly disable blending - overlays might have enabled it
    glDisable(GL_BLEND);
    
    // Disable depth test - make sure video is always visible
    glDisable(GL_DEPTH_TEST);
    
    // Set MVP matrix (identity)
    float mvp_matrix[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->u_mvp_matrix, 1, GL_FALSE, mvp_matrix);
    
    // Set keystone matrix
    const float *keystone_matrix = keystone_get_matrix(keystone);
    glUniformMatrix4fv(gl->u_keystone_matrix, 1, GL_FALSE, keystone_matrix);
    
    // Ensure texture units are properly bound
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl->texture_y);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gl->texture_u);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, gl->texture_v);
    
    // Set texture sampler uniforms
    glUniform1i(gl->u_texture_y, 0);
    glUniform1i(gl->u_texture_u, 1);
    glUniform1i(gl->u_texture_v, 2);
    
    // Update YUV textures if video data is provided
    if (y_data && u_data && v_data) {
        // Calculate UV dimensions (usually half resolution for 4:2:0)
        int uv_width = width / 2;
        int uv_height = height / 2;
        
        // OPTIMIZED: Check if we can use direct upload for all planes
        bool y_direct = (y_stride == width);
        bool u_direct = (u_stride == uv_width);
        bool v_direct = (v_stride == uv_width);
        bool size_changed = (width != last_width || height != last_height || frame_rendered == 0);
        
        if (size_changed) {
            printf("YUV strides: Y=%d U=%d V=%d (dimensions: %dx%d, UV: %dx%d)\n", 
                   y_stride, u_stride, v_stride, width, height, uv_width, uv_height);
            printf("Direct upload: Y=%s U=%s V=%s\n", y_direct?"YES":"NO", u_direct?"YES":"NO", v_direct?"YES":"NO");
        }
        
        // Y texture - OPTIMIZED: minimize texture operations
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gl->texture_y);
        
        // Set proper texture parameters (might have been changed by overlays)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        if (y_direct) {
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, y_data);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, y_data);
            }
        } else {
            // Only use stride copying when absolutely necessary
            static uint8_t *y_temp_buffer = NULL;
            static int y_temp_size = 0;
            int needed_size = width * height;
            if (y_temp_size < needed_size) {
                free(y_temp_buffer);
                y_temp_buffer = malloc(needed_size);
                y_temp_size = needed_size;
            }
            uint8_t *src = y_data, *dst = y_temp_buffer;
            for (int row = 0; row < height; row++) {
                memcpy(dst, src, width);
                src += y_stride;
                dst += width;
            }
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, y_temp_buffer);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, y_temp_buffer);
            }
        }
        
        // U texture - OPTIMIZED: minimize texture operations  
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gl->texture_u);
        
        // Set proper texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        if (u_direct) {
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, u_data);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, u_data);
            }
        } else {
            static uint8_t *u_temp_buffer = NULL;
            static int u_temp_size = 0;
            int needed_size = uv_width * uv_height;
            if (u_temp_size < needed_size) {
                free(u_temp_buffer);
                u_temp_buffer = malloc(needed_size);
                u_temp_size = needed_size;
            }
            uint8_t *src = u_data, *dst = u_temp_buffer;
            for (int row = 0; row < uv_height; row++) {
                memcpy(dst, src, uv_width);
                src += u_stride;
                dst += uv_width;
            }
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, u_temp_buffer);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, u_temp_buffer);
            }
        }
        
        // V texture - OPTIMIZED: minimize texture operations
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, gl->texture_v);
        
        // Set proper texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        if (v_direct) {
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, v_data);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, v_data);
            }
        } else {
            static uint8_t *v_temp_buffer = NULL;
            static int v_temp_size = 0;
            int needed_size = uv_width * uv_height;
            if (v_temp_size < needed_size) {
                free(v_temp_buffer);
                v_temp_buffer = malloc(needed_size);
                v_temp_size = needed_size;
            }
            uint8_t *src = v_data, *dst = v_temp_buffer;
            for (int row = 0; row < uv_height; row++) {
                memcpy(dst, src, uv_width);
                src += v_stride;
                dst += uv_width;
            }
            if (size_changed) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, v_temp_buffer);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, v_temp_buffer);
            }
        }
        
        if (frame_rendered == 0) {
            printf("GPU YUV→RGB rendering started (%dx%d)\n", width, height);
        }
        last_width = width;
        last_height = height;
        
        frame_rendered++;
    } else if (frame_rendered == 0) {
        printf("Waiting for video data...\n");
    }
    
    // Check for OpenGL errors before drawing
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        printf("OpenGL error before draw: 0x%x\n", error);
    }
    
    // Draw quad
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
    // Check for OpenGL errors after drawing
    error = glGetError();
    if (error != GL_NO_ERROR) {
        printf("OpenGL error after draw: 0x%x\n", error);
    }
}