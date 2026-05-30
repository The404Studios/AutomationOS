# AutomationOS Development Guide

**Version:** 0.1.0  
**Phase:** 1 - Core Foundation  
**Last Updated:** 2026-05-26

---

## Table of Contents

1. [Getting Started](#getting-started)
2. [Development Environment Setup](#development-environment-setup)
3. [Code Structure](#code-structure)
4. [Adding New Features](#adding-new-features)
5. [Writing Drivers](#writing-drivers)
6. [Testing Guidelines](#testing-guidelines)
7. [Debugging](#debugging)
8. [Code Style](#code-style)
9. [Contributing](#contributing)
10. [Common Development Tasks](#common-development-tasks)

---

## Getting Started

### Prerequisites

Before developing for AutomationOS, ensure you have:

1. Built the system successfully (see [BUILD_GUIDE.md](BUILD_GUIDE.md))
2. Read the architecture overview ([ARCHITECTURE.md](ARCHITECTURE.md))
3. Familiarized yourself with the API ([API_REFERENCE.md](API_REFERENCE.md))

### Development Workflow

```
┌─────────────────┐
│  Read Docs      │
│  & Understand   │
│  Architecture   │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Design         │
│  Feature/Fix    │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Write Tests    │  (Test-Driven Development)
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Implement      │
│  Code           │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Test in QEMU   │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Debug Issues   │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Code Review    │
│  & Submit PR    │
└─────────────────┘
```

---

## Development Environment Setup

### Editor Configuration

#### Visual Studio Code

Recommended extensions:
- C/C++ (Microsoft)
- x86 and x86_64 Assembly
- Makefile Tools
- GitLens

**Settings (`.vscode/settings.json`):**
```json
{
    "C_Cpp.default.includePath": [
        "${workspaceFolder}/kernel/include"
    ],
    "C_Cpp.default.defines": [
        "__KERNEL__",
        "__x86_64__"
    ],
    "C_Cpp.default.compilerPath": "/usr/local/cross/bin/x86_64-elf-gcc",
    "files.associations": {
        "*.asm": "asm-intel-x86-generic",
        "*.h": "c",
        "*.c": "c"
    }
}
```

**Build task (`.vscode/tasks.json`):**
```json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "Build AutomationOS",
            "type": "shell",
            "command": "make all",
            "group": {
                "kind": "build",
                "isDefault": true
            }
        },
        {
            "label": "Run in QEMU",
            "type": "shell",
            "command": "make qemu"
        }
    ]
}
```

#### Vim/Neovim

Add to `.vimrc`:
```vim
" Set include path for kernel headers
set path+=kernel/include

" Use 4-space indentation
set tabstop=4 shiftwidth=4 expandtab

" Syntax highlighting
syntax on
filetype plugin indent on
```

### GDB Setup

Create `.gdbinit` in project root:

```gdb
# Connect to QEMU GDB server
target remote localhost:1234

# Load kernel symbols
file build/kernel.elf

# Useful breakpoints
break kernel_main
break kernel_panic

# Custom commands
define reload
    file build/kernel.elf
    symbol-file build/kernel.elf
end

# Display settings
set disassembly-flavor intel
layout src
```

### Serial Console Setup

AutomationOS outputs debug logs to COM1 (serial port).

**Linux:**
```bash
# In one terminal
make qemu

# QEMU automatically redirects serial to stdout
```

**macOS:**
```bash
# Serial output shown in terminal
make qemu
```

---

## Code Structure

### Source Organization

```
kernel/
├── arch/           # Architecture-specific code
│   └── x86_64/
│       ├── *.asm   # Low-level assembly
│       └── *.c     # x86_64-specific C code
│
├── core/           # Core kernel subsystems
│   ├── mem/        # Memory management
│   ├── sched/      # Process scheduling
│   └── syscall/    # System calls
│
├── drivers/        # Device drivers
│   ├── serial.c
│   ├── ps2.c
│   └── framebuffer.c
│
├── lib/            # Kernel library functions
│   ├── string.c
│   ├── printf.c
│   └── panic.c
│
├── include/        # Public kernel headers
│   ├── kernel.h
│   ├── mem.h
│   ├── sched.h
│   └── ...
│
└── kernel.c        # Kernel main entry
```

### Header Dependencies

```
types.h              (base types)
    ↓
kernel.h             (core definitions)
    ↓
mem.h, sched.h,      (subsystem APIs)
syscall.h, drivers.h
    ↓
x86_64.h             (arch-specific)
```

**Rule:** Headers should include only what they directly need.

### Naming Conventions

| Entity | Convention | Example |
|--------|-----------|---------|
| Functions | `subsystem_action()` | `pmm_alloc_page()` |
| Structs | `name_t` | `process_t` |
| Enums | `NAME_VALUE` | `PROCESS_RUNNING` |
| Macros | `UPPER_CASE` | `PAGE_SIZE` |
| Constants | `UPPER_CASE` | `MAX_PROCESSES` |
| Global vars | `subsystem_name` | `current_process` |
| Local vars | `snake_case` | `page_count` |

---

## Adding New Features

### Example: Adding a New System Call

Let's add `sys_time()` to get system uptime.

#### Step 1: Define System Call Number

Edit `kernel/include/syscall.h`:

```c
#define SYS_TIME   10  // New syscall
```

#### Step 2: Declare Handler

Add to `kernel/include/syscall.h`:

```c
int64_t sys_time(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6);
```

#### Step 3: Implement Handler

Create or edit `kernel/core/syscall/handlers.c`:

```c
#include "../../include/syscall.h"
#include "../../include/drivers.h"

int64_t sys_time(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg1; (void)arg2; (void)arg3;
    (void)arg4; (void)arg5; (void)arg6;
    
    // Get ticks and convert to seconds
    uint64_t ticks = timer_get_ticks();
    uint64_t frequency = timer_get_frequency();
    return (int64_t)(ticks / frequency);
}
```

#### Step 4: Register Handler

Edit `kernel/core/syscall/syscall.c`:

```c
void syscall_init(void) {
    // ... existing handlers ...
    syscall_handlers[SYS_TIME] = sys_time;
}
```

#### Step 5: Add Userspace Wrapper

Edit `userspace/libc/syscall.c`:

```c
time_t time(void) {
    return (time_t)syscall(SYS_TIME, 0, 0, 0, 0, 0);
}
```

Add declaration to `userspace/libc/syscall.h`:

```c
time_t time(void);
```

#### Step 6: Write Test

Create `tests/unit/test_time.c`:

```c
#include <syscall.h>
#include <stdio.h>

int main(void) {
    time_t t1 = time();
    sleep(1000);  // Sleep 1 second
    time_t t2 = time();
    
    if (t2 > t1) {
        printf("PASS: time() works\n");
        return 0;
    } else {
        printf("FAIL: time() not increasing\n");
        return 1;
    }
}
```

#### Step 7: Build and Test

```bash
make kernel
make userspace
make qemu

# In QEMU shell
/test/test_time
```

---

## Writing Drivers

### Driver Template

```c
#include "../include/drivers.h"
#include "../include/kernel.h"
#include "../include/x86_64.h"

// Driver state
static struct {
    bool initialized;
    uint16_t io_base;
    // ... driver-specific state
} driver_state;

// Initialize driver
void mydriver_init(void) {
    kprintf("[MYDRIVER] Initializing...\n");
    
    // Setup I/O ports, interrupts, etc.
    driver_state.io_base = 0x300;
    
    // Test hardware presence
    uint8_t status = inb(driver_state.io_base);
    if (status == 0xFF) {
        kprintf("[MYDRIVER] Hardware not detected\n");
        return;
    }
    
    // Configure hardware
    outb(driver_state.io_base, 0x01);  // Enable
    
    driver_state.initialized = true;
    kprintf("[MYDRIVER] Initialized successfully\n");
}

// Driver operations
int mydriver_read(void* buffer, size_t size) {
    if (!driver_state.initialized) {
        return -ENODEV;
    }
    
    // Implement read logic
    for (size_t i = 0; i < size; i++) {
        ((uint8_t*)buffer)[i] = inb(driver_state.io_base);
    }
    
    return size;
}

int mydriver_write(const void* buffer, size_t size) {
    if (!driver_state.initialized) {
        return -ENODEV;
    }
    
    // Implement write logic
    for (size_t i = 0; i < size; i++) {
        outb(driver_state.io_base, ((const uint8_t*)buffer)[i]);
    }
    
    return size;
}
```

### Interrupt-Driven Driver

```c
#include "../include/drivers.h"
#include "../include/x86_64.h"

#define IRQ_NUM 5
#define BUFFER_SIZE 256

static struct {
    char buffer[BUFFER_SIZE];
    size_t read_pos;
    size_t write_pos;
} driver_buffer;

// Interrupt handler
void mydriver_irq_handler(void) {
    // Read data from hardware
    uint8_t data = inb(0x300);
    
    // Store in ring buffer
    size_t next_pos = (driver_buffer.write_pos + 1) % BUFFER_SIZE;
    if (next_pos != driver_buffer.read_pos) {
        driver_buffer.buffer[driver_buffer.write_pos] = data;
        driver_buffer.write_pos = next_pos;
    }
    
    // Send EOI to PIC
    outb(0x20, 0x20);
}

void mydriver_init(void) {
    // Register interrupt handler
    idt_register_handler(IRQ_NUM + 32, mydriver_irq_handler);
    
    // Enable IRQ
    // ... PIC configuration ...
}

char mydriver_getchar(void) {
    // Wait for data
    while (driver_buffer.read_pos == driver_buffer.write_pos) {
        hlt();  // Wait for interrupt
    }
    
    // Read from buffer
    char c = driver_buffer.buffer[driver_buffer.read_pos];
    driver_buffer.read_pos = (driver_buffer.read_pos + 1) % BUFFER_SIZE;
    return c;
}
```

### Driver Best Practices

1. **Check hardware presence** in init function
2. **Return error codes** on failure (don't panic unless critical)
3. **Use ring buffers** for interrupt-driven I/O
4. **Log initialization** with `kprintf()`
5. **Document I/O ports** and registers used
6. **Handle edge cases** (buffer full, device not ready, etc.)
7. **Test on real hardware** when possible

---

## Testing Guidelines

### Test-Driven Development

Write tests BEFORE implementing features:

```c
// tests/unit/test_feature.c
#include "../../kernel/include/feature.h"

void test_basic_operation(void) {
    int result = feature_operation();
    ASSERT(result == EXPECTED_VALUE);
}

void test_edge_case(void) {
    int result = feature_operation_with_null();
    ASSERT(result == ERROR_CODE);
}

int main(void) {
    test_basic_operation();
    test_edge_case();
    kprintf("All tests passed\n");
    return 0;
}
```

### Integration Testing

Test complete workflows:

```python
# tests/integration/test_feature.py
import subprocess
import sys

def test_feature_workflow():
    result = subprocess.run(
        ['make', 'qemu', 'TEST_FEATURE=1'],
        capture_output=True,
        timeout=30
    )
    
    output = result.stdout.decode()
    assert 'Feature initialized' in output
    assert 'Test passed' in output
    
if __name__ == '__main__':
    test_feature_workflow()
```

### Manual Testing Checklist

For each feature, test:

- [ ] Basic functionality works
- [ ] Edge cases handled (NULL pointers, zero sizes, etc.)
- [ ] Error conditions return proper codes
- [ ] No memory leaks (check with memory stats)
- [ ] No kernel panics
- [ ] Works with interrupts enabled
- [ ] Works in QEMU and on real hardware (if applicable)

---

## Debugging

### Debugging Techniques

#### 1. Serial Logging

```c
kprintf("[DEBUG] Entering function: %s\n", __func__);
kprintf("[DEBUG] Variable x = %d\n", x);
kprintf("[DEBUG] Pointer p = %p\n", p);
```

#### 2. GDB Debugging

```bash
# Terminal 1: Start QEMU with GDB server
make qemu-debug

# Terminal 2: Attach GDB
gdb build/kernel.elf
(gdb) target remote localhost:1234
(gdb) break kernel_main
(gdb) continue
(gdb) print variable_name
(gdb) backtrace
```

**Useful GDB Commands:**

| Command | Description |
|---------|-------------|
| `break function` | Set breakpoint |
| `continue` | Resume execution |
| `step` | Step into function |
| `next` | Step over function |
| `print var` | Print variable |
| `x/16x address` | Examine memory (hex) |
| `info registers` | Show all registers |
| `backtrace` | Show call stack |
| `disassemble` | Show assembly |

#### 3. Assertions

```c
ASSERT(ptr != NULL);
ASSERT(size > 0 && size < MAX_SIZE);
ASSERT(process->state == PROCESS_RUNNING);
```

#### 4. Memory Debugging

```c
// Track allocations
void* ptr = kmalloc(size);
kprintf("[MEM] Allocated %zu bytes at %p\n", size, ptr);

// Check memory stats
uint64_t free = pmm_get_free_memory();
kprintf("[MEM] Free memory: %llu bytes\n", free);
```

### Common Debugging Scenarios

#### Kernel Panic

1. Note the panic message and location
2. Check the call stack with GDB
3. Inspect variables leading to the panic
4. Look for NULL dereferences, buffer overflows

**Example:**
```
[PANIC] Page fault at 0x0000000000000000
RIP: 0xFFFFFFFF80123456
```

Solution:
```bash
(gdb) list *0xFFFFFFFF80123456
# Shows source code location
```

#### Triple Fault

**Symptoms:** QEMU restarts immediately

**Causes:**
- Stack overflow
- Invalid page tables
- Exception during exception handling

**Debug:**
```bash
# Enable QEMU logging
qemu-system-x86_64 -d int,cpu_reset -no-reboot ...
```

#### Hanging System

**Symptoms:** No output, system stops responding

**Causes:**
- Infinite loop
- Deadlock
- Waiting for interrupt that never comes

**Debug:**
```bash
# Attach GDB while hanging
(gdb) interrupt
(gdb) backtrace
(gdb) info threads
```

#### Memory Corruption

**Symptoms:** Random crashes, data corruption

**Causes:**
- Buffer overflow
- Use-after-free
- Double free

**Debug:**
1. Add bounds checking
2. Fill freed memory with pattern (0xDEADBEEF)
3. Use memory debugging tools (Phase 2+)

---

## Code Style

### C Code Style

```c
// Function documentation
/**
 * Allocate a physical page.
 * 
 * @return Physical address of page, or NULL on failure
 */
void* pmm_alloc_page(void) {
    // Implementation
}

// Braces on same line for functions
void function(void) {
    if (condition) {
        // Code
    } else {
        // Code
    }
    
    // For single-line statements, braces optional but recommended
    if (error)
        return ERROR_CODE;
}

// Spaces around operators
int x = a + b;
ptr = kmalloc(size);

// No spaces inside parentheses
function(arg1, arg2);

// Line length: aim for 80-100 characters
```

### Assembly Style

```nasm
; Function comment
; Inputs: rdi = parameter
; Outputs: rax = return value
global my_function
my_function:
    push rbp
    mov rbp, rsp
    
    ; Save registers
    push rbx
    push r12
    
    ; Function body
    mov rax, rdi
    add rax, 1
    
    ; Restore registers
    pop r12
    pop rbx
    
    mov rsp, rbp
    pop rbp
    ret
```

### Commit Messages

```
component: Brief description (50 chars or less)

Longer explanation of what and why (not how).
Wrap at 72 characters.

- Bullet points for multiple changes
- Use present tense ("Add feature" not "Added feature")

Fixes #123
```

**Examples:**
```
pmm: Fix buddy allocator coalescing bug

The buddy allocator was not correctly coalescing free blocks
when both buddies were free. This led to fragmentation over time.

scheduler: Implement round-robin time slicing

- Add time_slice field to process_t
- Decrement slice on each scheduler tick
- Switch process when slice expires

Closes #45
```

---

## Contributing

### Contribution Workflow

1. **Fork the repository**
2. **Create a feature branch:** `git checkout -b feature/my-feature`
3. **Make changes and commit:** Follow commit message guidelines
4. **Write tests:** Add unit and integration tests
5. **Test thoroughly:** `make test-full`
6. **Submit pull request:** Include description and test results

### Pull Request Template

```markdown
## Description
Brief description of changes

## Motivation
Why is this change needed?

## Changes
- Bullet list of changes

## Testing
- [ ] Unit tests added/updated
- [ ] Integration tests pass
- [ ] Tested in QEMU
- [ ] Tested on real hardware (if applicable)

## Checklist
- [ ] Code follows style guide
- [ ] Documentation updated
- [ ] No compiler warnings
- [ ] Commit messages are clear
```

---

## Common Development Tasks

### Task 1: Add a New Driver

1. Create `kernel/drivers/mydriver.c`
2. Add function prototypes to `kernel/include/drivers.h`
3. Implement `mydriver_init()` and operations
4. Call `mydriver_init()` from `kernel_main()`
5. Update `kernel/drivers/Makefile`
6. Test with QEMU

### Task 2: Extend Memory Manager

1. Define new function in `kernel/include/mem.h`
2. Implement in `kernel/core/mem/pmm.c` or `vmm.c`
3. Write unit tests in `tests/unit/test_mem.c`
4. Test allocations/deallocations thoroughly

### Task 3: Add Scheduler Feature

1. Update `process_t` structure in `kernel/include/sched.h`
2. Modify scheduler logic in `kernel/core/sched/scheduler.c`
3. Test process switching behavior
4. Verify no deadlocks or starvation

### Task 4: Implement New Userspace Program

1. Create directory: `userspace/myprog/`
2. Write `myprog.c` with `main()`
3. Create `Makefile` (copy from `userspace/init/Makefile`)
4. Update `userspace/Makefile` to include new program
5. Copy binary to ISO in `scripts/build-iso.py`

### Task 5: Fix a Bug

1. Reproduce the bug consistently
2. Write a failing test that exposes the bug
3. Debug with GDB and serial logging
4. Fix the issue
5. Verify test passes
6. Check for similar issues in related code

---

## Resources

### Documentation

- [OSDev Wiki](https://wiki.osdev.org/) - OS development resources
- [Intel SDM](https://software.intel.com/sdm) - x86_64 reference
- [UEFI Specification](https://uefi.org/specifications)

### Tools

- [QEMU](https://www.qemu.org/docs/master/)
- [GDB](https://sourceware.org/gdb/documentation/)
- [NASM](https://www.nasm.us/docs.php)

### Books

- "Operating Systems: Three Easy Pieces" by Remzi and Andrea Arpaci-Dusseau
- "Modern Operating Systems" by Andrew S. Tanenbaum
- "The Art of Unix Programming" by Eric S. Raymond

---

## Getting Help

- **Issues:** Check existing issues on GitHub
- **Discussions:** Join the project discussions
- **Documentation:** Read all documentation in `docs/`
- **Code Comments:** Review in-code comments

---

## Next Steps

1. **Explore the codebase:** Start with `kernel/kernel.c`
2. **Run the tests:** `make test`
3. **Make a small change:** Fix a typo, add a comment
4. **Write a test:** Add a unit test
5. **Implement a feature:** Pick an issue labeled "good first issue"

---

**End of Development Guide**
