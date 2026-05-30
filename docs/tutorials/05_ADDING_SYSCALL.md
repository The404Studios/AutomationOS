# Tutorial 5: Adding a New System Call

**Difficulty:** Intermediate  
**Time:** 45-60 minutes  
**Prerequisites:** Tutorials 1-4  

---

## Introduction

Now it's time to extend the kernel! In this tutorial, you'll add a brand new system call to AutomationOS.

You'll learn:

- The anatomy of a system call
- How to register syscalls in the kernel
- Creating userspace wrappers
- Testing and debugging new syscalls
- Best practices for syscall design

By the end, you'll have added a custom `sys_uptime()` syscall that returns how long the system has been running.

---

## What We'll Build

### The Syscall: `uptime()`

**Purpose:** Return the number of timer ticks since boot.

**Signature:** `uint64_t uptime(void)`

**Returns:** Number of ticks (at 100Hz, so ticks/100 = seconds)

**Use Cases:**
- Benchmarking
- Timestamps
- Uptime display
- Performance monitoring

---

## Step 1: Understand the Syscall Flow

Let's trace how a syscall works from start to finish:

```
┌─────────────────────────────────────────────────┐
│           Userspace Program                     │
│                                                 │
│   uint64_t ticks = uptime();  // Call wrapper  │
└──────────────────┬──────────────────────────────┘
                   │
                   │ 1. Set RAX = SYS_UPTIME (5)
                   │ 2. Execute syscall instruction
                   ↓
┌─────────────────────────────────────────────────┐
│      kernel/arch/x86_64/syscall.asm             │
│                                                 │
│   syscall_entry:  // CPU jumps here            │
│       - Save user registers                     │
│       - Switch to kernel stack                  │
│       - Call syscall_dispatch()                 │
└──────────────────┬──────────────────────────────┘
                   │
                   │ 3. Dispatch to handler
                   ↓
┌─────────────────────────────────────────────────┐
│      kernel/core/syscall/syscall.c              │
│                                                 │
│   syscall_dispatch(5, ...)                     │
│       - Validate syscall number                 │
│       - Look up handler in table                │
│       - Call: syscall_table[5](...)             │
└──────────────────┬──────────────────────────────┘
                   │
                   │ 4. Execute handler
                   ↓
┌─────────────────────────────────────────────────┐
│      kernel/core/syscall/handlers.c             │
│                                                 │
│   sys_uptime()                                  │
│       - Read timer_ticks global                 │
│       - Return value                            │
└──────────────────┬──────────────────────────────┘
                   │
                   │ 5. Return via syscall_entry
                   │    - Restore user registers
                   │    - Execute sysret
                   ↓
┌─────────────────────────────────────────────────┐
│           Userspace Program                     │
│                                                 │
│   ticks = <return value>  // RAX               │
│   printf("Uptime: %llu\n", ticks);             │
└─────────────────────────────────────────────────┘
```

We need to modify **4 files**:

1. `kernel/include/syscall.h` - Add syscall number
2. `kernel/core/syscall/handlers.c` - Implement handler
3. `kernel/core/syscall/syscall.c` - Register handler
4. `userspace/libc/syscall.h` - Add userspace wrapper

---

## Step 2: Add Syscall Number

Open the syscall header:

```bash
cd ~/Desktop/AutomationOS
nano kernel/include/syscall.h
```

Find the syscall numbers section:

```c
// System call numbers
#define SYS_EXIT    0
#define SYS_FORK    1
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_GETPID  4
```

Add your new syscall:

```c
// System call numbers
#define SYS_EXIT    0
#define SYS_FORK    1
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_GETPID  4
#define SYS_UPTIME  5    // Add this line
```

Now find the handler declarations:

```c
// System call handlers
int64_t sys_exit(uint64_t status, ...);
int64_t sys_fork(uint64_t arg1, ...);
int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count, ...);
int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count, ...);
int64_t sys_getpid(uint64_t arg1, ...);
```

