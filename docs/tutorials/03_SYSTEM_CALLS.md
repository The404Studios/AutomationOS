# Tutorial 3: Understanding System Calls

**Difficulty:** Intermediate  
**Time:** 30-40 minutes  
**Prerequisites:** Tutorials 1-2  

---

## Introduction

System calls are the **gateway between user and kernel space**. They're how programs request services from the operating system.

In this tutorial, you'll learn:

- How system calls work at the CPU level
- The x86_64 `syscall` instruction
- All AutomationOS syscalls in detail
- How to call syscalls from C and assembly
- Error handling and return values
- Performance considerations

By the end, you'll master the userspace-kernel interface.

---

## What is a System Call?

### The Problem

Userspace programs (Ring 3) are restricted:
- Can't directly access hardware (disk, network, keyboard)
- Can't allocate physical memory
- Can't manipulate other processes
- Can't change CPU mode or page tables

**But programs need these services!**

### The Solution

**System calls** provide a controlled interface:

```
┌─────────────────────────────────────┐
│        Userspace (Ring 3)           │
│                                     │
│  printf("hello")                    │
│        ↓                            │
│  write(1, "hello", 5)  ← syscall   │
└─────────────┬───────────────────────┘
              │ syscall instruction
              │ (switch to Ring 0)
              ↓
┌─────────────────────────────────────┐
│         Kernel (Ring 0)             │
│                                     │
│  sys_write(fd, buf, len)            │
│        ↓                            │
│  serial_console_write(buf, len)    │
└─────────────┬───────────────────────┘
              │ sysret instruction
              │ (switch to Ring 3)
              ↓
┌─────────────────────────────────────┐
│        Userspace (Ring 3)           │
│                                     │
│  (continues execution)              │
└─────────────────────────────────────┘
```

---

## The x86_64 SYSCALL Instruction

### Calling Convention

AutomationOS uses the standard x86_64 Linux syscall convention:

| Register | Purpose           |
|----------|-------------------|
| RAX      | Syscall number    |
| RDI      | Argument 1        |
| RSI      | Argument 2        |
| RDX      | Argument 3        |
| R10      | Argument 4 (not RCX!) |
| R8       | Argument 5        |
| R9       | Argument 6        |
| RAX      | Return value      |

**Note:** R10 is used instead of RCX because `syscall` clobbers RCX with the return address.

### CPU State Transitions

When `syscall` executes:

1. **Save user state:**
   - RCX ← RIP (return address)
   - R11 ← RFLAGS (CPU flags)

2. **Switch to kernel:**
   - CS ← kernel code segment
   - SS ← kernel stack segment
   - RIP ← kernel syscall handler (from MSR)
   - RFLAGS masked according to MSR

3. **Kernel handles request**

4. **Return with `sysret`:**
   - RIP ← RCX (user return address)
   - RFLAGS ← R11 (user flags)
   - CS/SS ← user segments

---

## AutomationOS System Calls

### Available Syscalls (Phase 1)

| Number | Name     | Signature | Description |
|--------|----------|-----------|-------------|
| 0      | `exit`   | `void exit(int status)` | Terminate process |
| 1      | `fork`   | `int fork(void)` | Create child process |
| 2      | `read`   | `ssize_t read(int fd, void* buf, size_t count)` | Read from file descriptor |
| 3      | `write`  | `ssize_t write(int fd, const void* buf, size_t count)` | Write to file descriptor |
| 4      | `getpid` | `int getpid(void)` | Get process ID |

### Syscall Numbering

Numbers are defined in `kernel/include/syscall.h`:

```c
#define SYS_EXIT    0
#define SYS_FORK    1
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_GETPID  4
```

---

## Syscall Details

### 1. exit() - Terminate Process

**Signature:** `void exit(int status)`

**Parameters:**
- `status` - Exit code (0 = success, non-zero = error)

**Returns:** Never returns

**Description:** Terminates the current process and returns the exit status to the parent. The kernel frees all resources (memory, file descriptors, etc.) and marks the process as a zombie until the parent calls `waitpid()`.

**Example (C):**

```c
#include "../libc/syscall.h"

void _start(void) {
    // Do some work
    
    exit(0);  // Exit successfully
}
```

