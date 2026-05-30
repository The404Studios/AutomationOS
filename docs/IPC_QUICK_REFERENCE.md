# IPC Quick Reference
**AutomationOS Desktop IPC Protocol**

---

## 5-Minute Quick Start

### Connect to Compositor

```c
#include "../include/ipc_protocol.h"
#include "../include/ipc_keys.h"
#include "../libc/ipc.h"

// 1. Connect to compositor
int compositor_queue = msgget(IPC_KEY_COMPOSITOR, 0);

// 2. Create response queue
int my_pid = getpid();
int response_queue = msgget(ipc_response_key(my_pid), IPC_CREAT | 0666);
```

### Create Window

```c
// 3. Send create window request
ipc_message_t msg;
IPC_MSG_INIT(&msg, MSG_COMPOSITOR_CREATE_WINDOW, my_pid, 1);

create_window_request_t req = {
    .window_type = 0,          // Normal window
    .x = 100, .y = 100,
    .width = 640, .height = 480,
    .parent_id = 0,
};
strcpy(req.title, "My App");

IPC_MSG_SET_PAYLOAD(&msg, &req, sizeof(req));
msgsnd(compositor_queue, &msg, sizeof(ipc_message_t) - sizeof(long), 0);

// 4. Receive response
ipc_message_t response;
msgrcv(response_queue, &response, sizeof(ipc_message_t) - sizeof(long),
       MSG_RESPONSE_SUCCESS, 0);

create_window_response_t *resp = IPC_MSG_GET_PAYLOAD(&response, create_window_response_t);
uint32_t window_id = resp->window_id;
int shm_id = resp->shm_id;
```

### Render to Window

```c
// 5. Attach to window surface
window_surface_t *surface = shmat(shm_id, NULL, 0);

// 6. Draw pixels (RGBA8888 format)
for (uint32_t y = 0; y < surface->height; y++) {
    for (uint32_t x = 0; x < surface->width; x++) {
        surface->pixels[y * surface->width + x] = 0xFF0000FF; // Red
    }
}

// 7. Mark damage
surface->damage_count = 1;
surface->damage_rects[0] = (rect_t){0, 0, surface->width, surface->height};

// 8. Notify compositor
IPC_MSG_INIT(&msg, MSG_COMPOSITOR_UPDATE_SURFACE, my_pid, 2);

update_surface_request_t update_req = {
    .window_id = window_id,
    .dirty_rect = {0, 0, surface->width, surface->height},
};

IPC_MSG_SET_PAYLOAD(&msg, &update_req, sizeof(update_req));
msgsnd(compositor_queue, &msg, sizeof(ipc_message_t) - sizeof(long), 0);
```

### Show Window

```c
// 9. Map window (make visible)
IPC_MSG_INIT(&msg, MSG_COMPOSITOR_MAP_WINDOW, my_pid, 3);

window_operation_t map_req = { .window_id = window_id };
IPC_MSG_SET_PAYLOAD(&msg, &map_req, sizeof(map_req));

msgsnd(compositor_queue, &msg, sizeof(ipc_message_t) - sizeof(long), 0);
```

### Cleanup

```c
// 10. Destroy window
IPC_MSG_INIT(&msg, MSG_COMPOSITOR_DESTROY_WINDOW, my_pid, 4);

window_operation_t destroy_req = { .window_id = window_id };
IPC_MSG_SET_PAYLOAD(&msg, &destroy_req, sizeof(destroy_req));

msgsnd(compositor_queue, &msg, sizeof(ipc_message_t) - sizeof(long), 0);

// 11. Detach and cleanup
shmdt(surface);
msgctl(response_queue, IPC_RMID, NULL);
```

---

## Well-Known Keys Cheat Sheet

| Component | Key | Purpose |
|-----------|-----|---------|
| Compositor | `0x1000` | Main command queue |
| Window Manager | `0x2000` | WM command queue |
| Desktop Shell | `0x3000` | Shell command queue |
| Notifications | `0x4000` | Notification queue |
| Input System | `0x5000` | Input event queue |
| Window Surface | `0x100000 + window_id` | Shared memory |

---

## Common Message Types

### Compositor Messages