Add your handler declaration:

```c
// System call handlers
int64_t sys_exit(uint64_t status, ...);
int64_t sys_fork(uint64_t arg1, ...);
int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count, ...);
int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count, ...);
int64_t sys_getpid(uint64_t arg1, ...);
int64_t sys_uptime(uint64_t arg1, ...);  // Add this line
```

Save and exit.

---

## Step 3: Implement the Handler

Open the handlers file:

```bash
nano kernel/core/syscall/handlers.c
```

Add your implementation at the end:

```c
// sys_uptime() - Get system uptime in ticks
int64_t sys_uptime(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    // Unused parameters
    (void)arg1; (void)arg2; (void)arg3;
    (void)arg4; (void)arg5; (void)arg6;
    
    // timer_ticks is incremented by PIT interrupt handler
    // Declared as: extern volatile uint64_t timer_ticks;
    extern volatile uint64_t timer_ticks;
    
    kprintf("[SYSCALL] sys_uptime() called, ticks=%llu\n", timer_ticks);
    
    return (int64_t)timer_ticks;
}
```

### Understanding the Code

- **Signature:** All syscall handlers take 6 `uint64_t` arguments (even if unused)
- **Return:** `int64_t` (negative for errors, non-negative for success)
- **timer_ticks:** Global variable incremented by timer interrupt (in `kernel/drivers/timer.c`)
- **Logging:** `kprintf()` helps debugging

Save and exit.

---

## Step 4: Register the Handler

Open the syscall dispatcher:

```bash
nano kernel/core/syscall/syscall.c
```

Find the `syscall_init()` function:

```c
void syscall_init(void) {
    kprintf("[SYSCALL] Initializing system call interface...\n");

    // Clear syscall table
    for (int i = 0; i < MAX_SYSCALLS; i++) {
        syscall_table[i] = NULL;
    }

    // Register syscall handlers
    syscall_table[SYS_EXIT] = sys_exit;
    syscall_table[SYS_FORK] = sys_fork;
    syscall_table[SYS_READ] = sys_read;
    syscall_table[SYS_WRITE] = sys_write;
    syscall_table[SYS_GETPID] = sys_getpid;

    kprintf("[SYSCALL] Registered %d syscalls\n", 5);
    kprintf("[SYSCALL] System call interface initialized\n");
}
```

Add your handler registration:

```c
void syscall_init(void) {
    kprintf("[SYSCALL] Initializing system call interface...\n");

    // Clear syscall table
    for (int i = 0; i < MAX_SYSCALLS; i++) {
        syscall_table[i] = NULL;
    }

    // Register syscall handlers
    syscall_table[SYS_EXIT] = sys_exit;
    syscall_table[SYS_FORK] = sys_fork;
    syscall_table[SYS_READ] = sys_read;
    syscall_table[SYS_WRITE] = sys_write;
    syscall_table[SYS_GETPID] = sys_getpid;
    syscall_table[SYS_UPTIME] = sys_uptime;  // Add this line

    kprintf("[SYSCALL] Registered %d syscalls\n", 6);  // Change 5 → 6
    kprintf("[SYSCALL] System call interface initialized\n");
}
```

Save and exit.

---

## Step 5: Add Userspace Wrapper

Now we need to expose this syscall to userspace programs.

Open the userspace syscall header:

```bash
nano userspace/libc/syscall.h
```

Add the syscall number (must match kernel):

```c
// System call numbers
#define SYS_EXIT    0
#define SYS_FORK    1
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_GETPID  4
#define SYS_UPTIME  5    // Add this line
```

Add the wrapper declaration:

```c
// System call wrappers
void exit(int status);
int fork(void);
int read(int fd, void* buf, size_t count);
int write(int fd, const void* buf, size_t count);
int getpid(void);
uint64_t uptime(void);  // Add this line
```

