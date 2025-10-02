# DRM Permission Troubleshooting Guide

## Problem: "Failed to set CRTC mode: Permission denied"

This error occurs when another process has control of the display (DRM master).

## Quick Diagnosis

Check your environment:
```bash
echo "Display: $DISPLAY"
echo "Session: $XDG_SESSION_TYPE"
tty
```

## Solutions (in order)

### 1. **Check for running instances**
```bash
# Kill any existing pickle instances
pkill pickle

# Check if anything else is using DRM
ps aux | grep -E "(X|wayland|weston|kms)"
```

### 2. **Stop display managers** (if running desktop)
```bash
# For Raspberry Pi OS Desktop
sudo systemctl stop lightdm

# For other systems
sudo systemctl stop gdm      # GNOME
sudo systemctl stop sddm     # KDE
sudo systemctl stop weston   # Wayland
```

### 3. **Run with sudo** (quick test)
```bash
sudo ./pickle your_video.mp4
```

### 4. **Add proper permissions** (permanent fix)
```bash
# Add your user to video and render groups
sudo usermod -a -G video,render $USER

# Logout and login for changes to take effect
```

### 5. **Direct console access** (if via SSH)

If you're SSHed in, you may need direct console:

**Option A: Switch to physical console**
- Press Ctrl+Alt+F1 (or F2-F6) on the Pi's keyboard
- Login directly
- Run: `./pickle your_video.mp4`

**Option B: Set console as default**
```bash
# Disable desktop on boot
sudo systemctl set-default multi-user.target
sudo reboot

# To re-enable desktop later
sudo systemctl set-default graphical.target
```

## Verify It's Working

When pickle starts successfully, you should see:
```
Successfully became DRM master
Using CRTC 100, Encoder 32, Connector 33
...
Setting display mode...
Display initialized. Video should appear now.
```

## Still Not Working?

Check DRM device permissions:
```bash
ls -la /dev/dri/
# You should see card0, card1, etc.

# Check your groups
groups
# Should include: video, render
```

Check if something is holding DRM:
```bash
sudo lsof /dev/dri/card*
```

## Working Environment

Pickle works best on:
- Direct console (TTY1-6)
- No X11/Wayland running
- User in video,render groups
- Or running with sudo

It does NOT work with:
- X11 desktop sessions
- Wayland desktop sessions
- VNC sessions
- Remote desktop