| Message Type | Purpose | Payload |
|--------------|---------|---------|
| `MSG_COMPOSITOR_CREATE_WINDOW` | Create new window | `create_window_request_t` |
| `MSG_COMPOSITOR_DESTROY_WINDOW` | Destroy window | `window_operation_t` |
| `MSG_COMPOSITOR_MAP_WINDOW` | Show window | `window_operation_t` |
| `MSG_COMPOSITOR_UNMAP_WINDOW` | Hide window | `window_operation_t` |
| `MSG_COMPOSITOR_UPDATE_SURFACE` | Blit from shared memory | `update_surface_request_t` |
| `MSG_COMPOSITOR_SET_TITLE` | Change title | `set_title_request_t` |
| `MSG_COMPOSITOR_MOVE_WINDOW` | Move window | `move_window_request_t` |
| `MSG_COMPOSITOR_RESIZE_WINDOW` | Resize window | `resize_window_request_t` |
| `MSG_COMPOSITOR_RAISE_WINDOW` | Bring to front | `window_operation_t` |
| `MSG_COMPOSITOR_LOWER_WINDOW` | Send to back | `window_operation_t` |

### Window Manager Messages

| Message Type | Purpose | Payload |
|--------------|---------|---------|
| `MSG_WM_FOCUS_CHANGED` | Focus changed | `wm_focus_changed_t` |
| `MSG_WM_GEOMETRY_UPDATE` | Window moved/resized | `wm_geometry_update_t` |
| `MSG_WM_WORKSPACE_CHANGED` | Workspace switched | `wm_workspace_changed_t` |
| `MSG_WM_MINIMIZE` | Minimize window | `window_operation_t` |
| `MSG_WM_MAXIMIZE` | Maximize window | `window_operation_t` |

### Input Events

| Message Type | Purpose | Payload |
|--------------|---------|---------|
| `MSG_EVENT_KEYBOARD` | Keyboard event | `keyboard_event_t` |
| `MSG_EVENT_MOUSE_MOTION` | Mouse moved | `mouse_motion_event_t` |
| `MSG_EVENT_MOUSE_BUTTON` | Mouse clicked | `mouse_button_event_t` |
| `MSG_EVENT_SCROLL` | Scroll wheel | `scroll_event_t` |

### Notifications

| Message Type | Purpose | Payload |
|--------------|---------|---------|
| `MSG_NOTIF_SEND` | Send notification | `notif_send_request_t` |
| `MSG_NOTIF_ACTION` | User clicked action | `notif_action_response_t` |
| `MSG_NOTIF_CLOSE` | Close notification | `notif_close_request_t` |

---

## Error Codes

| Code | Constant | Meaning |
|------|----------|---------|
| 0 | `IPC_SUCCESS` | Success |
| -1 | `IPC_ERR_INVALID_WINDOW` | Window doesn't exist |
| -2 | `IPC_ERR_NO_MEMORY` | Out of memory |
| -3 | `IPC_ERR_PERMISSION` | Permission denied |
| -4 | `IPC_ERR_TIMEOUT` | Operation timed out |
| -5 | `IPC_ERR_NOT_FOUND` | Resource not found |

---

## Helper Macros

```c
// Initialize message header
IPC_MSG_INIT(&msg, type, sender_id, sequence);

// Set payload data
IPC_MSG_SET_PAYLOAD(&msg, &data, sizeof(data));

// Get payload from message
type *payload = IPC_MSG_GET_PAYLOAD(&msg, type);

// Calculate surface size
size_t size = WINDOW_SURFACE_SIZE(width, height);

// Generate response queue key
int response_key = ipc_response_key(getpid());

// Generate window surface key
int surface_key = IPC_KEY_WINDOW_SURFACE(window_id);
```

---

## Window Surface Structure

```c
typedef struct {
    uint32_t width, height;      // Surface dimensions
    uint32_t format;             // 0 = RGBA8888
    uint32_t stride;             // Bytes per row (width * 4)
    uint32_t version;            // Incremented on resize
    uint32_t damage_count;       // Number of damaged regions
    rect_t damage_rects[8];      // Damaged regions
    uint32_t pixels[];           // Pixel data (RGBA8888)
} window_surface_t;
```

**Pixel format:** RGBA8888 (32-bit)
- Red: bits 0-7
- Green: bits 8-15
- Blue: bits 16-23
- Alpha: bits 24-31

**Example:**
```c
uint32_t red = 0xFF0000FF;    // Opaque red
uint32_t green = 0xFF00FF00;  // Opaque green
uint32_t blue = 0xFFFF0000;   // Opaque blue
uint32_t white = 0xFFFFFFFF;  // Opaque white
uint32_t black = 0xFF000000;  // Opaque black
uint32_t transparent = 0x00000000;  // Fully transparent
```

