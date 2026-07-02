# Agent 11: Terminal PTY Integration Plan

## Overview
Wire the terminal application to the PTY driver and window manager to create a functional interactive terminal.

## Current State

### Completed Components
1. **PTY Driver** (`kernel/drivers/pty/`)
   - Full POSIX-style PTY implementation
   - VFS integration with `/dev/ptmx` and `/dev/pts/*`
   - Line discipline (canonical mode, echo, signals)
   - Window size (TIOCGWINSZ/TIOCSWINSZ)
   - Termios control

2. **Terminal Applications**
   - Simple terminal: `userspace/terminal/` (basic, for testing)
   - Full terminal: `userspace/apps/terminal/` (GPU, tabs, VT100)

3. **Window Manager** (`userspace/wm/`)
   - Minimal stub implementation
   - Needs IPC protocol

### Missing Components
1. **ioctl syscall** - Required for TIOCGWINSZ/TIOCSWINSZ
2. **Window Manager IPC** - Protocol for creating windows, getting input events
3. **Terminal-WM Integration** - Connect terminal to window manager
4. **Shell Integration** - Spawn shell in PTY

## Implementation Plan

### Task 1: Add ioctl Syscall
**File**: `kernel/include/syscall.h`, `kernel/core/syscall/syscall.c`, `kernel/core/syscall/handlers.c`

- Add `SYS_IOCTL` (syscall #26)
- Implement `sys_ioctl()` handler
- Route to VFS ioctl handler
- Support PTY ioctls (TIOCGWINSZ, TIOCSWINSZ, TCGETS, TCSETS)

**Priority**: HIGH - Required for PTY operations

### Task 2: Implement Window Manager IPC Protocol
**File**: `userspace/wm/wm_ipc.h`, `userspace/wm/wm_ipc.c`

Define IPC protocol using message queues or shared memory:
```c
// Window creation
typedef struct {
    uint32_t width, height;
    char title[256];
    uint32_t flags;
} wm_create_window_req_t;

typedef struct {
    uint32_t window_id;
    void *surface;  // Shared memory framebuffer
} wm_create_window_resp_t;

// Input events
typedef enum {
    WM_EVENT_KEY_PRESS,
    WM_EVENT_KEY_RELEASE,
    WM_EVENT_MOUSE_MOVE,
    WM_EVENT_MOUSE_BUTTON,
    WM_EVENT_WINDOW_CLOSE,
    WM_EVENT_WINDOW_RESIZE
} wm_event_type_t;

typedef struct {
    wm_event_type_t type;
    uint32_t window_id;
    union {
        struct { uint32_t keycode, modifiers; } key;
        struct { int32_t x, y; } mouse_move;
        struct { uint32_t button, state; int32_t x, y; } mouse_button;
        struct { uint32_t width, height; } resize;
    } data;
} wm_event_t;
```

**Priority**: HIGH - Required for window creation

### Task 3: Integrate Terminal with PTY
**File**: `userspace/terminal/main.c`, `userspace/terminal/shell.c`

Replace stub implementations with real PTY integration:

1. **Open PTY**: Call `open("/dev/ptmx", O_RDWR)`
2. **Set window size**: `ioctl(pty_fd, TIOCSWINSZ, &winsize)`
3. **Fork and exec shell**:
   ```c
   pid = fork();
   if (pid == 0) {
       setsid();
       int slave_fd = open("/dev/pts/0", O_RDWR);
       dup2(slave_fd, 0);  // stdin
       dup2(slave_fd, 1);  // stdout
       dup2(slave_fd, 2);  // stderr
       execve("/bin/sh", argv, NULL);
   }
   ```
4. **Event loop**:
   - Read from PTY → Parse VT100 → Update buffer → Render
   - Read keyboard events from WM → Write to PTY
   - Handle window resize → Update PTY window size

**Priority**: HIGH - Core functionality

### Task 4: Implement VT100 Parser
**File**: `userspace/terminal/vt_parser.c` or use `userspace/apps/terminal/vt_parser.c`

Parse escape sequences:
- Cursor movement: `ESC[H`, `ESC[A/B/C/D`
- Clear screen: `ESC[2J`
- Colors: `ESC[30-37m` (foreground), `ESC[40-47m` (background)
- Text attributes: `ESC[1m` (bold), `ESC[4m` (underline), etc.

**Priority**: MEDIUM - Can start with basic text output, add later

### Task 5: Test Terminal Workflow
**Test cases**:

1. **Launch terminal**
   - Terminal opens window via WM IPC
   - Window surface allocated (shared memory)
   - Terminal registered for input events

2. **Spawn shell**
   - Terminal opens `/dev/ptmx`
   - Fork + exec `/bin/sh` on PTY slave
   - Shell prints prompt to PTY
   - Terminal reads from PTY and displays

3. **Type command**
   - User presses keys
   - WM sends key events to terminal
   - Terminal writes to PTY
   - PTY echoes back (line discipline)
   - Terminal renders characters

4. **Execute command**
   - User presses Enter
   - PTY sends newline to shell
   - Shell executes command
   - Shell output written to PTY
   - Terminal reads and displays

5. **Resize window**
   - WM sends resize event
   - Terminal updates dimensions
   - Terminal calls `ioctl(TIOCSWINSZ)`
   - Shell receives SIGWINCH

**Priority**: HIGH - Validation

## Deliverables

1. **ioctl syscall implementation**
   - `SYS_IOCTL` added to syscall table
   - PTY ioctls working

2. **WM IPC protocol**
   - Header file with protocol definition
   - Simple WM implementation or mock

3. **Terminal-PTY integration**
   - Terminal opens `/dev/ptmx`
   - Spawns shell on PTY
   - Bidirectional I/O working

4. **Working demo**
   - Terminal launches
   - Shell prompt appears
   - Commands can be typed and executed
   - Output displayed

## Timeline
- **Task 1 (ioctl)**: 2 hours
- **Task 2 (WM IPC)**: 3 hours
- **Task 3 (PTY integration)**: 4 hours
- **Task 4 (VT100 parser)**: 3 hours
- **Task 5 (Testing)**: 2 hours

**Total**: ~14 hours (1.5 days)

## Dependencies
- Agent 9: Terminal code (COMPLETE)
- Agent 10: PTY driver (IN PROGRESS - needs ioctl syscall)
- Agent 6: Font rendering (COMPLETE)
- Compositor/WM (PARTIAL - needs IPC)

## Success Criteria
- [ ] ioctl syscall implemented and working
- [ ] WM IPC protocol defined
- [ ] Terminal creates window
- [ ] Terminal spawns shell via PTY
- [ ] Can type in terminal and see echo
- [ ] Commands execute and show output
- [ ] Basic VT100 escape sequences handled
- [ ] Window resize updates PTY size
