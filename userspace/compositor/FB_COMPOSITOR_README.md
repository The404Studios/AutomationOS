# AutomationOS Framebuffer Compositor

**Software rendering compositor using direct framebuffer access.**

## Overview

This compositor replaces the GPU-dependent OpenGL implementation with a pure software rendering solution. It uses the framebuffer device (`/dev/fb0`) for direct pixel manipulation and implements efficient compositing using the painter's algorithm.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Applications                             │
│  (Terminal, File Manager, Desktop Shell, etc.)             │
└────────────┬────────────────────────────────────────────────┘
             │ IPC Messages (create/update/move windows)
             ▼
┌─────────────────────────────────────────────────────────────┐
│              Compositor IPC Interface                       │
│  - Message queue for commands                               │
│  - Shared memory for window surfaces                        │
└────────────┬────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────┐
│           Window Composition Engine                         │
│  - Painter's algorithm (back-to-front)                      │
│  - Damage tracking (only redraw changed regions)            │
│  - Z-order management                                       │
│  - Window decorations (title bar, borders)                  │
└────────────┬────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────┐
│              Blitting Engine                                │
│  - Alpha blending (premultiplied alpha)                     │
│  - Optimized memcpy for opaque blits                        │
│  - Per-pixel compositing for transparency                   │
└────────────┬────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────┐
│           Double Buffering                                  │
│  - Back buffer (off-screen)                                 │
│  - Front buffer (framebuffer)                               │
│  - Tear-free page flip                                      │
└────────────┬────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────┐
│          Framebuffer Device (/dev/fb0)                      │
│  - Memory-mapped 1024x768x32 RGBA                           │
│  - Direct hardware access                                   │
└─────────────────────────────────────────────────────────────┘
```

## Features

### Core Functionality
- **Software Rendering**: Pure CPU rendering, no GPU dependencies
- **Window Composition**: Painter's algorithm for layered windows
- **Alpha Blending**: Premultiplied alpha for transparent windows
- **Double Buffering**: Eliminates tearing artifacts
- **Damage Tracking**: Only redraws changed regions (30+ FPS target)

### Window Management
- Multiple window types (normal, dialog, desktop, dock, etc.)
- Z-order management (raise/lower windows)
- Window decorations (title bar, borders, shadows)
- Focus management
- Move and resize operations

### Performance Optimizations
- Damage tracking: Skips unchanged regions
- Fast opaque blit: Uses memcpy when no alpha
- Rectangular clipping: Efficient boundary checks
- Target: 30+ FPS software rendering

## File Structure

```
compositor/
├── fb_compositor.h        # Main compositor API
├── fb_compositor.c        # Compositor core (frame loop, FPS, etc.)
├── composition.c          # Window composition (painter's algorithm)
├── blit.c                 # Optimized blitting with alpha blending
├── damage.c               # Damage tracking for efficient redraws
├── window.c               # Window creation and management
├── ipc.h                  # IPC protocol definitions
├── ipc.c                  # IPC message handling (Agent 1 integration)
├── compositor_main.c      # Main entry point / daemon
├── fb.h                   # Framebuffer device interface
├── fb.c                   # Framebuffer access implementation
├── Makefile.fb            # Build system
└── FB_COMPOSITOR_README.md # This file
```

## Building

### Prerequisites
- GCC 7.0+ or Clang 6.0+
- Linux kernel with framebuffer support
- 512MB RAM minimum

### Compile
```bash
make -f Makefile.fb
```

### Debug Build
```bash
make -f Makefile.fb debug
```

### Clean
```bash
make -f Makefile.fb clean
```

## Running

### Manual Start
```bash
./build/userspace/compositor/fb_compositor
```

### As Daemon (launched by init)
Add to `/etc/init.d/compositor`:
```bash
#!/bin/sh
/usr/bin/fb_compositor &
```

## IPC Protocol

### Message Types

#### Create Window
```c
MSG_CREATE_WINDOW
Payload: {
    window_type_t type;    // WINDOW_NORMAL, etc.
    int32_t x, y;          // Position
    uint32_t width, height;
    char title[128];
}
```

#### Update Surface
```c
MSG_UPDATE_SURFACE
Payload: {
    uint32_t shm_id;       // Shared memory segment ID
    uint32_t width, height;
    uint32_t offset;       // Offset into shared memory
}
```

#### Move Window
```c
MSG_MOVE_WINDOW
Payload: {
    int32_t x, y;          // New position
}
```

#### Other Messages
- `MSG_DESTROY_WINDOW` - Destroy window
- `MSG_MAP_WINDOW` - Make window visible
- `MSG_UNMAP_WINDOW` - Hide window
- `MSG_SET_TITLE` - Change title
- `MSG_RESIZE_WINDOW` - Resize window
- `MSG_FOCUS_WINDOW` - Focus window
- `MSG_RAISE_WINDOW` - Raise to top
- `MSG_LOWER_WINDOW` - Lower to bottom

### Application Integration

Applications communicate with the compositor via:

1. **Message Queue**: Send commands (create window, move, etc.)
2. **Shared Memory**: Share window pixel buffers for zero-copy updates

Example (pseudocode):
```c
// Create window
msg.type = MSG_CREATE_WINDOW;
msg.window_id = 123;
create_window_request_t *req = (create_window_request_t *)msg.payload;
req->type = WINDOW_NORMAL;
req->x = 100;
req->y = 100;
req->width = 640;
req->height = 480;
strcpy(req->title, "My Application");
msgsnd(compositor_queue, &msg, sizeof(msg), 0);

// Update surface via shared memory
int shm_id = shmget(IPC_PRIVATE, 640 * 480 * 4, 0666 | IPC_CREAT);
uint32_t *pixels = shmat(shm_id, NULL, 0);

