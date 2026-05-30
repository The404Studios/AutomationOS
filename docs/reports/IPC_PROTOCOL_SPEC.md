# IPC Protocol Specification
**Integration Agent 9: IPC Complete Integration**  
**Date:** 2026-05-27  
**Status:** 🚀 SPECIFICATION COMPLETE

---

## Executive Summary

This document defines the **IPC protocol** for all inter-component communication in AutomationOS desktop environment. It specifies message formats, shared memory layouts, key assignment, and communication patterns between:

1. **Compositor ↔ Applications** (window surfaces, rendering)
2. **Window Manager ↔ Compositor** (geometry, focus, events)
3. **Desktop Shell ↔ Applications** (app launch, notifications)
4. **Notification Daemon ↔ Desktop Shell** (notification delivery)

**Foundation:** System V IPC (shared memory + message queues) implemented by Agent 1.

---

## 1. IPC Key Assignment Scheme

All IPC objects use agreed-upon keys to enable discovery. Keys are generated using `ftok()` or fixed constants.

### 1.1 Key Ranges

| Component | Key Range | Format | Example |
|-----------|-----------|--------|---------|
| Compositor | `0x1000-0x1FFF` | Fixed | `0x1000` = main message queue |
| Window Manager | `0x2000-0x2FFF` | Fixed | `0x2000` = WM message queue |
| Desktop Shell | `0x3000-0x3FFF` | Fixed | `0x3000` = shell message queue |
| Notification Daemon | `0x4000-0x4FFF` | Fixed | `0x4000` = notif queue |
| Applications | `0x5000-0xFFFF` | Dynamic | `ftok("/app/terminal", 'W')` |
| Window Surfaces | `0x10000+` | Window ID | `0x10000 + window_id` |

### 1.2 Well-Known Keys

```c
// Well-known IPC keys (userspace/include/ipc_keys.h)
#define IPC_KEY_COMPOSITOR        0x1000    // Compositor command queue
#define IPC_KEY_COMPOSITOR_EVENTS 0x1001    // Compositor event queue
#define IPC_KEY_WM                0x2000    // Window manager queue
#define IPC_KEY_WM_EVENTS         0x2001    // WM event queue
#define IPC_KEY_DESKTOP_SHELL     0x3000    // Desktop shell queue
#define IPC_KEY_NOTIFICATION      0x4000    // Notification daemon queue
#define IPC_KEY_INPUT             0x5000    // Input event queue

// Window surface keys (shared memory)
#define IPC_KEY_WINDOW_SURFACE(wid)  (0x10000 + (wid))
```

---

## 2. Message Queue Protocol

### 2.1 Message Structure

All message queues use a standard header + payload format:

```c
// Standard IPC message header
typedef struct {
    long mtype;              // Message type (for filtering)
    uint32_t sender_id;      // Sender process ID or component ID
    uint32_t sequence;       // Sequence number (for ordering)
    uint32_t payload_size;   // Size of payload in bytes
} ipc_msg_header_t;

// Full message structure
typedef struct {
    ipc_msg_header_t header;
    uint8_t payload[IPC_MAX_PAYLOAD];  // 256 bytes max
} ipc_message_t;

#define IPC_MAX_PAYLOAD 256
```

### 2.2 Message Types

Message types are used for filtering via `msgrcv()`. Convention:

- **1-99:** System-level messages (compositor, WM)
- **100-199:** Application messages (create window, etc.)
- **200-299:** Event messages (keyboard, mouse)
- **300-399:** Notification messages
- **400+:** Custom application protocols

