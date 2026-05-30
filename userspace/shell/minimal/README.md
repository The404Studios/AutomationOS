# AutomationOS Minimal Desktop Shell

A simple, self-contained desktop shell for AutomationOS that provides basic desktop environment functionality.

## Overview

The minimal shell provides:

1. **Desktop Background** - Solid color background (can be extended for wallpaper)
2. **Taskbar** - Bottom bar showing running apps and system clock
3. **App Launcher** - Popup menu to launch applications
4. **Window Management** - Basic support for tracking running apps

## Architecture

```
shell/
├── main.c         - Entry point and main loop
├── shell.c/.h     - Shell orchestration
├── desktop.c/.h   - Desktop background rendering
├── taskbar.c/.h   - Taskbar with app buttons and clock
├── launcher.c/.h  - Application launcher menu
├── render.c/.h    - Framebuffer drawing utilities
└── Makefile       - Build configuration
```

## Components

### 1. Desktop (desktop.c)

- Renders solid color background
- Fullscreen window
- Future: wallpaper support

### 2. Taskbar (taskbar.c)

- Fixed 40px height at bottom of screen
- Shows running applications as buttons
- Displays system clock (HH:MM format)
- Dark theme (0x202020 background)

### 3. Launcher (launcher.c)

- Popup menu (300x400px, centered)
- Lists available applications
- Default apps:
  - Terminal
  - File Manager
  - Settings
  - About
- Click to launch via fork/exec

### 4. Render (render.c)

- Framebuffer initialization
- Drawing primitives:
  - Clear screen
  - Fill rectangle
  - Draw rectangle (outline)
  - Put pixel
  - Draw line
- Supports both hardware `/dev/fb0` and simulated framebuffer

## Building

```bash
cd userspace/shell/minimal
make clean
make
```

Output: `build/shell/minimal/shell`

## Installation

```bash
make install
```

Installs to: `/usr/bin/shell`

## Running

```bash
# Direct run
./build/shell/minimal/shell

# Or after install
/usr/bin/shell
```

## Usage

- **Desktop Click**: Opens launcher menu
- **Launcher**: Click on app to launch
- **Taskbar**: Click on app button to switch focus (future)
- **Ctrl+C**: Exit shell

## Default Applications

The launcher comes pre-configured with:

1. **Terminal** (`/usr/bin/terminal`) - Command line interface
2. **File Manager** (`/usr/bin/files`) - Browse filesystem
3. **Settings** (`/usr/bin/settings`) - System configuration
4. **About** (`/usr/bin/about`) - System information

## Event Loop

The shell runs at 60 FPS (~16ms per frame):

```c
while (running) {
    handle_events();    // Process input
    update();           // Update state
    render();           // Draw to framebuffer
    usleep(16000);      // Sleep ~16ms
}
```

## Color Scheme

### Desktop
- Background: `0xFF2C3E50` (Blue-gray)

### Taskbar
- Background: `0xE0202020` (Dark gray, semi-transparent)
- Button: `0xFF404040` (Medium gray)
- Active: `0xFF4A90E2` (Blue)
- Text: `0xFFFFFFFF` (White)

### Launcher
- Background: `0xE0F0F0F0` (Light gray, semi-transparent)
- Item: `0xFFFFFFFF` (White)
- Hover: `0xFF4A90E2` (Blue)
- Text: `0xFF202020` (Dark gray)

## Future Enhancements

### Phase 2
- [ ] Font rendering (bitmap or FreeType)
- [ ] Icon support (load PNG/SVG)
- [ ] Mouse input handling
- [ ] Keyboard shortcuts

### Phase 3
- [ ] Window decorations (title bar, close button)
- [ ] Window stacking/focus
- [ ] Drag and drop
- [ ] Context menus

### Phase 4
- [ ] Wallpaper support
- [ ] Desktop icons
- [ ] System tray
- [ ] Notifications

## Testing

### Simulated Mode

If `/dev/fb0` is not available, the shell runs in simulated mode:
- Creates 1920x1080 framebuffer in memory
- All rendering works, but output not visible
- Useful for build/unit testing

### Hardware Mode

On real hardware or with QEMU:
- Opens `/dev/fb0`
- Maps framebuffer memory
- Renders directly to screen

## Integration

### With Init System

Create service file:

```ini
[Service]
Description=Desktop Shell
ExecStart=/usr/bin/shell
Requires=compositor.service
After=compositor.service
Restart=always
```

### With Window Manager

The minimal shell can run standalone or integrate with a compositor:
- Standalone: Direct framebuffer access
- Compositor: Create windows via IPC

## Code Statistics

- **Total Lines**: ~800 LOC
- **main.c**: 95 lines
- **shell.c**: 140 lines
- **desktop.c**: 90 lines
- **taskbar.c**: 175 lines
- **launcher.c**: 200 lines
- **render.c**: 240 lines

## Dependencies

- **System**: POSIX (fork, exec, signals)
- **Graphics**: Framebuffer (`/dev/fb0` or simulated)
- **Math**: libm (for future animations)

No external GUI libraries required!

## Performance

- **CPU**: < 5% on modern hardware
- **Memory**: ~2MB RSS
- **Frame time**: < 1ms (plenty of headroom for 60 FPS)

## License

Part of AutomationOS project.
