# Makefile for RPi4 Video Player with Keystone Correction

# Compiler and flags
CC = gcc

# OPTIMIZED: RPi4-specific compiler flags
CFLAGS = -Wall -Wextra -std=c99 -O2 -g

# ARM CPU Detection and NEON SIMD optimization (works for both armv7 and aarch64)
ARCH_FLAGS = $(shell uname -m | grep -qE 'arm|aarch64' && echo '-ftree-vectorize -ffast-math')
# RPi4-specific: Cortex-A72 CPU tuning with NEON intrinsics
ifeq ($(shell uname -m),aarch64)
    ARCH_FLAGS += -march=armv8-a+crc -mtune=cortex-a72 -mcpu=cortex-a72
endif
CFLAGS += $(ARCH_FLAGS)

# Additional RPi4 optimizations
RPi4_FLAGS = -pipe -fomit-frame-pointer -finline-functions -ffunction-sections -fdata-sections
CFLAGS += $(RPi4_FLAGS)

# Link-time optimization (reduce binary size, improve performance)
CFLAGS += -flto=auto
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

# Build the executable with optimizations
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -Wl,--gc-sections -Wl,-O1 -o $(TARGET) $(OBJECTS) $(LIBS)

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

# Strip binary for smaller size (optional)
strip:
	strip $(TARGET)

# Show build flags
show-flags:
	@echo "Build flags for this system:"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "ARCH_FLAGS: $(ARCH_FLAGS)"
	@echo "System: $$(uname -m)"

# Release build with maximum optimization
release: CFLAGS = -Wall -Wextra -std=c99 -O3 -DNDEBUG
release: CFLAGS += $(ARCH_FLAGS) $(RPi4_FLAGS)
release: CFLAGS += -flto=auto -fvisibility=hidden -ffunction-sections -fdata-sections
# RPi4-specific release optimizations
ifeq ($(shell uname -m),aarch64)
release: CFLAGS += -march=armv8-a+crc+simd -mtune=cortex-a72 -mcpu=cortex-a72
endif
release: LDFLAGS = -Wl,--gc-sections,-s -flto=auto
release: clean $(TARGET)
	@echo "Release build complete: $(TARGET)"
	@ls -lh $(TARGET)

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
	@echo "Pickle Video Player - RPi4 Makefile"
	@echo ""
	@echo "Available targets:"
	@echo "  all          - Build the video player (default, -O2 optimization)"
	@echo "  release      - Build with maximum optimization (-O3 -flto, stripped)"
	@echo "  debug        - Build with debug symbols (-ggdb3)"
	@echo "  clean        - Remove generated files"
	@echo "  rebuild      - Clean and build"
	@echo "  install-deps - Install required system dependencies"
	@echo "  test         - Run with test.mp4 if available"
	@echo "  info         - Show build configuration"
	@echo "  show-flags   - Display compiler optimization flags"
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
.PHONY: all run test clean rebuild debug release info help install-deps