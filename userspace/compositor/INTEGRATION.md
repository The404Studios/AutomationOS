# Compositor Framebuffer + Input Integration

## Overview

This document describes the integration of Agent 5's compositor with the kernel framebuffer and input subsystem, completed by Integration Agent 4.

## Components

### 1. Framebuffer Integration (`fb.c` / `fb.h`)

**Physical Address:** `0x40000000`  
**Resolution:** 1024x768x32 (RGBA)  
**Pitch:** 4096 bytes per scanline

#### Implementation Modes

The framebuffer driver supports three modes:

1. **Direct Physical Access** (bare metal, default)
   - Maps framebuffer at physical address `0x40000000`
   - No device driver needed
   - Used for initial bare-metal testing

2. **Linux /dev/fb0** (hosted testing)
   - Uses standard Linux framebuffer device
   - Supports ioctl queries for resolution
   - mmap() for memory mapping

3. **Simulated** (fallback)
   - Allocates memory buffer for testing
   - Used when no hardware is available

#### Key Functions

```c
framebuffer_t *fb_init(void);              // Initialize and map framebuffer
void fb_cleanup(framebuffer_t *fb);        // Unmap and cleanup
void fb_clear(framebuffer_t *fb, uint32_t color);
void fb_fill_rect(framebuffer_t *fb, ...);
void fb_blit(framebuffer_t *fb, ...);
```

### 2. Input Integration (`input.c` / `input.h`)

**Device:** `/dev/input/event0`  
**Protocol:** Linux input event protocol

#### Event Structure

```c
typedef struct {
    uint64_t timestamp;     // Microseconds since boot
    uint16_t type;          // Event type (KEY, REL, ABS)
    uint16_t code;          // Event code
    int32_t value;          // Event value
} compositor_input_event_t;
```

#### Supported Events

- **Mouse Movement:** `INPUT_EVENT_REL` with `REL_X` / `REL_Y`
- **Mouse Buttons:** `BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE`
- **Keyboard:** All key codes from `kernel/include/input.h`

#### Key Functions

```c
input_handler_t *input_init(void);         // Open /dev/input/event0
int input_poll(input_handler_t *input);    // Non-blocking event poll
void input_get_mouse_pos(...);             // Get current mouse position
uint8_t input_get_mouse_buttons(...);      // Get button state
```

#### Mouse Position Tracking

- Tracks relative movement and accumulates position
- Clamps to screen bounds (0-1023, 0-767)
- Updates button state on press/release

### 3. Renderer Integration (`render.c` / `render.h`)

The renderer ties everything together:

- **Double Buffering:** Prevents tearing
- **Painter's Algorithm:** Back-to-front compositing by z-order
- **Cursor Rendering:** 16x16 arrow cursor with outline
- **Window Management:** Up to 64 windows

#### Main Loop Flow

```
1. Poll input events          → input_poll()
2. Update cursor position     → renderer_set_cursor_position()
3. Clear back buffer          → renderer_clear()
4. Composite all windows      → renderer_composite_windows()
5. Draw cursor                → renderer_draw_cursor()
6. Present to framebuffer     → renderer_present()
```

### 4. Main Compositor (`main.c`)

Entry point that orchestrates all components:

- Initializes framebuffer, renderer, input
- Runs 60 FPS render loop
- Handles graceful shutdown on SIGINT/SIGTERM
- Reports FPS statistics every second

## Building

### Hosted Build (Linux/WSL)

```bash
cd userspace/compositor
make hosted
```

Outputs:
- `../../build/userspace/compositor/compositor-hosted`
- `../../build/userspace/compositor/test-integration`

### Freestanding Build (Bare Metal)

```bash
cd userspace/compositor
make freestanding
```

Outputs:
- `../../build/userspace/compositor/compositor`

## Testing

### Integration Test

```bash
../../build/userspace/compositor/test-integration
```

Tests:
- ✓ Framebuffer initialization
- ✓ Renderer initialization
- ✓ Input initialization
- ✓ Window creation
- ✓ 60 FPS compositing loop
- ✓ Cursor rendering
- ✓ Input event handling
- ✓ Test pattern drawing

Expected output:
```
=== Compositor Integration Test ===
[Test] Initializing framebuffer...
[FB] Direct framebuffer initialized: 1024x768 at 0x40000000
[Test] PASS: Framebuffer initialized (1024x768)
[Test] Initializing renderer...
[Renderer] Initialized with 1024x768 back buffer
[Test] PASS: Renderer initialized
[Test] Initializing input...
[Input] Input handler initialized on /dev/input/event0
[Test] PASS: Input initialized
[Test] Creating test windows...
[Test] PASS: Created 3 test windows
[Test] Starting render loop (60 FPS target, 10 seconds)...
[Test] FPS: 60.0 (16.67 ms/frame)
...
```

## IPC Integration (Future)

The IPC system (`ipc.c` / `ipc.h`) is currently in stub mode. Future integration with Agent 1's IPC subsystem will enable:

- Applications to create windows via message queue
- Shared memory for window surfaces
- Window management commands (move, resize, focus, etc.)

### IPC Message Types

```c
MSG_CREATE_WINDOW      // Create a new window
MSG_DESTROY_WINDOW     // Destroy window
MSG_MAP_WINDOW         // Make window visible
MSG_UNMAP_WINDOW       // Hide window
MSG_UPDATE_SURFACE     // Window surface has changed
MSG_SET_TITLE          // Change window title
MSG_MOVE_WINDOW        // Move window
MSG_RESIZE_WINDOW      // Resize window
MSG_FOCUS_WINDOW       // Focus window
MSG_RAISE_WINDOW       // Raise to top
MSG_LOWER_WINDOW       // Lower to bottom
```

## Performance Targets

- **Frame Rate:** 60 FPS (16.67 ms/frame)
- **Latency:** < 1 frame input-to-photon
- **Memory:** Double buffer = 1024 × 768 × 4 × 2 = 6 MB

## Known Limitations

1. **No GPU acceleration** - Pure software rendering
2. **No DMA** - Uses memcpy for blitting
3. **No VSYNC** - Software timing via usleep()
4. **Fixed resolution** - Hardcoded to 1024x768
5. **IPC stub mode** - No real application integration yet

## Next Steps (Agent 1 Integration)

1. Replace IPC stub with real System V message queues
2. Implement shared memory window surfaces
3. Create compositor daemon (compositord)
4. Build client library for applications
5. Add window manager logic (focus, stacking, etc.)

## File Structure

```
userspace/compositor/
├── fb.c / fb.h              - Framebuffer access
├── input.c / input.h        - Input event handling
├── render.c / render.h      - Renderer and compositor
├── ipc.c / ipc.h            - IPC protocol (stub)
├── main.c                   - Main compositor loop
├── test_integration.c       - Integration test
├── Makefile                 - Build system
└── INTEGRATION.md           - This document
```

## Verification Checklist

- [x] Direct framebuffer access at 0x40000000
- [x] Input reading from /dev/input/event0
- [x] 60 FPS compositing loop
- [x] Cursor rendering
- [x] Window management
- [x] Double buffering
- [x] Integration test
- [ ] IPC with Agent 1's subsystem
- [ ] Real application support

## Contact

Integration Agent 4  
Timeline: 1 day (COMPLETED)  
Priority: CRITICAL