Add the necessary includes at the top if not already there:

```c
#include <stdint.h>
#include <stddef.h>
```

Save and exit.

Now implement the wrapper:

```bash
nano userspace/libc/syscall.c
```

Add at the end:

```c
// uptime() - Get system uptime in ticks
uint64_t uptime(void) {
    uint64_t ret;
    
    __asm__ volatile (
        "syscall"
        : "=a"(ret)                // Output: RAX
        : "a"(SYS_UPTIME)          // Input: RAX = 5
        : "rcx", "r11", "memory"   // Clobbered
    );
    
    return ret;
}
```

Save and exit.

---

## Step 6: Build and Test

Rebuild the kernel:

```bash
cd ~/Desktop/AutomationOS
make clean
make all
```

Watch for errors. If all goes well, you should see:

```
[KERNEL] Building kernel...
[SYSCALL] Compiled handlers.c
[SYSCALL] Compiled syscall.c
...
[USERSPACE] Building libc...
[USERSPACE] Compiled syscall.c
✓ Build complete!
```

---

## Step 7: Create a Test Program

Let's write a userspace program to test our new syscall.

```bash
nano userspace/bin/test_uptime.c
```

Enter:

```c
#include "../libc/stdio.h"
#include "../libc/syscall.h"

void _start(void) {
    printf("=================================\n");
    printf("  System Uptime Test\n");
    printf("=================================\n\n");
    
    // Get uptime
    uint64_t ticks = uptime();
    
    printf("Uptime: %llu ticks\n", ticks);
    
    // Convert to seconds (100Hz timer)
    uint64_t seconds = ticks / 100;
    uint64_t minutes = seconds / 60;
    uint64_t hours = minutes / 60;
    
    printf("Uptime: %llu seconds\n", seconds);
    printf("Uptime: %llu:%02llu:%02llu (H:M:S)\n",
           hours, minutes % 60, seconds % 60);
    
    printf("\nWaiting 5 seconds...\n");
    
    // Busy wait (no sleep() yet in Phase 1)
    uint64_t start = uptime();
    while (uptime() - start < 500) {  // 500 ticks = 5 seconds
        // Wait
    }
    
    printf("\nAfter wait:\n");
    uint64_t ticks2 = uptime();
    printf("Uptime: %llu ticks\n", ticks2);
    printf("Elapsed: %llu ticks (%llu seconds)\n",
           ticks2 - ticks, (ticks2 - ticks) / 100);
    
    printf("\n=================================\n");
    printf("  Test complete!\n");
    printf("=================================\n\n");
    
    exit(0);
}
```

Create a Makefile:

```bash
nano userspace/bin/Makefile
```

```makefile
CC = x86_64-elf-gcc
CFLAGS = -ffreestanding -nostdlib -fno-builtin -mno-red-zone -Wall -Wextra -std=gnu11
LDFLAGS = -nostdlib -static

LIBC = ../libc/libc.a
BUILD_DIR = ../../build/userspace/bin

TARGETS = test_uptime

all: $(TARGETS)

test_uptime: test_uptime.c $(LIBC)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/$@ $< $(LIBC)
	@echo "Built: $(BUILD_DIR)/$@"

clean:
	rm -f $(BUILD_DIR)/$(TARGETS)

.PHONY: all clean
```

Build the test program:

```bash
make -C userspace/bin
```

---

## Step 8: Integrate with Shell

For now, we'll add a built-in command to test our syscall.

```bash
nano userspace/shell/shell.c
```

Add to the built-ins array:

```c
static builtin_t builtins[] = {
    {"echo", cmd_echo},
    {"help", cmd_help},
    {"exit", cmd_exit},
    {"clear", cmd_clear},
    {"pid", cmd_pid},
    {"uptime", cmd_uptime},  // Add this
    {NULL, NULL}
};
```

Implement the command:

