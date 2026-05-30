# IPC Quick Start Guide
**AutomationOS Inter-Process Communication**

---

## 5-Minute Quick Start

### Shared Memory Example

```c
#include "../libc/ipc.h"

// Process A: Create and write
int shm_id = shmget(0x1234, 4096, IPC_CREAT | 0666);
uint32_t* data = shmat(shm_id, NULL, 0);
data[0] = 0xDEADBEEF;
shmdt(data);

// Process B: Attach and read
int shm_id = shmget(0x1234, 4096, 0);
uint32_t* data = shmat(shm_id, NULL, 0);
printf("Value: %x\n", data[0]);  // Prints: DEADBEEF
shmdt(data);

// Cleanup
shmctl(shm_id, IPC_RMID, NULL);
```

### Message Queue Example

```c
#include "../libc/ipc.h"

// Process A: Create and send
int msg_id = msgget(0x5678, IPC_CREAT | 0666);
struct { long type; char text[64]; } msg;
msg.type = 1;
strcpy(msg.text, "Hello from A!");
msgsnd(msg_id, &msg, strlen(msg.text)+1, 0);

// Process B: Receive
struct { long type; char text[64]; } msg;
msgrcv(msg_id, &msg, 64, 1, 0);
printf("Received: %s\n", msg.text);  // Prints: Hello from A!

// Cleanup
msgctl(msg_id, IPC_RMID, NULL);
```

---

## API Cheat Sheet

### Shared Memory

| Function | Purpose | Returns |
|----------|---------|---------|
| `shmget(key, size, flags)` | Create/get segment | shm_id or -1 |
| `shmat(shm_id, addr, flags)` | Attach to address space | void* or NULL |
| `shmdt(addr)` | Detach from address space | 0 or -1 |
| `shmctl(shm_id, cmd, buf)` | Control operations | 0 or -1 |

### Message Queues

| Function | Purpose | Returns |
|----------|---------|---------|
| `msgget(key, flags)` | Create/get queue | msg_id or -1 |
| `msgsnd(msg_id, msg, size, flags)` | Send message | 0 or -1 |
| `msgrcv(msg_id, msg, size, type, flags)` | Receive message | bytes or -1 |
| `msgctl(msg_id, cmd, buf)` | Control operations | 0 or -1 |

---

## Common Flags

### Shared Memory Flags (shmget)
- `IPC_CREAT` - Create if doesn't exist
- `IPC_EXCL` - Fail if exists (with IPC_CREAT)
- `0666` - Read/write permissions

### Shared Memory Flags (shmat)
- `SHM_RDONLY` - Read-only attachment
- `SHM_RND` - Round address to SHMLBA
- `0` - Read-write, kernel chooses address

### Message Queue Flags (msgsnd/msgrcv)
- `IPC_NOWAIT` - Non-blocking mode
- `MSG_NOERROR` - Truncate if too long
- `0` - Blocking mode (default)

---

## Error Codes

| Code | Meaning |
|------|---------|
| `-2` | No such IPC object |
| `-12` | Out of memory |
| `-13` | Permission denied |
| `-14` | Bad address |
| `-17` | IPC object exists |
| `-22` | Invalid argument |
| `-42` | No message of desired type |

---

## Limits

| Resource | Limit |
|----------|-------|
| Max segment size | 256 MB |
| Max message size | 8 KB |
| Max queue size | 16 KB |
| Max messages per queue | 100 |
| Page alignment | 4 KB |

---

## Use Cases

### 1. Compositor Window Surface

```c
// Compositor: Create surface for 640x480 window
int shm_id = shmget(window_key, 640*480*4, IPC_CREAT | 0666);
uint32_t* pixels = shmat(shm_id, NULL, 0);

// Send shm_id to application
struct { long type; int shm_id; } msg = { 1, shm_id };
msgsnd(app_queue, &msg, sizeof(int), 0);

// Application: Attach and draw
struct { long type; int shm_id; } msg;
msgrcv(my_queue, &msg, sizeof(int), 1, 0);
uint32_t* pixels = shmat(msg.shm_id, NULL, 0);
pixels[0] = 0xFF0000FF;  // Red pixel
```

### 2. Window Manager Control

```c
// Send window create request
struct {
    long type;
    struct { int x, y, w, h; char title[64]; } data;
} msg;
msg.type = MSG_WINDOW_CREATE;
msg.data.x = 100; msg.data.y = 100;
msg.data.w = 640; msg.data.h = 480;
strcpy(msg.data.title, "Terminal");
msgsnd(wm_queue, &msg, sizeof(msg.data), 0);
```

### 3. Input Event Distribution

```c
// Keyboard driver sends event
struct {
    long type;  // 1=keyboard, 2=mouse
    struct { uint16_t keycode; uint8_t pressed; } data;
} event;
event.type = 1;
event.data.keycode = KEY_A;
event.data.pressed = 1;
msgsnd(input_queue, &event, sizeof(event.data), 0);

// Window manager receives keyboard events only
msgrcv(input_queue, &event, sizeof(event.data), 1, 0);
```

---

## Best Practices

