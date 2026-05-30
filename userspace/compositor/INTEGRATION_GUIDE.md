# Framebuffer Compositor Integration Guide

**For Agents 1, 4, 6, 8, 9, 10, 11**

This guide explains how to integrate your components with the framebuffer compositor.

---

## Quick Start

### Building the Compositor

```bash
cd userspace/compositor
make -f Makefile.fb
./build/userspace/compositor/fb_compositor
```

### Expected Output
```
[FB Compositor] Initialized successfully
[Compositor] FPS: 60
```

---

## Agent 1: IPC Architect

### What You Need to Implement

Replace the IPC stubs in `ipc.c`:

#### 1. Message Queue Initialization
```c
int ipc_init_compositor(void) {
    // TODO: Replace with your message queue API
    // Current: System V msgget()
    // Replace: Your kernel IPC implementation
    
    // Example:
    // msg_queue_id = your_msgqueue_create("compositor", 0666);
    // return (msg_queue_id >= 0) ? 0 : -1;
}
```

#### 2. Receive Message
```c
int ipc_receive_message(ipc_message_t *msg) {
    // TODO: Replace with your receive API
    // Current: System V msgrcv()
    // Replace: Your kernel message receive
    
    // Example:
    // return your_msgqueue_recv(msg_queue_id, msg, sizeof(*msg), MSG_NOWAIT);
}
```

#### 3. Shared Memory Attachment
```c
static void *shm_attach(uint32_t shm_id) {
    // TODO: Replace with your shared memory API
    // Current: System V shmat()
    // Replace: Your kernel SHM implementation
    
    // Example:
    // return your_shm_attach(shm_id, SHM_READONLY);
}
```

### Message Format

Applications send messages like this:
```c
ipc_message_t msg;
msg.client_id = getpid();
msg.type = MSG_CREATE_WINDOW;
msg.window_id = 123;

create_window_request_t *req = (create_window_request_t *)msg.payload;
req->type = WINDOW_NORMAL;
req->x = 100;
req->y = 100;
req->width = 640;
req->height = 480;
strcpy(req->title, "My Window");

// Send to compositor
your_msgsnd(compositor_queue, &msg, sizeof(msg), 0);
```

### Integration Points

| Function | File | Action |
|----------|------|--------|
| `ipc_init_compositor()` | `ipc.c:42` | Replace msgget() |
| `ipc_receive_message()` | `ipc.c:73` | Replace msgrcv() |
| `shm_attach()` | `ipc.c:102` | Replace shmat() |

---

## Agent 4: Input Pipeline Developer

### What You Need to Implement

Provide input events from `/dev/input/event0` to the compositor.

#### Integration Point

File: `compositor_main.c`

```c
static void process_input_events(fb_compositor_t *comp) {
    // TODO: Read from /dev/input/event0
    // Current: Stub implementation
    
    input_event_t event;
    while (read_input_event(&event) == 0) {
        switch (event.type) {
            case INPUT_EVENT_MOUSE_MOVE:
                fb_compositor_set_cursor_position(comp, event.x, event.y);
                break;
                
            case INPUT_EVENT_MOUSE_BUTTON:
                // TODO: Send to focused window
                break;
                
            case INPUT_EVENT_KEY:
                // TODO: Send to focused window
                break;
        }
    }
}
```

### Expected Input Event Format

```c
typedef struct {
    uint64_t timestamp;
    uint16_t type;      // INPUT_EVENT_KEY, INPUT_EVENT_MOUSE_MOVE, etc.
    uint16_t code;      // Key code or button number
    int32_t value;      // State (pressed/released) or position
} input_event_t;
```

### Mouse Movement

```c
// Example: Mouse moved to (320, 240)
input_event_t event;
event.type = INPUT_EVENT_MOUSE_MOVE;
event.value = 320 | (240 << 16);  // Pack X and Y

int32_t x = event.value & 0xFFFF;
int32_t y = (event.value >> 16) & 0xFFFF;
fb_compositor_set_cursor_position(comp, x, y);
```

### Window Focus

```c
// Find window under cursor
for (int i = comp->window_count - 1; i >= 0; i--) {
    window_t *win = comp->windows[i];
    if (rect_contains_point(&win->geometry, cursor_x, cursor_y)) {
        ipc_message_t msg;
        msg.type = MSG_FOCUS_WINDOW;
        msg.window_id = win->id;
        ipc_dispatch_message(comp, &msg);
        break;
    }
}
```

---

## Agent 6: Font Rendering Engineer

### What You Need to Implement

Render window titles in the title bar.

#### Integration Point

File: `composition.c:45`

