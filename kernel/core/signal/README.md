# Signal System

This directory contains the signal handling system for process management.

## Files

- `kill.c` - Implementation of `sys_kill` syscall for sending signals to processes

## Implemented Syscalls

### SYS_KILL (26)

Send a signal to a process.

**Signature:**
```c
int64_t sys_kill(uint64_t pid, uint64_t sig, ...);
```

**Arguments:**
- `pid`: Target process ID
- `sig`: Signal number (0-31)

**Returns:**
- 0 on success
- -ESRCH if process not found
- -EINVAL if signal number is invalid
- -ENOTSUP if signal type is not yet implemented

**Supported Signals:**
- `SIGKILL (9)`: Immediately terminate process
- `SIGTERM (15)`: Graceful termination (currently same as SIGKILL)
- `SIGSTOP (19)`: Suspend process execution
- `SIGCONT (18)`: Resume suspended process
- Signal 0: Check if process exists (no signal sent)

**Future Work:**
- Full signal delivery mechanism with signal handlers
- Signal masks and pending signals
- Permission checks based on UID/GID
- Additional signal types (SIGHUP, SIGINT, etc.)
- Signal queuing for real-time signals

## Usage Example

```c
#include <syscall.h>

// Check if process exists
if (kill(pid, 0) == 0) {
    printf("Process %d exists\n", pid);
}

// Terminate process
kill(pid, SIGKILL);

// Suspend process
kill(pid, SIGSTOP);

// Resume process
kill(pid, SIGCONT);
```

## Integration with Task Manager

The task manager (`userspace/apps/taskmanager/`) uses these syscalls to:
- Kill unresponsive processes
- Suspend/resume processes
- Check process status

## Testing

See `userspace/apps/test_process_mgmt.c` for comprehensive syscall tests.