**Example (Assembly):**

```nasm
section .text
global _start

_start:
    ; Exit with status 0
    mov rax, 0      ; SYS_EXIT
    mov rdi, 0      ; status = 0
    syscall         ; Invoke kernel
    ; Never returns
```

**Kernel Handler:** `sys_exit()` in `kernel/core/syscall/handlers.c`

---

### 2. fork() - Create Child Process

**Signature:** `int fork(void)`

**Parameters:** None

**Returns:**
- In parent: Child PID (positive integer)
- In child: 0
- On error: -1

**Description:** Creates an exact copy of the current process. The child inherits:
- Memory (copy-on-write)
- File descriptors
- Register state

Both processes continue executing from the point after `fork()`.

**Example:**

```c
#include "../libc/syscall.h"
#include "../libc/stdio.h"

void _start(void) {
    printf("Before fork\n");
    
    int pid = fork();
    
    if (pid == 0) {
        // Child process
        printf("I'm the child! PID=%d\n", getpid());
        exit(0);
    } else if (pid > 0) {
        // Parent process
        printf("I'm the parent! Child PID=%d\n", pid);
        exit(0);
    } else {
        // Error
        printf("Fork failed!\n");
        exit(1);
    }
}
```

**Output:**

```
Before fork
I'm the parent! Child PID=3
I'm the child! PID=3
```

**Note:** In Phase 1, `fork()` is implemented but may have limitations. Full process management comes in Phase 2.

---

### 3. read() - Read from File Descriptor

**Signature:** `ssize_t read(int fd, void* buf, size_t count)`

**Parameters:**
- `fd` - File descriptor (0 = stdin, 1 = stdout, 2 = stderr)
- `buf` - Buffer to read into
- `count` - Maximum bytes to read

**Returns:**
- Number of bytes read (>= 0)
- -1 on error

**Description:** Reads up to `count` bytes from file descriptor `fd` into `buf`.

**Example:**

```c
#include "../libc/syscall.h"
#include "../libc/stdio.h"

void _start(void) {
    char buf[128];
    
    printf("Enter your name: ");
    
    int n = read(0, buf, sizeof(buf) - 1);  // Read from stdin
    
    if (n > 0) {
        buf[n] = '\0';  // Null terminate
        printf("Hello, %s!\n", buf);
    }
    
    exit(0);
}
```

**Note:** In Phase 1, `read()` from stdin may not be fully functional yet. It works for serial console input.

---

### 4. write() - Write to File Descriptor

**Signature:** `ssize_t write(int fd, const void* buf, size_t count)`

**Parameters:**
- `fd` - File descriptor (0 = stdin, 1 = stdout, 2 = stderr)
- `buf` - Buffer to write from
- `count` - Number of bytes to write

**Returns:**
- Number of bytes written (>= 0)
- -1 on error

**Description:** Writes up to `count` bytes from `buf` to file descriptor `fd`.

**Example:**

```c
#include "../libc/syscall.h"
#include "../libc/string.h"

void _start(void) {
    const char* msg = "Hello from write syscall!\n";
    
    // Write directly using syscall
    write(1, msg, strlen(msg));  // 1 = stdout
    
    exit(0);
}
```

**Advanced Example - Write to Serial Port:**

```c
void _start(void) {
    const char* msg1 = "Line 1\n";
    const char* msg2 = "Line 2\n";
    const char* msg3 = "Line 3\n";
    
    write(1, msg1, strlen(msg1));
    write(1, msg2, strlen(msg2));
    write(1, msg3, strlen(msg3));
    
    exit(0);
}
```

---

### 5. getpid() - Get Process ID

**Signature:** `int getpid(void)`

**Parameters:** None

**Returns:** Process ID (always > 0)

**Description:** Returns the current process's unique identifier (PID). PID 1 is always the init process.

**Example:**

```c
#include "../libc/syscall.h"
#include "../libc/stdio.h"

void _start(void) {
    int pid = getpid();
    
    if (pid == 1) {
        printf("I'm the init process!\n");
    } else {
        printf("I'm process %d\n", pid);
    }
    
    exit(0);
}
```