```c
// Message type enumeration
typedef enum {
    // Compositor messages (1-99)
    MSG_COMPOSITOR_CREATE_WINDOW = 1,
    MSG_COMPOSITOR_DESTROY_WINDOW = 2,
    MSG_COMPOSITOR_MAP_WINDOW = 3,
    MSG_COMPOSITOR_UNMAP_WINDOW = 4,
    MSG_COMPOSITOR_UPDATE_SURFACE = 5,
    MSG_COMPOSITOR_SET_TITLE = 6,
    MSG_COMPOSITOR_MOVE_WINDOW = 7,
    MSG_COMPOSITOR_RESIZE_WINDOW = 8,
    MSG_COMPOSITOR_RAISE_WINDOW = 9,
    MSG_COMPOSITOR_LOWER_WINDOW = 10,
    
    // Window manager messages (11-50)
    MSG_WM_CONFIGURE = 11,
    MSG_WM_FOCUS_CHANGED = 12,
    MSG_WM_GEOMETRY_UPDATE = 13,
    MSG_WM_WORKSPACE_CHANGED = 14,
    MSG_WM_MINIMIZE = 15,
    MSG_WM_MAXIMIZE = 16,
    MSG_WM_FULLSCREEN = 17,
    MSG_WM_CLOSE = 18,
    
    // Application messages (100-199)
    MSG_APP_LAUNCH = 100,
    MSG_APP_EXIT = 101,
    MSG_APP_READY = 102,
    MSG_APP_REGISTER = 103,
    
    // Input events (200-299)
    MSG_EVENT_KEYBOARD = 200,
    MSG_EVENT_MOUSE_MOTION = 201,
    MSG_EVENT_MOUSE_BUTTON = 202,
    MSG_EVENT_SCROLL = 203,
    MSG_EVENT_TOUCH = 204,
    
    // Notifications (300-399)
    MSG_NOTIF_SEND = 300,
    MSG_NOTIF_UPDATE = 301,
    MSG_NOTIF_CLOSE = 302,
    MSG_NOTIF_ACTION = 303,
    
} ipc_msg_type_t;
```

---

## 3. Compositor ↔ Application Protocol

### 3.1 Connection Flow

```
┌────────────┐                           ┌────────────┐
│Application │                           │ Compositor │
└────┬───────┘                           └────┬───────┘
     │                                        │
     │  1. msgget(IPC_KEY_COMPOSITOR)         │
     │────────────────────────────────────────>│
     │                                        │
     │  2. MSG_APP_REGISTER                   │
     │  (app_id, app_name, version)           │
     │───────────────────────────────────────>│
     │                                        │
     │  3. Response: client_id                │
     │<───────────────────────────────────────│
     │                                        │
     │  4. Create app-specific queue          │
     │  msgget(ftok("/app/name", 'Q'))        │
     │                                        │
     │  5. MSG_COMPOSITOR_CREATE_WINDOW       │
     │  (type, x, y, width, height, title)    │
     │───────────────────────────────────────>│
     │                                        │
     │  6. Response: window_id, shm_id        │
     │<───────────────────────────────────────│
     │                                        │
     │  7. shmat(shm_id, NULL, 0)             │
     │  (attach to window surface)            │
     │                                        │
     │  8. Draw to shared memory              │
     │  pixels[x + y*width] = color           │
     │                                        │
     │  9. MSG_COMPOSITOR_UPDATE_SURFACE      │
     │  (window_id, dirty_rect)               │
     │───────────────────────────────────────>│
     │                                        │
     │  10. Compositor blits to framebuffer   │
     │                                        │
```

### 3.2 Shared Memory Layout

Each window has a dedicated shared memory segment for its pixel buffer:

```c
// Window surface shared memory layout
typedef struct {
    uint32_t width;              // Surface width in pixels
    uint32_t height;             // Surface height in pixels
    uint32_t format;             // Pixel format (0=RGBA8888)
    uint32_t stride;             // Bytes per row
    uint32_t version;            // Incremented on resize
    uint32_t damage_count;       // Number of damaged regions
    rect_t damage_rects[8];      // Damaged regions (for optimization)
    uint32_t pixels[];           // Pixel data (RGBA8888)
} window_surface_t;
```