```c
// uptime command - Show system uptime
static int cmd_uptime(int argc, char** argv) {
    (void)argc;
    (void)argv;
    
    uint64_t ticks = uptime();
    uint64_t seconds = ticks / 100;
    uint64_t minutes = seconds / 60;
    uint64_t hours = minutes / 60;
    
    printf("\nSystem Uptime:\n");
    printf("  Ticks:   %llu\n", ticks);
    printf("  Seconds: %llu\n", seconds);
    printf("  Time:    %llu:%02llu:%02llu (H:M:S)\n\n",
           hours, minutes % 60, seconds % 60);
    
    return 0;
}
```

Add the include at the top if not present:

```c
#include <stdint.h>
```

---

## Step 9: Test Your Syscall!

Rebuild everything:

```bash
cd ~/Desktop/AutomationOS
make clean
make all
make qemu
```

In the shell, test your new command:

```bash
aos> uptime
```

You should see:

```
System Uptime:
  Ticks:   234
  Seconds: 2
  Time:    0:00:02 (H:M:S)
```

Wait a bit and run again:

```bash
aos> uptime

System Uptime:
  Ticks:   1543
  Seconds: 15
  Time:    0:00:15 (H:M:S)
```

**Congratulations!** You've added a new system call!

---

## Step 10: Debug with GDB

Let's trace your syscall in action.

Start QEMU in debug mode:

```bash
make qemu-debug
```

In another terminal:

```bash
gdb build/kernel.elf
target remote :1234

# Set breakpoint at your handler
break sys_uptime

# Continue
continue
```

In the shell, run `uptime`. GDB will break:

```
Breakpoint 1, sys_uptime () at kernel/core/syscall/handlers.c:89
89          extern volatile uint64_t timer_ticks;
```

Inspect:

```gdb
# Step through
next
next

# Print timer_ticks
print timer_ticks

# View call stack
backtrace

# Continue
continue
```

---

## Step 11: Add More Features

### Add Uptime Format Options

Modify the handler to support different formats:

```c
// New syscall: sys_uptime_format(format)
// format: 0 = ticks, 1 = seconds, 2 = string
int64_t sys_uptime_format(uint64_t format, uint64_t arg2, uint64_t arg3,
                          uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    
    extern volatile uint64_t timer_ticks;
    
    switch (format) {
        case 0:  // Ticks
            return (int64_t)timer_ticks;
        
        case 1:  // Seconds
            return (int64_t)(timer_ticks / 100);
        
        case 2:  // Minutes
            return (int64_t)(timer_ticks / 6000);
        
        default:
            return -1;  // Invalid format
    }
}
```

### Add High-Precision Timer

Use `rdtsc` (CPU cycle counter) for nanosecond precision:

```c
int64_t sys_rdtsc(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg1; (void)arg2; (void)arg3;
    (void)arg4; (void)arg5; (void)arg6;
    
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    
    return ((uint64_t)hi << 32) | lo;
}
```

Userspace wrapper:

```c
uint64_t rdtsc(void) {
    uint64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(SYS_RDTSC)  // Assign a syscall number
        : "rcx", "r11", "memory"
    );
    return ret;
}
```

---

## Best Practices

### 1. Validate All Input

```c
int64_t sys_uptime_format(uint64_t format, ...) {
    // Validate format
    if (format > 2) {
        kprintf("[SYSCALL] Invalid format: %llu\n", format);
        return -EINVAL;  // Invalid argument
    }
    
    // ... rest of code
}
```

### 2. Check Pointers from Userspace

```c
int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count, ...) {
    // Validate pointer is in userspace
    if (buf >= 0xFFFF800000000000) {
        kprintf("[SYSCALL] Invalid buffer pointer: %p\n", (void*)buf);
        return -EINVAL;
    }
    
    // ... rest of code
}
```

### 3. Use Helper Functions

