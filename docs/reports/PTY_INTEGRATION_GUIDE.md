# PTY Integration Guide
## How to Integrate PTY Driver into AutomationOS Kernel

**Target:** Agent 12 (Integration Test Lead) and kernel maintainers  
**Purpose:** Step-by-step instructions to activate PTY support

---

## Integration Checklist

### ✅ Phase 1: Kernel Driver Integration

#### Step 1: Build PTY Driver

```bash
cd kernel/drivers/pty
make
# Produces: libpty.a
```

#### Step 2: Add PTY to Kernel Makefile

Edit `kernel/Makefile`:

```makefile
# Add PTY driver
DRIVER_OBJS += drivers/pty/libpty.a

# Update link command
$(KERNEL_BIN): $(OBJS) $(DRIVER_OBJS)
    $(LD) -n -T linker.ld -o $(KERNEL_BIN) $(OBJS) $(DRIVER_OBJS)
```

#### Step 3: Initialize PTY at Boot

Edit `kernel/init/main.c`:

```c
#include "../drivers/pty/pty.h"

void kernel_main(void) {
    // ... existing initialization ...
    
    vfs_init();
    ramfs_init();
    
    // NEW: Initialize PTY driver
    pty_init();
    
    // NEW: Create PTY device nodes
    vfs_inode_t *dev_root = vfs_path_lookup("/dev");
    if (dev_root) {
        pty_dev_init(dev_root);
    }
    
    // ... rest of initialization ...
}
```

#### Step 4: Add ioctl Syscall

Edit `kernel/include/syscall.h`:

```c
#define SYS_IOCTL   26  // New syscall number

// Declare handler
int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t argp,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6);
```

Edit `kernel/core/syscall/syscall.c`:

```c
void syscall_init(void) {
    // ... existing syscalls ...
    
    syscall_table[SYS_IOCTL] = sys_ioctl;  // NEW
}
```

Edit `kernel/core/syscall/handlers.c`:

```c
#include "../../drivers/pty/pty.h"

int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t argp,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    // Get file from VFS
    vfs_file_t *file = vfs_fd_get((int)fd);
    if (!file) {
        return -EBADF;
    }
    
    // Check if file is a PTY device
    if (file->inode && file->inode->type == VFS_TYPE_DEVICE) {
        // Call PTY ioctl handler
        return pty_ioctl(file, (uint32_t)request, (void *)argp);
    }
    
    return -ENOTSUP;  // Not a PTY
}
```

#### Step 5: Update VFS to Handle PTY Devices

Edit `kernel/fs/vfs.c`:

```c
int vfs_open(const char *path, int flags, int mode) {
    // ... existing path lookup ...
    
    // NEW: Special handling for /dev/ptmx
    if (strcmp(path, "/dev/ptmx") == 0) {
        vfs_inode_t *ptmx_inode = vfs_path_lookup("/dev/ptmx");
        if (ptmx_inode && ptmx_inode->ops) {
            int fd = vfs_fd_alloc();
            if (fd < 0) return -1;
            
            vfs_file_t *file = kmalloc(sizeof(vfs_file_t));
            memset(file, 0, sizeof(vfs_file_t));
            file->inode = ptmx_inode;
            file->flags = flags;
            
            // Call PTY open handler
            if (ptmx_inode->ops->open) {
                if (ptmx_inode->ops->open(ptmx_inode, file) < 0) {
                    kfree(file);
                    vfs_fd_free(fd);
                    return -1;
                }
            }
            
            vfs_state.fd_table[fd] = file;
            return fd;
        }
    }
    
    // ... rest of vfs_open ...
}
```

---

### ✅ Phase 2: Userspace Integration

#### Step 6: Update Terminal Makefile

Edit `userspace/apps/terminal/Makefile`:

```makefile
OBJS = terminal.o vt_parser.o renderer.o tabs.o profiles.o buffer.o utils.o pty_impl.o

# Add pty_impl.c to compilation
pty_impl.o: pty_impl.c terminal.h
	$(CC) $(CFLAGS) -c pty_impl.c -o pty_impl.o
```

#### Step 7: Remove Stub Implementations

Edit `userspace/apps/terminal/utils.c`:

**REMOVE** the following stub functions (lines 71-104):
- `pty_open()`
- `pty_close()`
- `pty_resize()`
- `pty_spawn()`
- `pty_read()`
- `pty_write()`

These are now implemented in `pty_impl.c`.

#### Step 8: Update Terminal to Use PTY

Edit `userspace/apps/terminal/tabs.c` (in `tab_create()` function):