**Size calculation:**
```c
size_t surface_size = sizeof(window_surface_t) + (width * height * 4);
int shm_id = shmget(IPC_KEY_WINDOW_SURFACE(window_id), surface_size, IPC_CREAT | 0666);
```

### 3.3 Message Payloads

#### Create Window Request
```c
typedef struct {
    uint32_t window_type;        // 0=normal, 1=dialog, 2=utility
    int32_t x, y;                // Desired position (-1 = WM decides)
    uint32_t width, height;      // Desired size
    char title[128];             // Window title
    uint32_t parent_id;          // Parent window (0 = top-level)
} create_window_request_t;
```

**Response:**
```c
typedef struct {
    uint32_t window_id;          // Assigned window ID
    int32_t shm_id;              // Shared memory ID for surface
    int32_t actual_x, actual_y;  // Actual position (WM may adjust)
} create_window_response_t;
```

#### Update Surface Request
```c
typedef struct {
    uint32_t window_id;
    rect_t dirty_rect;           // Region that changed (0,0,0,0 = full)
} update_surface_request_t;
```

#### Move/Resize Window
```c
typedef struct {
    uint32_t window_id;
    int32_t x, y;
    uint32_t width, height;
} configure_window_request_t;
```

---

## 4. Window Manager ↔ Compositor Protocol

### 4.1 Communication Pattern

```
┌────────────┐                           ┌────────────┐
│Window Mgr  │                           │ Compositor │
└────┬───────┘                           └────┬───────┘
     │                                        │
     │  MSG_WM_GEOMETRY_UPDATE                │
     │  (window_id, new_x, new_y, w, h)       │
     │───────────────────────────────────────>│
     │                                        │
     │  MSG_WM_FOCUS_CHANGED                  │
     │  (old_window_id, new_window_id)        │
     │───────────────────────────────────────>│
     │                                        │
     │  MSG_EVENT_MOUSE_BUTTON                │
     │  (window_id, x, y, button, pressed)    │
     │<───────────────────────────────────────│
     │                                        │
     │  (WM handles focus, raise, etc.)       │
     │                                        │
     │  MSG_WM_RAISE_WINDOW                   │
     │  (window_id)                           │
     │───────────────────────────────────────>│
     │                                        │
```

### 4.2 Message Payloads

#### Geometry Update
```c
typedef struct {
    uint32_t window_id;
    int32_t x, y;
    uint32_t width, height;
    uint32_t flags;              // 0x1=animated, 0x2=user-initiated
} wm_geometry_update_t;
```

#### Focus Change Notification
```c
typedef struct {
    uint32_t old_window_id;      // Previously focused window (0=none)
    uint32_t new_window_id;      // Newly focused window
    uint32_t reason;             // 0=user click, 1=alt-tab, 2=programmatic
} wm_focus_changed_t;
```

#### Workspace Change
```c
typedef struct {
    uint32_t old_workspace;
    uint32_t new_workspace;
    uint32_t animation_ms;       // Animation duration
} wm_workspace_changed_t;
```

---

## 5. Desktop Shell ↔ Application Protocol

### 5.1 Application Launch

```
┌────────────┐                           ┌────────────┐
│Desktop     │                           │Application │
│Shell       │                           │            │
└────┬───────┘                           └────┬───────┘
     │                                        │
     │  MSG_APP_LAUNCH                        │
     │  (app_path, args[], env[])             │
     │───────────────────────────────────────>│
     │                                        │
     │  (fork + exec)                         │
     │                                        │
     │  MSG_APP_READY                         │
     │  (app_id, capabilities)                │
     │<───────────────────────────────────────│
     │                                        │
     │  (Update dock icon)                    │
     │                                        │
```

### 5.2 Message Payloads

#### Launch Application
```c
typedef struct {
    char app_path[256];          // Path to executable
    char args[512];              // Command-line arguments
    char working_dir[256];       // Working directory
    uint32_t flags;              // 0x1=detached, 0x2=wait
} app_launch_request_t;
```