### Shared Memory
✅ **DO:**
- Always detach (`shmdt`) before exiting
- Use `IPC_RMID` when done to free memory
- Check return values for errors
- Use page-aligned sizes for efficiency

❌ **DON'T:**
- Access after detaching (undefined behavior)
- Assume addresses persist across processes
- Share pointers in shared memory (use offsets)

### Message Queues
✅ **DO:**
- Use unique message types for filtering
- Keep messages small (< 1 KB)
- Use `IPC_NOWAIT` for non-critical messages
- Check for `-42` (no message) with `IPC_NOWAIT`

❌ **DON'T:**
- Send large data (use shared memory instead)
- Block indefinitely (use timeout or `IPC_NOWAIT`)
- Forget to remove queues (`IPC_RMID`)

---

## Debugging Tips

### Check if IPC object exists
```c
int shm_id = shmget(key, 0, 0);  // 0 size, no create
if (shm_id < 0) {
    printf("Shared memory doesn't exist\n");
}
```

### Verify attachment
```c
void* addr = shmat(shm_id, NULL, 0);
if (addr == NULL) {
    printf("Failed to attach: check permissions\n");
}
```

### Test message queue
```c
// Send test message
msgsnd(msg_id, &test_msg, sizeof(test_msg), IPC_NOWAIT);

// Receive test message
if (msgrcv(msg_id, &test_msg, sizeof(test_msg), 0, IPC_NOWAIT) < 0) {
    printf("No messages in queue\n");
}
```

---

## Performance Tips

### When to Use Shared Memory
- ✅ Large data (images, video, audio buffers)
- ✅ High-frequency updates (60+ FPS rendering)
- ✅ Zero-copy requirement
- ✅ Data > 1 KB

### When to Use Message Queues
- ✅ Small control messages (< 1 KB)
- ✅ Event notifications
- ✅ RPC-style communication
- ✅ Priority via message types

### Optimization
1. **Batch operations:** Send multiple messages at once
2. **Use shared memory for bulk:** Message queue for notifications only
3. **Align to page boundaries:** Shared memory performs better
4. **Minimize syscalls:** Attach once, use many times

---

## Testing

### Run IPC test
```bash
# Build and run
make userspace
./test_ipc

# Expected output:
# === IPC Test Program ===
# [TEST 1] Creating shared memory segment...
# [PASS] Created shared memory segment ID: 1
# ...
# === All tests passed! ===
```

### Manual test
```c
// test_shm.c
int main() {
    int id = shmget(0x1234, 4096, IPC_CREAT | 0666);
    printf("SHM ID: %d\n", id);
    
    uint32_t* data = shmat(id, NULL, 0);
    printf("Attached at: %p\n", data);
    
    data[0] = 42;
    printf("Wrote: %d\n", data[0]);
    
    shmdt(data);
    shmctl(id, IPC_RMID, NULL);
    return 0;
}
```

---

## Common Patterns

### Producer-Consumer (Shared Memory)
```c
// Producer
int shm_id = shmget(key, 4096, IPC_CREAT | 0666);
struct buffer { int head, tail; char data[4000]; } *buf;
buf = shmat(shm_id, NULL, 0);
buf->data[buf->tail++] = 'A';  // Write
shmdt(buf);

// Consumer
buf = shmat(shm_id, NULL, 0);
char c = buf->data[buf->head++];  // Read
shmdt(buf);
```

### Request-Response (Message Queue)
```c
// Client
struct { long type; int req_id; char data[64]; } req;
req.type = REQ_TYPE;
req.req_id = getpid();
msgsnd(server_queue, &req, sizeof(req)-sizeof(long), 0);

struct { long type; char result[64]; } resp;
msgrcv(client_queue, &resp, sizeof(resp)-sizeof(long), req.req_id, 0);

// Server
msgrcv(server_queue, &req, sizeof(req)-sizeof(long), REQ_TYPE, 0);
// ... process request ...
resp.type = req.req_id;
msgsnd(client_queue, &resp, sizeof(resp)-sizeof(long), 0);
```

---

## FAQ

**Q: How do I share memory between compositor and app?**  
A: Compositor creates with `shmget`, gets ID, sends ID via message queue, app attaches with `shmat(id, ...)`.

**Q: Can I use pointers in shared memory?**  
A: No! Virtual addresses differ per process. Use array indices or offsets from base.

**Q: What happens if I don't detach?**  
A: Memory leaks! Segment not freed until all processes detach + `IPC_RMID`.

**Q: Why does `msgrcv` return -42?**  
A: No message of requested type. Use `IPC_NOWAIT` or change `msgtyp` to 0 (any type).

**Q: How do I wait for a message?**  
A: Currently non-blocking only. Use polling or wait for blocking support in Phase 2.

**Q: Can I resize shared memory?**  
A: No. Create new segment and migrate data.

---

## Next Steps

1. **Read:** IPC_IMPLEMENTATION_COMPLETE.md for full details
2. **Test:** Run `userspace/tests/test_ipc.c`
3. **Integrate:** Use in compositor (Agent 5)
4. **Optimize:** Add blocking, huge pages (Phase 2)

---

*Quick Start Guide v1.0*  
*AutomationOS IPC Subsystem*  
*Agent 1: IPC Architect*