```c
tab_t *tab_create(terminal_t *term, profile_t *profile) {
    // ... existing code ...
    
    // NEW: Open PTY for root pane
    pane_t *root = pane_create(DEFAULT_COLS, DEFAULT_ROWS);
    if (root) {
        root->pty_fd = pty_open(DEFAULT_COLS, DEFAULT_ROWS);
        if (root->pty_fd >= 0) {
            // Spawn shell
            const char *shell = profile ? profile->shell : pty_find_shell();
            char *argv[] = { (char *)shell, NULL };
            root->child_pid = pty_spawn(root->pty_fd, shell, argv);
            
            kprintf("[Terminal] Spawned shell (PID %d) on PTY %d\n", 
                    root->child_pid, root->pty_fd);
        }
    }
    
    tab->root_pane = root;
    tab->focused_pane = root;
    
    // ... rest of function ...
}
```

---

### ✅ Phase 3: Build and Test

#### Step 9: Rebuild Kernel

```bash
cd kernel
make clean
make

# Verify PTY driver is linked
x86_64-elf-nm kernel.bin | grep pty_
# Should show:
#   pty_init
#   pty_allocate
#   pty_master_read
#   pty_master_write
#   ... etc
```

#### Step 10: Rebuild Terminal

```bash
cd userspace/apps/terminal
make clean
make

# Verify PTY symbols
x86_64-elf-nm terminal | grep pty_
# Should show:
#   pty_open
#   pty_spawn
#   pty_read
#   pty_write
#   ... etc
```

#### Step 11: Update Initrd

```bash
cd tools
./mkinitrd ../build/initrd.img \
    ../userspace/apps/terminal/terminal \
    ../userspace/bin/sh \
    ../userspace/bin/autoshell

# Verify terminal binary is in initrd
./dump-initrd ../build/initrd.img
```

#### Step 12: Boot Test

```bash
qemu-system-x86_64 \
    -kernel build/kernel.bin \
    -initrd build/initrd.img \
    -serial stdio \
    -m 4G

# Expected output:
# [PTY] Initializing pseudo-terminal driver...
# [PTY] Pseudo-terminal driver initialized (32 max pairs)
# [PTY] Created /dev/ptmx and /dev/pts/
# [VFS] Mounted ramfs at /
# [Terminal] Initializing...
# [Terminal] Spawned shell (PID 3) on PTY 0
# [Shell] AutomationOS Shell v1.0
# $ _
```

---

## Verification Tests

### Test 1: PTY Allocation

```c
// In kernel test harness
int pty0 = pty_allocate();
assert(pty0 == 0);
int pty1 = pty_allocate();
assert(pty1 == 1);
pty_free(pty0);
pty_free(pty1);
```

### Test 2: Master/Slave I/O

```c
int pty_idx = pty_allocate();

// Write to master → read from slave
const char *input = "hello\n";
pty_master_write(pty_idx, input, 6);

char output[10];
int n = pty_slave_read(pty_idx, output, 10);
assert(n == 6);
assert(memcmp(output, "hello\n", 6) == 0);

pty_free(pty_idx);
```

### Test 3: Line Discipline (Canonical Mode)

```c
int pty_idx = pty_allocate();

// Type "test" + backspace twice + "st" + Enter
pty_master_write(pty_idx, "test", 4);
pty_master_write(pty_idx, "\b\b", 2);  // Remove "st"
pty_master_write(pty_idx, "st\n", 3);  // Add "st\n"

// Should read "test\n" (canonical mode processes backspace)
char line[20];
int n = pty_slave_read(pty_idx, line, 20);
assert(n == 5);
assert(memcmp(line, "test\n", 5) == 0);

pty_free(pty_idx);
```

### Test 4: Window Size

```c
int pty_idx = pty_allocate();

// Set window size
pty_set_winsize(pty_idx, 40, 120);

// Get window size
uint16_t rows, cols;
pty_get_winsize(pty_idx, &rows, &cols);
assert(rows == 40);
assert(cols == 120);

pty_free(pty_idx);
```

### Test 5: Terminal End-to-End

**Manual test in running system:**

1. Boot kernel with PTY support
2. Launch terminal from desktop
3. Type `echo "Hello, AutomationOS!"`
4. Verify output appears in terminal
5. Press Ctrl+C → verify shell prompt returns
6. Type `exit` → verify shell exits gracefully

---

## Debugging Guide

### Issue: PTY init fails

**Symptom:** `[PTY] Failed to allocate PTY` message

**Fix:**
- Check `pty_init()` was called before any PTY allocation
- Verify `pty_state.initialized == true`
- Check kernel heap has enough memory (16KB × 32 PTYs = 512KB)

### Issue: Can't open /dev/ptmx

**Symptom:** `open("/dev/ptmx")` returns -1

**Fix:**
- Verify `pty_dev_init()` was called
- Check `/dev/ptmx` exists: `ls /dev/` in shell
- Ensure VFS device support is enabled
- Check `ptmx_open()` handler is registered

### Issue: Shell doesn't spawn

**Symptom:** Terminal opens but no prompt

**Fix:**
- Verify `/bin/sh` or `/bin/autoshell` exists in initrd
- Check `fork()` syscall is implemented
- Check `execve()` syscall is implemented
- Verify slave PTY `/dev/pts/0` was created
- Check `pty_spawn()` return value (should be > 0)