#### Application Ready Notification
```c
typedef struct {
    uint32_t app_id;             // Process ID
    char app_name[64];           // Human-readable name
    uint32_t capabilities;       // 0x1=notifications, 0x2=tray icon
    uint32_t primary_window_id;  // Main window ID
} app_ready_notification_t;
```

---

## 6. Notification Daemon ↔ Desktop Shell Protocol

### 6.1 Notification Flow

```
┌────────────┐                           ┌────────────┐
│Application │                           │Notification│
│            │                           │Daemon      │
└────┬───────┘                           └────┬───────┘
     │                                        │
     │  MSG_NOTIF_SEND                        │
     │  (app_name, summary, body, urgency)    │
     │───────────────────────────────────────>│
     │                                        │
     │  (Daemon processes, assigns ID)        │
     │                                        │
     │  Response: notification_id             │
     │<───────────────────────────────────────│
     │                                        │
     │                                   ┌────┴───────┐
     │                                   │Desktop     │
     │                                   │Shell       │
     │                                   └────┬───────┘
     │                                        │
     │  MSG_NOTIF_SEND (forwarded)            │
     │  (notification_id, data...)            │
     │<───────────────────────────────────────│
     │                                        │
     │  (Shell displays notification)         │
     │                                        │
     │  MSG_NOTIF_ACTION                      │
     │  (notification_id, action_id)          │
     │───────────────────────────────────────>│
     │                                        │
     │  (Forward to original app)             │
     │                                        │
```

### 6.2 Message Payloads

#### Send Notification
```c
typedef struct {
    char app_name[64];           // Sender application
    char summary[128];           // Notification title
    char body[256];              // Notification body
    uint32_t urgency;            // 0=low, 1=normal, 2=critical
    uint32_t timeout_ms;         // Auto-dismiss timeout (0=manual)
    uint32_t icon_id;            // Icon resource ID (0=default)
    uint32_t action_count;       // Number of actions
    struct {
        char label[32];
        uint32_t action_id;
    } actions[4];
} notif_send_request_t;
```

#### Notification Action
```c
typedef struct {
    uint32_t notification_id;
    uint32_t action_id;          // Which action was clicked
    uint32_t timestamp;          // When action occurred
} notif_action_t;
```

---

## 7. Input Event Distribution

### 7.1 Event Flow

```
┌──────────┐     ┌──────────┐     ┌──────────┐     ┌──────────┐
│ Input    │────>│ Window   │────>│Compositor│────>│Application│
│ Driver   │     │ Manager  │     │          │     │           │
└──────────┘     └──────────┘     └──────────┘     └──────────┘
  (Kernel)         (Userspace)      (Userspace)     (Userspace)
                   
   Raw events    Focus routing    Event transform   Event handling
   (scancodes)   (which window)   (window coords)   (app logic)
```

### 7.2 Event Message Payloads

#### Keyboard Event
```c
typedef struct {
    uint32_t window_id;          // Target window (0=global)
    uint16_t keycode;            // Physical keycode
    uint16_t keysym;             // Logical key symbol
    uint32_t modifiers;          // Shift, Ctrl, Alt, Super
    uint8_t pressed;             // 1=press, 0=release
    uint8_t repeat;              // 1=auto-repeat event
    uint32_t timestamp;          // Event timestamp
} keyboard_event_t;
```

#### Mouse Motion Event
```c
typedef struct {
    uint32_t window_id;          // Window under cursor (0=none)
    int32_t x, y;                // Window-relative coordinates
    int32_t screen_x, screen_y;  // Screen-absolute coordinates
    uint32_t modifiers;          // Active modifier keys
    uint32_t timestamp;
} mouse_motion_event_t;
```

#### Mouse Button Event
```c
typedef struct {
    uint32_t window_id;
    int32_t x, y;
    uint8_t button;              // 1=left, 2=middle, 3=right
    uint8_t pressed;             // 1=press, 0=release
    uint8_t click_count;         // 1=single, 2=double, 3=triple
    uint32_t modifiers;
    uint32_t timestamp;
} mouse_button_event_t;
```