---

## Common Patterns

### Request/Response

```c
// Send request
msg.header.sender_id = getpid();
msg.header.sequence = seq++;
msgsnd(service_queue, &msg, ...);

// Wait for response
ipc_message_t response;
msgrcv(response_queue, &response, ..., MSG_RESPONSE_SUCCESS, 0);

if (response.header.sequence == msg.header.sequence) {
    // Matching response
}
```

### Damage Tracking (Partial Update)

```c
// Update only a small region (e.g., blinking cursor)
rect_t cursor_rect = {x, y, cursor_width, cursor_height};

surface->damage_count = 1;
surface->damage_rects[0] = cursor_rect;

// Send update with dirty rect
update_surface_request_t update = {
    .window_id = window_id,
    .dirty_rect = cursor_rect,  // Only this region changed
};
```

### Send Notification

```c
int notif_queue = msgget(IPC_KEY_NOTIFICATION, 0);

ipc_message_t msg;
IPC_MSG_INIT(&msg, MSG_NOTIF_SEND, getpid(), 1);

notif_send_request_t notif = {0};
strcpy(notif.app_name, "My App");
strcpy(notif.summary, "File Downloaded");
strcpy(notif.body, "document.pdf is ready");
notif.urgency = 1;  // Normal
notif.timeout_ms = 5000;

IPC_MSG_SET_PAYLOAD(&msg, &notif, sizeof(notif));
msgsnd(notif_queue, &msg, sizeof(ipc_message_t) - sizeof(long), 0);
```

---

## Best Practices

### ✅ DO

- Use `IPC_MSG_INIT()`, `IPC_MSG_SET_PAYLOAD()`, `IPC_MSG_GET_PAYLOAD()` macros
- Check return values from all IPC calls
- Use well-known keys from `ipc_keys.h`
- Detach shared memory with `shmdt()` when done
- Remove response queues with `msgctl(queue_id, IPC_RMID, NULL)`
- Use damage tracking for partial updates
- Attach shared memory once, reuse many times

### ❌ DON'T

- Don't use pointers in shared memory (virtual addresses differ per process)
- Don't assume message order (use sequence numbers)
- Don't block indefinitely (use `IPC_NOWAIT` or timeouts)
- Don't forget to cleanup shared memory and queues
- Don't send large data via messages (use shared memory instead)
- Don't hard-code IPC keys (use `IPC_KEY_*` constants)

---

## Debugging

### Check if service is running

```c
int queue = msgget(IPC_KEY_COMPOSITOR, 0);
if (queue < 0) {
    printf("Compositor not running\n");
}
```

### Dump window surface to file

```c
window_surface_t *surface = shmat(shm_id, NULL, SHM_RDONLY);
FILE *f = fopen("surface.raw", "wb");
fwrite(surface->pixels, 4, surface->width * surface->height, f);
fclose(f);
shmdt(surface);
```

### Monitor IPC messages

```c
while (1) {
    ipc_message_t msg;
    if (msgrcv(queue, &msg, ..., 0, IPC_NOWAIT | MSG_COPY) > 0) {
        printf("Type=%ld Sender=%u Seq=%u Size=%u\n",
               msg.header.mtype, msg.header.sender_id,
               msg.header.sequence, msg.header.payload_size);
    }
    usleep(100000);  // 10 Hz polling
}
```

---

## Performance Tips

### When to use Shared Memory

✅ Large data (images, video, audio)  
✅ High-frequency updates (60+ FPS)  
✅ Zero-copy requirement  
✅ Data > 1 KB  

### When to use Message Queues

✅ Small control messages (< 1 KB)  
✅ Event notifications  
✅ RPC-style communication  
✅ Priority via message types  

### Optimization

1. **Batch operations:** Send multiple messages at once
2. **Use shared memory for bulk:** Message queue for notifications only
3. **Minimize syscalls:** Attach once, use many times
4. **Use damage tracking:** Only update changed regions
5. **Use `IPC_NOWAIT`:** Don't block on non-critical messages

---

## See Also

- **Full Specification:** `IPC_PROTOCOL_SPEC.md`
- **Test Example:** `userspace/tests/test_ipc_window.c`
- **Implementation:** `userspace/compositor/ipc.c`

---

*Quick Reference v1.0*  
*Agent 9: IPC Integration*