**Use Cases:**
- Logging and debugging
- Process-specific behavior
- Creating unique IDs (e.g., PID-based temp files)

---

## Making Syscalls in Assembly

Sometimes you need raw control. Here's how to make syscalls directly in assembly.

### Basic Template

```nasm
section .data
    msg db "Hello from assembly!", 10  ; 10 = newline
    msg_len equ $ - msg

section .text
global _start

_start:
    ; write(1, msg, msg_len)
    mov rax, 3          ; SYS_WRITE
    mov rdi, 1          ; fd = stdout
    lea rsi, [rel msg]  ; buf = msg
    mov rdx, msg_len    ; count = msg_len
    syscall
    
    ; exit(0)
    mov rax, 0          ; SYS_EXIT
    mov rdi, 0          ; status = 0
    syscall
```

### Preserving Registers

The `syscall` instruction clobbers RCX and R11. If you need them:

```nasm
_start:
    ; Save RCX and R11
    push rcx
    push r11
    
    ; Make syscall
    mov rax, 4          ; SYS_GETPID
    syscall
    ; RAX now contains PID
    
    ; Restore (though they're clobbered anyway)
    pop r11
    pop rcx
```

### Multiple Syscalls

```nasm
_start:
    ; Get PID
    mov rax, 4          ; SYS_GETPID
    syscall
    mov r12, rax        ; Save PID in callee-saved register
    
    ; Write PID (simplified - normally you'd convert to ASCII)
    mov rax, 3          ; SYS_WRITE
    mov rdi, 1          ; stdout
    lea rsi, [rel msg]
    mov rdx, 10
    syscall
    
    ; Exit
    mov rax, 0          ; SYS_EXIT
    xor rdi, rdi        ; status = 0
    syscall
```

---

## Making Syscalls from C

The libc provides convenient wrappers. Let's see how they work.

### libc Implementation

Check `userspace/libc/syscall.c`:

```c
int write(int fd, const void* buf, size_t count) {
    int64_t ret;
    
    __asm__ volatile (
        "syscall"
        : "=a"(ret)                          // Output: RAX
        : "a"(SYS_WRITE),                    // Input: RAX = 3
          "D"(fd),                           // Input: RDI = fd
          "S"(buf),                          // Input: RSI = buf
          "d"(count)                         // Input: RDX = count
        : "rcx", "r11", "memory"             // Clobbered
    );
    
    return (int)ret;
}
```

**Breakdown:**
- `__asm__ volatile` - Inline assembly
- `"syscall"` - The instruction
- Output constraints: `"=a"(ret)` means "output RAX to ret"
- Input constraints: `"a"(SYS_WRITE)` means "input 3 to RAX"
- Clobbers: Tell compiler RCX, R11, memory are modified

### Using Wrappers

```c
#include "../libc/syscall.h"
#include "../libc/string.h"

void _start(void) {
    const char* msg = "Using syscall wrappers!\n";
    
    // These look like normal functions, but they're syscalls
    int bytes_written = write(1, msg, strlen(msg));
    int pid = getpid();
    
    exit(0);
}
```

---

## Error Handling

### Error Codes

Syscalls return negative values on error:

```c
#define EINVAL   -1   // Invalid argument
#define ENOTSUP  -2   // Operation not supported
#define ENOMEM   -3   // Out of memory
#define EBADF    -4   // Bad file descriptor
#define EPERM    -5   // Operation not permitted
```

### Checking Errors

```c
#include "../libc/syscall.h"
#include "../libc/stdio.h"

void _start(void) {
    char buf[10];
    
    // Try to read from invalid fd
    int n = read(999, buf, sizeof(buf));
    
    if (n < 0) {
        printf("Read failed with error: %d\n", n);
        
        if (n == -4) {  // EBADF
            printf("Bad file descriptor!\n");
        }
    }
    
    exit(0);
}
```

### Kernel Logging

When a syscall fails, the kernel logs it:

```
[SYSCALL] Dispatching syscall 2 (read)
[SYSCALL] Invalid file descriptor: 999
[SYSCALL] Syscall 2 returned -4 (EBADF)
```

---

## Performance Considerations

### Syscall Overhead