```c
// Helper to validate user pointer
static bool is_user_pointer(void* ptr) {
    return (uint64_t)ptr < 0xFFFF800000000000;
}

int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count, ...) {
    if (!is_user_pointer((void*)buf)) {
        return -EINVAL;
    }
    // ... rest of code
}
```

### 4. Document Your Syscall

Add comprehensive documentation:

```c
/**
 * sys_uptime() - Get system uptime in timer ticks
 *
 * Returns the number of timer ticks since system boot.
 * The timer runs at 100Hz, so divide by 100 to get seconds.
 *
 * Returns:
 *   Number of ticks (uint64_t, always >= 0)
 *
 * Errors:
 *   None (this syscall cannot fail)
 *
 * Notes:
 *   - Wraps around after ~58 million years at 100Hz
 *   - Timer frequency can be changed at compile time
 */
int64_t sys_uptime(...) {
    // ...
}
```

### 5. Test Thoroughly

Create comprehensive tests:

```c
void test_uptime(void) {
    // Test basic functionality
    uint64_t t1 = uptime();
    assert(t1 > 0);
    
    // Test monotonic increase
    uint64_t t2 = uptime();
    assert(t2 >= t1);
    
    // Test timing accuracy
    uint64_t start = uptime();
    busy_wait_ms(1000);  // Wait 1 second
    uint64_t end = uptime();
    assert(end - start >= 95 && end - start <= 105);  // Within 5%
}
```

---

## Common Issues

### Syscall returns 0 or garbage

**Problem:** Handler not registered.

**Solution:** Check `syscall_init()` includes your handler.

### Segfault in kernel

**Problem:** Invalid memory access in handler.

**Solution:** Use GDB to debug. Check all pointers.

### Wrong return value

**Problem:** Userspace wrapper has wrong inline assembly.

**Solution:** Verify register constraints match ABI.

### Syscall number mismatch

**Problem:** Kernel and userspace have different numbers.

**Solution:** Ensure both use same `#define SYS_UPTIME 5`.

---

## Summary

You've learned:

✅ How to add a new syscall from scratch  
✅ The four files that need changes  
✅ How to implement kernel handlers  
✅ How to create userspace wrappers  
✅ Testing and debugging new syscalls  
✅ Best practices for syscall design  

**Key Concepts:**

- **Syscall numbers** must match in kernel and userspace
- **Handlers** take 6 uint64_t args, return int64_t
- **Registration** happens in `syscall_init()`
- **Wrappers** use inline assembly with `syscall` instruction
- **Validation** is critical for security

---

## Exercises

### Easy

1. Add `sys_getppid()` - return parent process ID
2. Add `sys_gettimeofday()` - return current time
3. Add `sys_sleep_ticks(n)` - sleep for n ticks

### Medium

4. Add `sys_process_count()` - return number of active processes
5. Add `sys_mem_free()` - return free memory in bytes
6. Add `sys_cpu_info()` - return CPU model/vendor

### Hard

7. Add `sys_send()` and `sys_recv()` for inter-process communication
8. Add `sys_mmap()` - memory mapping syscall
9. Add `sys_signal()` - basic signal handling

---

## Next Steps

Ready for more kernel hacking?

1. **Tutorial 6: Writing a Driver** - Implement a device driver
2. **Tutorial 7: Memory Management** - Deep dive into PMM/VMM
3. **Tutorial 8: Process Scheduling** - Understanding the scheduler

---

## Resources

- [API Reference](../API_REFERENCE.md) - All syscalls documented
- [Linux Syscall Reference](https://man7.org/linux/man-pages/man2/syscalls.2.html) - Inspiration
- [kernel/core/syscall/](../../kernel/core/syscall/) - Syscall implementation
- [Development Guide](../DEVELOPMENT_GUIDE.md) - Advanced topics

---

**Next Tutorial:** [06_WRITING_DRIVER.md](06_WRITING_DRIVER.md) - Write a device driver

---

*Last Updated: 2026-05-26*