### Issue: No echo in terminal

**Symptom:** Typing doesn't show characters

**Fix:**
- Check `ECHO` flag is set in termios
- Verify `pty_process_input()` is calling `pty_echo()`
- Check master-to-slave buffer is not full
- Verify terminal is rendering buffer contents

### Issue: Backspace doesn't work

**Symptom:** Backspace shows as `^?` or `^H`

**Fix:**
- Check `ICANON` flag is set in termios
- Verify `VERASE` control character is 0x7F (DEL)
- Check `pty_process_input()` handles `c_cc[VERASE]`
- Ensure echo is sending `\b \b` sequence

---

## Performance Monitoring

### Key Metrics

1. **PTY Allocation Time**: Should be < 100 μs
   ```c
   uint64_t start = rdtsc();
   int pty = pty_allocate();
   uint64_t end = rdtsc();
   kprintf("PTY alloc: %llu cycles\n", end - start);
   ```

2. **Character Echo Latency**: Should be < 10 μs
   ```c
   // Measure time from master_write to slave_read
   ```

3. **Buffer Utilization**: Monitor buffer fullness
   ```c
   uint32_t avail = pty_master_available(pty_idx);
   if (avail > PTY_BUFFER_SIZE * 0.8) {
       kprintf("WARNING: PTY buffer 80%% full\n");
   }
   ```

4. **PTY Pair Usage**: Track active PTYs
   ```c
   // Count allocated PTY pairs
   int active = 0;
   for (int i = 0; i < PTY_MAX_PAIRS; i++) {
       if (pty_state.pairs[i].allocated) active++;
   }
   kprintf("Active PTYs: %d / %d\n", active, PTY_MAX_PAIRS);
   ```

---

## Limitations and Future Work

### Current Limitations

1. **No Flow Control**: No XON/XOFF or CTS/RTS
   - **Impact**: Fast output can overflow buffers
   - **Workaround**: 4KB buffers sufficient for most use

2. **Simplified Termios**: Only ECHO, ICANON, ISIG flags
   - **Impact**: No OPOST, INLCR, ONLCR processing
   - **Workaround**: Shell handles most newline conversion

3. **No Job Control**: Signals detected but not delivered
   - **Impact**: Ctrl+C prints message but doesn't kill process
   - **Workaround**: Implement signal delivery in Phase 2

4. **Static PTY Limit**: 32 PTY pairs maximum
   - **Impact**: Only 32 simultaneous terminals
   - **Workaround**: Increase PTY_MAX_PAIRS if needed

### Planned Enhancements

**Phase 2 (Post-Tier 1):**
- Signal delivery (SIGINT, SIGQUIT, SIGTSTP)
- Full termios support (input/output processing)
- PTY packet mode for multiplexers
- Dynamic PTY allocation (unlimited pairs)

**Phase 3 (Advanced):**
- Flow control (XON/XOFF)
- Job control (tcsetpgrp, tcgetpgrp)
- PTY session management
- Audit logging of PTY I/O

---

## Appendix: File Locations

### Kernel Files

| File | Path | Description |
|------|------|-------------|
| PTY Driver | `kernel/drivers/pty/pty.c` | Core PTY logic |
| PTY Header | `kernel/drivers/pty/pty.h` | Public API |
| PTY Devices | `kernel/drivers/pty/pty_dev.c` | VFS integration |
| Makefile | `kernel/drivers/pty/Makefile` | Build script |

### Userspace Files

| File | Path | Description |
|------|------|-------------|
| PTY Impl | `userspace/apps/terminal/pty_impl.c` | PTY wrappers |
| Terminal | `userspace/apps/terminal/terminal.c` | Main app |
| VT Parser | `userspace/apps/terminal/vt_parser.c` | ANSI escape parser |
| Renderer | `userspace/apps/terminal/renderer.c` | GPU text rendering |

### Integration Points

| File | Changes Required |
|------|------------------|
| `kernel/init/main.c` | Call `pty_init()` and `pty_dev_init()` |
| `kernel/Makefile` | Link `libpty.a` |
| `kernel/include/syscall.h` | Add `SYS_IOCTL` |
| `kernel/core/syscall/handlers.c` | Implement `sys_ioctl()` |
| `kernel/fs/vfs.c` | Handle `/dev/ptmx` in `vfs_open()` |

---

## Support

For issues or questions:
1. Check "Debugging Guide" section above
2. Review `PTY_IMPLEMENTATION_REPORT.md`
3. Examine kernel boot logs for `[PTY]` messages
4. Test PTY driver in isolation (unit tests)

**Integration Lead**: Agent 12  
**PTY Developer**: Agent 9  
**Font Support**: Agent 6 (dependency)  
**Window Manager**: Agent 8 (dependency)

---

*Integration Guide prepared by Agent 9*  
*AutomationOS Desktop Completion Plan - Tier 1*  
*Last Updated: 2026-05-27*