---

## 8. Error Handling

### 8.1 Error Response Messages

All requests should send a response with result code:

```c
typedef struct {
    uint32_t request_sequence;   // Matches request sequence number
    int32_t result;              // 0=success, <0=error code
    char error_message[128];     // Human-readable error (if result < 0)
} ipc_response_t;
```

### 8.2 Error Codes

```c
#define IPC_SUCCESS             0
#define IPC_ERR_INVALID_WINDOW  -1
#define IPC_ERR_NO_MEMORY       -2
#define IPC_ERR_PERMISSION      -3
#define IPC_ERR_TIMEOUT         -4
#define IPC_ERR_NOT_FOUND       -5
#define IPC_ERR_ALREADY_EXISTS  -6
#define IPC_ERR_PROTOCOL        -7
#define IPC_ERR_DISCONNECTED    -8
```

### 8.3 Timeout and Retry Policy

- **Non-blocking by default:** Use `IPC_NOWAIT` flag
- **Polling interval:** 16ms (60 FPS alignment)
- **Retry count:** 3 attempts for critical messages
- **Timeout:** 1 second for synchronous operations
- **Fallback:** Log error and continue (no hard failures)

---

## 9. Performance Considerations

### 9.1 Message Queue Optimization

**Problem:** Linear scan for message type filtering (O(n))

**Mitigation:**
1. Use specific message types (avoid type=0 "any")
2. Keep queue short (process messages every frame)
3. Batch operations when possible

**Future:** Ring buffer implementation (Phase 2)

### 9.2 Shared Memory Optimization

**Best practices:**
1. **Dirty regions:** Only update changed areas
2. **Page alignment:** Allocate on 4KB boundaries
3. **Damage tracking:** Use `damage_rects[]` array
4. **Attach once:** Don't detach/reattach every frame
5. **Read-only when possible:** Use `SHM_RDONLY` for surfaces

**Future:** Huge pages (2MB) for large surfaces (Phase 3)

### 9.3 Typical Message Rates

| Component Pair | Messages/sec | Bottleneck Risk |
|----------------|--------------|-----------------|
| App → Compositor (render) | 60 (vsync) | Low |
| WM → Compositor (geometry) | <10 | Low |
| Input → WM → App | 120-1000 | Medium |
| App → Notification | <1 | None |

---

## 10. Security Considerations

### 10.1 Permission Checking

All IPC objects use UID/GID-based permissions:

```c
// Example: Create compositor queue (read/write for all users)
int queue_id = msgget(IPC_KEY_COMPOSITOR, IPC_CREAT | 0666);

// Example: Create private app queue (read/write for owner only)
int app_queue = msgget(ftok("/app/name", 'Q'), IPC_CREAT | 0600);
```

### 10.2 Input Validation

**Required checks:**
1. ✅ Window ID exists before operations
2. ✅ Shared memory ID valid before attaching
3. ✅ Message payload size <= `IPC_MAX_PAYLOAD`
4. ✅ String fields null-terminated
5. ✅ Coordinates within screen bounds
6. ✅ Sender process exists (check PID)

### 10.3 Resource Limits

Prevent resource exhaustion:

```c
// Per-application limits
#define MAX_WINDOWS_PER_APP     32
#define MAX_NOTIFICATIONS       100
#define MAX_SHM_SEGMENTS        64
#define MAX_MESSAGE_QUEUE_SIZE  100   // From kernel IPC
```

---

## 11. Implementation Checklist

### Phase 1: Core Integration (Agent 9)

