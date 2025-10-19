# Makefile for RPi4 Video Player with Keystone Correction

# Compiler and flags
CC = gcc
# OPTIMIZED: Enable ARM NEON SIMD when available
CFLAGS = -Wall -Wextra -std=c99 -O2 -g
ARCH_FLAGS = $(shell uname -m | grep -q 'arm' && echo '-mfpu=neon -ftree-vectorize')
CFLAGS += $(ARCH_FLAGS)
TARGET = pickle
SOURCES = pickel.c video_player.c drm_display.c gl_context.c video_decoder.c keystone.c input_handler.c v4l2_utils.c
OBJECTS = $(SOURCES:.c=.o)

# Library dependencies for RPi4
LIBS = -ldrm -lgbm -lEGL -lGLESv2 -lavformat -lavcodec -lavutil -lswscale -lpthread -lm

# Include paths
INCLUDES = -I/usr/include/libdrm -I/usr/include/ffmpeg

# PKG-config for better library detection
PKG_CFLAGS = $(shell pkg-config --cflags libdrm gbm egl glesv2 libavformat libavcodec libavutil libswscale 2>/dev/null)
PKG_LIBS = $(shell pkg-config --libs libdrm gbm egl glesv2 libavformat libavcodec libavutil libswscale 2>/dev/null)

# Use pkg-config if available, fallback to manual libs
ifneq ($(PKG_LIBS),)
    LIBS = $(PKG_LIBS) -lpthread -lm
endif

ifneq ($(PKG_CFLAGS),)
    INCLUDES += $(PKG_CFLAGS)
endif

# Default target
all: $(TARGET)

# Build the executable
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LIBS)

# Compile source files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Run the program (requires video file argument)
run: $(TARGET)
	@echo "Usage: ./$(TARGET) <video_file.mp4>"
	@echo "Make sure you have a video file to test with"

# Test with a sample video (if available)
test: $(TARGET)
	@if [ -f test.mp4 ]; then \
		echo "Running with test.mp4..."; \
		sudo ./$(TARGET) test.mp4; \
	else \
		echo "No test.mp4 file found. Please provide a video file:"; \
		echo "sudo ./$(TARGET) your_video.mp4"; \
	fi

# Install dependencies (Debian/Ubuntu/Raspberry Pi OS)
install-deps:
	@echo "Installing development dependencies..."
	sudo apt-get update
	sudo apt-get install -y \
		build-essential \
		libdrm-dev \
		libgbm-dev \
		libegl1-mesa-dev \
		libgles2-mesa-dev \
		libavformat-dev \
		libavcodec-dev \
		libavutil-dev \
		libswscale-dev \
		pkg-config

# Clean up generated files
clean:
	rm -f $(TARGET) $(OBJECTS)

# Rebuild (clean + build)
rebuild: clean all

# Debug build with extra symbols
debug: CFLAGS += -DDEBUG -ggdb3
debug: $(TARGET)

# Show build information
info:
	@echo "Target: $(TARGET)"
	@echo "Sources: $(SOURCES)"
	@echo "Objects: $(OBJECTS)"
	@echo "Compiler: $(CC)"
	@echo "Flags: $(CFLAGS)"
	@echo "Includes: $(INCLUDES)"
	@echo "Libraries: $(LIBS)"

# Help target
help:
	@echo "Available targets:"
	@echo "  all          - Build the video player (default)"
	@echo "  clean        - Remove generated files"
	@echo "  rebuild      - Clean and build"
	@echo "  debug        - Build with debug symbols"
	@echo "  install-deps - Install required system dependencies"
	@echo "  test         - Run with test.mp4 if available"
	@echo "  info         - Show build configuration"
	@echo "  help         - Show this help"
	@echo ""
	@echo "Usage: sudo ./$(TARGET) <video_file.mp4>"
	@echo "Note: Requires root privileges for direct hardware access (DRM/KMS)"

# Dependencies
pickel.o: pickel.c video_player.h
video_player.o: video_player.c video_player.h drm_display.h gl_context.h video_decoder.h keystone.h input_handler.h
drm_display.o: drm_display.c drm_display.h
gl_context.o: gl_context.c gl_context.h drm_display.h
video_decoder.o: video_decoder.c video_decoder.h
keystone.o: keystone.c keystone.h
input_handler.o: input_handler.c input_handler.h

# Phony targets
.PHONY: all run test clean rebuild debug info help install-deps