Syscalls are **expensive**:
- Mode switch (Ring 3 → Ring 0 → Ring 3)
- Register save/restore
- TLB flush (sometimes)
- Kernel stack allocation

**Typical cost:** 100-1000 CPU cycles (depends on syscall)

### Optimization Strategies

#### 1. Batch Operations

**Bad:**
```c
for (int i = 0; i < 1000; i++) {
    char c = 'A';
    write(1, &c, 1);  // 1000 syscalls!
}
```

**Good:**
```c
char buf[1000];
for (int i = 0; i < 1000; i++) {
    buf[i] = 'A';
}
write(1, buf, 1000);  // 1 syscall
```

#### 2. Buffer in Userspace

```c
// Buffered output
char output_buf[4096];
int output_len = 0;

void buffered_write(const char* str) {
    int len = strlen(str);
    
    // Flush if buffer full
    if (output_len + len > sizeof(output_buf)) {
        write(1, output_buf, output_len);
        output_len = 0;
    }
    
    // Add to buffer
    memcpy(output_buf + output_len, str, len);
    output_len += len;
}

void flush_buffer(void) {
    if (output_len > 0) {
        write(1, output_buf, output_len);
        output_len = 0;
    }
}
```

#### 3. Use Syscalls Wisely

```c
// Instead of multiple getpid() calls
int pid = getpid();  // Call once
printf("PID: %d\n", pid);
printf("PID: %d\n", pid);
printf("PID: %d\n", pid);
```

---

## Debugging Syscalls

### Enable Kernel Logging

The kernel logs all syscalls. Watch the serial console:

```
[SYSCALL] Dispatching syscall 3 (write)
[SYSCALL]   fd=1, buf=0x400000, count=14
[SYSCALL] Syscall 3 returned 14
```

### Use GDB

Set breakpoints on syscall handlers:

```bash
# In QEMU
make qemu-debug

# In GDB
break sys_write
continue
```

### Trace Syscalls

Add your own tracing:

```c
#include "../libc/stdio.h"

// Wrapper around write
int traced_write(int fd, const void* buf, size_t count) {
    printf("[TRACE] write(fd=%d, count=%d)\n", fd, count);
    int ret = write(fd, buf, count);
    printf("[TRACE] write() returned %d\n", ret);
    return ret;
}
```

---

## Exercises

### Easy

1. Write a program that calls all 5 syscalls
2. Create a program that writes to stdout 100 times (compare single-char vs batched writes)
3. Make a program that forks and prints PIDs of parent and child

### Medium

4. Implement a buffered output library (like the example above)
5. Write a program that benchmarks syscall overhead (use RDTSC)
6. Create error-handling wrappers for all syscalls

### Hard

7. Write a syscall tracer (intercept all syscalls and log them)
8. Implement `printf()` without using the libc version
9. Create a program that demonstrates all error conditions

---

## Summary

You've learned:

✅ What system calls are and why they exist  
✅ The x86_64 `syscall` instruction and calling convention  
✅ All 5 AutomationOS syscalls in detail  
✅ How to make syscalls from C and assembly  
✅ Error handling strategies  
✅ Performance considerations  
✅ Debugging techniques  

**Key Concepts:**

- **Syscalls bridge user and kernel space** with controlled privilege elevation
- **Register convention** defines how arguments are passed
- **Syscalls are expensive** - minimize and batch when possible
- **Error codes** are negative integers
- **Mode switches** happen via `syscall` (user→kernel) and `sysret` (kernel→user)

---

## Next Steps

Ready for more?

1. **Tutorial 4: Debugging** - Debug programs with GDB
2. **Tutorial 5: Adding a Syscall** - Add your own syscall to the kernel
3. **Tutorial 6: Writing a Driver** - Implement a device driver

---

## Resources

- [API Reference](../API_REFERENCE.md) - Complete syscall documentation
- [kernel/core/syscall/](../../kernel/core/syscall/) - Syscall implementation
- [userspace/libc/syscall.c](../../userspace/libc/syscall.c) - Syscall wrappers
- [Architecture Guide](../ARCHITECTURE.md) - System design details

---

**Next Tutorial:** [04_DEBUGGING.md](04_DEBUGGING.md) - Debugging with GDB

---

*Last Updated: 2026-05-26*