- [ ] Create `userspace/include/ipc_protocol.h` (all message structures)
- [ ] Create `userspace/include/ipc_keys.h` (well-known keys)
- [ ] Implement compositor IPC handlers (all 11 message types)
- [ ] Implement window manager IPC handlers (geometry, focus)
- [ ] Implement desktop shell IPC handlers (launch, ready)
- [ ] Implement notification daemon IPC handlers (send, action)
- [ ] Create shared memory for test window surfaces
- [ ] Test: Compositor ↔ Test App (create window, render)
- [ ] Test: WM ↔ Compositor (geometry update)
- [ ] Test: Shell → App (launch)
- [ ] Test: App → Notification (send notification)
- [ ] Integration test: Full stack (shell → app → compositor → render)

### Phase 2: Error Handling

- [ ] Add timeout handling for all `msgrcv` calls
- [ ] Add retry logic for critical messages
- [ ] Add error response messages
- [ ] Add logging for IPC errors
- [ ] Add graceful degradation (fallback modes)

### Phase 3: Performance

- [ ] Add dirty region tracking
- [ ] Optimize message queue polling (event-driven)
- [ ] Add shared memory damage tracking
- [ ] Profile message rates
- [ ] Add performance counters

### Phase 4: Testing

- [ ] Unit test: Each message handler
- [ ] Integration test: Compositor + WM + App
- [ ] Stress test: 1000 messages/sec
- [ ] Leak test: Create/destroy 1000 windows
- [ ] Concurrency test: Multiple apps simultaneously

---

## 12. Example Usage

### 12.1 Create Window and Render

```c
#include "ipc_protocol.h"
#include "ipc_keys.h"
#include "../libc/ipc.h"

int main() {
    // 1. Connect to compositor
    int compositor_queue = msgget(IPC_KEY_COMPOSITOR, 0);
    
    // 2. Create window
    ipc_message_t msg = {0};
    msg.header.mtype = MSG_COMPOSITOR_CREATE_WINDOW;
    msg.header.sender_id = getpid();
    msg.header.sequence = 1;
    msg.header.payload_size = sizeof(create_window_request_t);
    
    create_window_request_t *req = (create_window_request_t *)msg.payload;
    req->window_type = 0;  // Normal window
    req->x = 100; req->y = 100;
    req->width = 640; req->height = 480;
    strcpy(req->title, "My Application");
    
    msgsnd(compositor_queue, &msg, sizeof(msg), 0);
    
    // 3. Receive response (window_id + shm_id)
    ipc_message_t resp;
    msgrcv(compositor_queue, &resp, sizeof(resp), getpid(), 0);
    create_window_response_t *resp_data = (create_window_response_t *)resp.payload;
    
    // 4. Attach to window surface
    window_surface_t *surface = shmat(resp_data->shm_id, NULL, 0);
    
    // 5. Draw to surface
    for (uint32_t y = 0; y < surface->height; y++) {
        for (uint32_t x = 0; x < surface->width; x++) {
            surface->pixels[y * surface->width + x] = 0xFF0000FF;  // Red
        }
    }
    
    // 6. Notify compositor of update
    msg.header.mtype = MSG_COMPOSITOR_UPDATE_SURFACE;
    msg.header.sequence = 2;
    msg.header.payload_size = sizeof(update_surface_request_t);
    
    update_surface_request_t *update = (update_surface_request_t *)msg.payload;
    update->window_id = resp_data->window_id;
    update->dirty_rect = (rect_t){0, 0, 640, 480};  // Full update
    
    msgsnd(compositor_queue, &msg, sizeof(msg), 0);
    
    // 7. Event loop (receive input events)
    while (1) {
        if (msgrcv(compositor_queue, &msg, sizeof(msg), MSG_EVENT_KEYBOARD, IPC_NOWAIT) > 0) {
            keyboard_event_t *key = (keyboard_event_t *)msg.payload;
            printf("Key %d pressed: %d\n", key->keycode, key->pressed);
        }
        usleep(16000);  // 60 FPS
    }
    
    return 0;
}
```

### 12.2 Send Notification