// Draw to pixels...

msg.type = MSG_UPDATE_SURFACE;
msg.window_id = 123;
update_surface_request_t *update = (update_surface_request_t *)msg.payload;
update->shm_id = shm_id;
update->width = 640;
update->height = 480;
update->offset = 0;
msgsnd(compositor_queue, &msg, sizeof(msg), 0);
```

## Performance

### Target Metrics
- **Frame Rate**: 30+ FPS (software rendering)
- **Latency**: < 50ms input-to-screen
- **CPU Usage**: < 10% idle, < 30% active
- **Memory**: ~30MB for compositor + window buffers

### Optimization Techniques

1. **Damage Tracking**: Only redraw changed regions
   - Tracks up to 128 damage rectangles per frame
   - Falls back to full redraw if exceeded

2. **Fast Opaque Blit**: Uses memcpy when no alpha
   - 10x faster than per-pixel blending

3. **Alpha Blending**: Premultiplied alpha formula
   - Efficient integer math, no floating point

4. **Clipping**: Early rejection of off-screen regions
   - Reduces unnecessary pixel operations

## Integration Points

### Agent 1: IPC (Shared Memory + Message Queues)
- **Status**: Stub implementation
- **Location**: `ipc.c` - `ipc_init_compositor()`, `ipc_receive_message()`
- **Integration**: Replace stubs with Agent 1's IPC API

### Agent 4: Input Pipeline (/dev/input/event0)
- **Status**: Stub implementation  
- **Location**: `compositor_main.c` - `process_input_events()`
- **Integration**: Read from `/dev/input/event0` for mouse/keyboard

### Agent 6: Font Rendering
- **Status**: TODO
- **Location**: `composition.c` - `draw_decorations()`
- **Integration**: Render window titles using TrueType fonts

### Agent 7: Image Loading
- **Status**: TODO
- **Integration**: Load window icons and wallpapers

### Agent 8: Window Manager
- **Status**: Standalone (can be integrated)
- **Integration**: WM calls compositor IPC to create/manage windows

## Testing

### Manual Test with Test Windows

The compositor creates three test windows on startup:
- **Desktop**: Dark blue-gray background (full screen)
- **Window 1**: Red window (400x300)
- **Window 2**: Green window (350x250)
- **Window 3**: Blue window (300x200)

Run and observe overlapping windows with proper z-order.

### Expected Output
```
======================================
  AutomationOS Framebuffer Compositor
  Software Rendering v1.0
======================================

[FB] Framebuffer mapped successfully:
[FB]   Resolution: 1024x768
[FB]   BPP: 32
[FB]   Pitch: 4096 bytes
[FB]   Size: 3145728 bytes (3.00 MB)
[FB Compositor] Initialized successfully
[FB Compositor]   Resolution: 1024x768
[FB Compositor]   Back buffer: 3.00 MB
[IPC] Running in stub mode (no kernel IPC support yet)
[Compositor] Initialization complete
[Compositor] Press Ctrl+C to exit

[Compositor] Creating test windows...
[Window] Created window 1 (1024x768 at 0,0)
[FB Compositor] Added window 1 (1024x768 at 0,0)
[Compositor]   Desktop window created
[Window] Created window 2 (400x300 at 100,100)
[FB Compositor] Added window 2 (400x300 at 100,100)
[Compositor]   Window 1 created (red)
[Window] Created window 3 (350x250 at 250,200)
[FB Compositor] Added window 3 (350x250 at 250,200)
[Compositor]   Window 2 created (green)
[Window] Created window 4 (300x200 at 400,150)
[FB Compositor] Added window 4 (300x200 at 400,150)
[Compositor]   Window 3 created (blue)

[Compositor] Entering main loop
[FB Compositor] FPS: 60
[FB Compositor] FPS: 60
...
```

## Known Limitations

### Current Implementation
1. **No GPU acceleration** - Pure software rendering (expected)
2. **IPC is stubbed** - Waiting for Agent 1's implementation
3. **No input handling** - Waiting for Agent 4's pipeline
4. **No font rendering** - Title bars are blank (waiting for Agent 6)
5. **No animations** - Instant window transitions (can be added)

### Future Enhancements (Tier 2+)
1. **SIMD optimizations** - Use SSE/AVX for faster blitting
2. **Multi-threading** - Parallel composition on multi-core CPUs
3. **Adaptive FPS** - Lower FPS when idle to save power
4. **Hardware cursors** - Reduce CPU overhead
5. **GPU path (Tier 3)** - Switch to OpenGL when DRM/KMS ready

## Troubleshooting

### Black screen on startup
**Cause**: Framebuffer device not accessible  
**Fix**: Check `/dev/fb0` permissions and kernel framebuffer driver

### Low FPS (< 20 FPS)
**Cause**: Too many windows or large resolution  
**Fix**: Reduce window count or lower resolution

### Tearing artifacts
**Cause**: Double buffering not working  
**Fix**: Verify back buffer allocation in `fb_compositor_init()`

### Windows not visible
**Cause**: Windows not mapped or z-order issue  
**Fix**: Check `window->mapped = true` and z-order values

## Contributing

This is **Agent 5: Framebuffer Compositor Engineer** deliverable. Integration with:
- **Agent 1** (IPC) - Replace IPC stubs
- **Agent 4** (Input) - Add input event handling
- **Agent 6** (Fonts) - Add title rendering
- **Agent 8** (WM) - Integrate window manager

## License

Part of AutomationOS project.

## Contact

Agent 5: Framebuffer Compositor Engineer  
Date: 2026-05-27  
Status: Core implementation complete, ready for integration