```c
static void draw_decorations(fb_compositor_t *comp, window_t *window) {
    // ... existing title bar drawing code ...
    
    // TODO: Draw title text (requires font rendering from Agent 6)
    // Replace this with your font rendering API:
    
    // Example integration:
    // font_t *font = font_load("DejaVuSans.ttf", 12);
    // int text_x = window->geometry.x + 8;
    // int text_y = window->geometry.y - TITLEBAR_HEIGHT + 6;
    // font_render_text(comp->back_buffer, comp->fb->width, comp->fb->height,
    //                  font, window->title, text_x, text_y, 
    //                  COLOR_TITLEBAR_TEXT);
}
```

### Font Rendering API Suggestion

```c
// Suggested API for Agent 6

// Load font
font_t *font_load(const char *path, int size);

// Render text to buffer
void font_render_text(uint32_t *buffer, uint32_t buffer_width, uint32_t buffer_height,
                     font_t *font, const char *text, 
                     int x, int y, uint32_t color);

// Measure text width (for centering)
int font_measure_text(font_t *font, const char *text);
```

### Example Usage

```c
// Center title in title bar
font_t *font = font_load("DejaVuSans.ttf", 12);
int text_width = font_measure_text(font, window->title);
int text_x = window->geometry.x + (window->geometry.width - text_width) / 2;
int text_y = window->geometry.y - TITLEBAR_HEIGHT + 8;

font_render_text(comp->back_buffer, comp->fb->width, comp->fb->height,
                font, window->title, text_x, text_y, 0xFFECF0F1);
```

---

## Agent 8: Window Manager Integrator

### What You Need to Implement

Connect your window manager to the compositor via IPC.

#### Creating Windows

```c
// In window_manager.c

void wm_create_window(const char *title, int x, int y, int width, int height) {
    // Allocate window ID
    uint32_t window_id = next_window_id++;
    
    // Create window in compositor
    ipc_message_t msg;
    msg.client_id = getpid();
    msg.type = MSG_CREATE_WINDOW;
    msg.window_id = window_id;
    
    create_window_request_t *req = (create_window_request_t *)msg.payload;
    req->type = WINDOW_NORMAL;
    req->x = x;
    req->y = y;
    req->width = width;
    req->height = height;
    strncpy(req->title, title, sizeof(req->title));
    
    // Send to compositor
    msgsnd(compositor_queue, &msg, sizeof(msg), 0);
    
    // Map window (make visible)
    msg.type = MSG_MAP_WINDOW;
    msg.window_id = window_id;
    msgsnd(compositor_queue, &msg, sizeof(msg), 0);
}
```

#### Updating Window Surface

```c
void wm_update_window(uint32_t window_id, uint32_t *pixels, int width, int height) {
    // Create shared memory for pixels
    int shm_id = shmget(IPC_PRIVATE, width * height * 4, 0666 | IPC_CREAT);
    uint32_t *shm_pixels = shmat(shm_id, NULL, 0);
    
    // Copy pixels to shared memory
    memcpy(shm_pixels, pixels, width * height * 4);
    
    // Notify compositor
    ipc_message_t msg;
    msg.client_id = getpid();
    msg.type = MSG_UPDATE_SURFACE;
    msg.window_id = window_id;
    
    update_surface_request_t *req = (update_surface_request_t *)msg.payload;
    req->shm_id = shm_id;
    req->width = width;
    req->height = height;
    req->offset = 0;
    
    msgsnd(compositor_queue, &msg, sizeof(msg), 0);
    
    // Detach (compositor now owns it)
    shmdt(shm_pixels);
}
```

#### Moving Windows

```c
void wm_move_window(uint32_t window_id, int x, int y) {
    ipc_message_t msg;
    msg.client_id = getpid();
    msg.type = MSG_MOVE_WINDOW;
    msg.window_id = window_id;
    
    move_window_request_t *req = (move_window_request_t *)msg.payload;
    req->x = x;
    req->y = y;
    
    msgsnd(compositor_queue, &msg, sizeof(msg), 0);
}
```

---

## Agent 9: Terminal Emulator Developer

### How to Create Your Window