```c
#include "ipc_protocol.h"

void send_notification(const char *title, const char *body) {
    int notif_queue = msgget(IPC_KEY_NOTIFICATION, 0);
    
    ipc_message_t msg = {0};
    msg.header.mtype = MSG_NOTIF_SEND;
    msg.header.sender_id = getpid();
    msg.header.sequence = 1;
    msg.header.payload_size = sizeof(notif_send_request_t);
    
    notif_send_request_t *notif = (notif_send_request_t *)msg.payload;
    strcpy(notif->app_name, "My App");
    strcpy(notif->summary, title);
    strcpy(notif->body, body);
    notif->urgency = 1;  // Normal
    notif->timeout_ms = 5000;
    
    msgsnd(notif_queue, &msg, sizeof(msg), 0);
}
```

---

## 13. Testing Strategy

### 13.1 Unit Tests

Each component tests its IPC handlers independently:

```c
// Test compositor create window handler
void test_create_window() {
    fb_compositor_t *comp = compositor_create();
    
    ipc_message_t msg = {0};
    msg.header.mtype = MSG_COMPOSITOR_CREATE_WINDOW;
    // ... fill request ...
    
    int result = ipc_handle_create_window(comp, &msg);
    assert(result == 0);
    assert(comp->window_count == 1);
}
```

### 13.2 Integration Tests

Test full message flow:

```c
// Test: App creates window and renders
void test_app_window_render() {
    // 1. Start compositor process
    pid_t comp_pid = fork();
    if (comp_pid == 0) {
        execl("./compositord", "compositord", NULL);
    }
    
    // 2. Start test app
    pid_t app_pid = fork();
    if (app_pid == 0) {
        execl("./test_app", "test_app", NULL);
    }
    
    // 3. Wait for window creation
    sleep(1);
    
    // 4. Verify: Window exists in compositor
    // 5. Verify: Shared memory contains expected pixels
    
    // Cleanup
    kill(comp_pid, SIGTERM);
    kill(app_pid, SIGTERM);
}
```

### 13.3 Stress Tests

```c
// Create and destroy 1000 windows
void stress_test_windows() {
    for (int i = 0; i < 1000; i++) {
        uint32_t window_id = create_window(...);
        render_window(window_id);
        destroy_window(window_id);
    }
    // Verify no memory leaks
}
```

---

## 14. Debugging Tools

### 14.1 IPC Monitor

```c
// Monitor all IPC messages
void ipc_monitor() {
    int queue = msgget(IPC_KEY_COMPOSITOR, 0);
    
    while (1) {
        ipc_message_t msg;
        if (msgrcv(queue, &msg, sizeof(msg), 0, IPC_NOWAIT | MSG_COPY) > 0) {
            printf("[%d] Type=%ld Sender=%u Seq=%u Size=%u\n",
                   time(NULL), msg.header.mtype, msg.header.sender_id,
                   msg.header.sequence, msg.header.payload_size);
        }
        usleep(100000);  // 10 Hz polling
    }
}
```

### 14.2 Shared Memory Dump

```c
// Dump window surface to file
void dump_surface(int shm_id, const char *filename) {
    window_surface_t *surface = shmat(shm_id, NULL, SHM_RDONLY);
    if (!surface) return;
    
    FILE *f = fopen(filename, "wb");
    fwrite(&surface->pixels, 4, surface->width * surface->height, f);
    fclose(f);
    
    shmdt(surface);
}
```

---

## Conclusion

This protocol specification provides a complete, well-defined communication layer for the AutomationOS desktop environment. All components can now communicate reliably using:

- **Shared memory** for high-bandwidth data (window surfaces)
- **Message queues** for low-latency control messages and events
- **Standardized message formats** for interoperability
- **Well-known keys** for service discovery
- **Error handling** for robustness

**Next step:** Implement handlers in each component following this specification.

---

*Protocol Specification v1.0*  
*Agent 9: IPC Complete Integration*  
*AutomationOS Desktop Environment*