```c
// In terminal.c

int main(void) {
    // Connect to compositor
    int compositor_queue = msgget(ftok("/tmp", 'C'), 0666);
    
    // Create terminal window
    ipc_message_t msg;
    msg.client_id = getpid();
    msg.type = MSG_CREATE_WINDOW;
    msg.window_id = 1000;  // Terminal window ID
    
    create_window_request_t *req = (create_window_request_t *)msg.payload;
    req->type = WINDOW_NORMAL;
    req->x = 100;
    req->y = 100;
    req->width = 800;
    req->height = 600;
    strcpy(req->title, "Terminal");
    
    msgsnd(compositor_queue, &msg, sizeof(msg), 0);
    
    // Map window
    msg.type = MSG_MAP_WINDOW;
    msgsnd(compositor_queue, &msg, sizeof(msg), 0);
    
    // Create shared memory for terminal surface
    int shm_id = shmget(IPC_PRIVATE, 800 * 600 * 4, 0666 | IPC_CREAT);
    uint32_t *pixels = shmat(shm_id, NULL, 0);
    
    // Main loop
    while (running) {
        // Render terminal to pixels buffer
        render_terminal(pixels, 800, 600);
        
        // Update compositor
        msg.type = MSG_UPDATE_SURFACE;
        msg.window_id = 1000;
        
        update_surface_request_t *upd = (update_surface_request_t *)msg.payload;
        upd->shm_id = shm_id;
        upd->width = 800;
        upd->height = 600;
        upd->offset = 0;
        
        msgsnd(compositor_queue, &msg, sizeof(msg), 0);
        
        usleep(16666);  // 60 FPS
    }
    
    return 0;
}
```

---

## Agent 10: File Manager Developer

### Example: Creating File Manager Window

```c
// In file_manager.c

void create_file_manager_window(void) {
    int compositor_queue = msgget(ftok("/tmp", 'C'), 0666);
    
    // Create window
    ipc_message_t msg;
    msg.client_id = getpid();
    msg.type = MSG_CREATE_WINDOW;
    msg.window_id = 2000;
    
    create_window_request_t *req = (create_window_request_t *)msg.payload;
    req->type = WINDOW_NORMAL;
    req->x = 200;
    req->y = 150;
    req->width = 640;
    req->height = 480;
    strcpy(req->title, "File Manager");
    
    msgsnd(compositor_queue, &msg, sizeof(msg), 0);
    msg.type = MSG_MAP_WINDOW;
    msgsnd(compositor_queue, &msg, sizeof(msg), 0);
}
```

---

## Agent 11: Desktop Shell Integrator

### Desktop Background Window

```c
// Create desktop background (full screen)
ipc_message_t msg;
msg.client_id = getpid();
msg.type = MSG_CREATE_WINDOW;
msg.window_id = 10;
msg.window_type = WINDOW_DESKTOP;

create_window_request_t *req = (create_window_request_t *)msg.payload;
req->type = WINDOW_DESKTOP;
req->x = 0;
req->y = 0;
req->width = 1024;  // Full screen
req->height = 768;
strcpy(req->title, "Desktop");

msgsnd(compositor_queue, &msg, sizeof(msg), 0);

// Load wallpaper (Agent 7 integration)
uint32_t *wallpaper = load_png("wallpaper.png");

// Update surface
msg.type = MSG_UPDATE_SURFACE;
update_surface_request_t *upd = (update_surface_request_t *)msg.payload;
upd->shm_id = create_shm_from_pixels(wallpaper, 1024, 768);
upd->width = 1024;
upd->height = 768;
msgsnd(compositor_queue, &msg, sizeof(msg), 0);
```

---

## Debugging

### Enable Debug Output

Edit `fb_compositor.c` and add:
```c
#define DEBUG 1

#ifdef DEBUG
    printf("[DEBUG] Compositing %d windows\n", comp->window_count);
#endif
```

### Check FPS

```bash
./fb_compositor | grep FPS
```

Expected: `[FB Compositor] FPS: 30-60`

### Verify Window Count

```bash
./fb_compositor | grep "Added window"
```

Expected: `[FB Compositor] Added window 1 (1024x768 at 0,0)`

---

## Common Issues

### Issue: Black Screen
**Cause**: No windows mapped  
**Fix**: Ensure `window->mapped = true` before adding to compositor

### Issue: Low FPS (< 20)
**Cause**: Too many damage regions or large windows  
**Fix**: Reduce window count or enable damage tracking

### Issue: Windows Not Appearing
**Cause**: Z-order or clipping issue  
**Fix**: Check `window->z_order` and ensure window is on-screen

### Issue: IPC Not Working
**Cause**: Agent 1's IPC not integrated yet  
**Fix**: Compositor runs in stub mode - messages are ignored but no crash

---

## Summary

| Agent | Integration Point | File | Function |
|-------|------------------|------|----------|
| Agent 1 | IPC | `ipc.c` | Replace stubs |
| Agent 4 | Input | `compositor_main.c` | `process_input_events()` |
| Agent 6 | Fonts | `composition.c` | `draw_decorations()` |
| Agent 8 | WM | External | Call IPC API |
| Agent 9 | Terminal | External | Create window via IPC |
| Agent 10 | File Manager | External | Create window via IPC |
| Agent 11 | Desktop Shell | External | Create desktop window |

---

**Questions?**  
Refer to `FB_COMPOSITOR_README.md` for detailed API documentation.

**Agent 5: Framebuffer Compositor Engineer**  
Ready for your integration!
