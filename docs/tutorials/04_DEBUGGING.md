# Tutorial 4: Debugging with GDB

**Difficulty:** Intermediate  
**Time:** 40-50 minutes  
**Prerequisites:** Tutorials 1-3  

---

## Introduction

Debugging an operating system is different from debugging regular applications. In this tutorial, you'll learn:

- How to debug the AutomationOS kernel with GDB
- Using QEMU's debugging features
- Setting breakpoints in kernel and userspace
- Inspecting memory, registers, and stack
- Debugging techniques specific to OS development
- Common bugs and how to find them

By the end, you'll be able to track down and fix bugs in both kernel and userspace code.

---

## Why OS Debugging is Different

### Challenges

1. **No debugger infrastructure** - You can't use `printf()` everywhere
2. **Crashes are severe** - Kernel panics halt the entire system
3. **Hardware interaction** - Bugs can come from devices, timing, interrupts
4. **Multiple privilege levels** - Kernel (Ring 0) vs userspace (Ring 3)
5. **Concurrency** - Multiple processes, interrupts, race conditions

### Tools at Your Disposal

- **Serial console** - Kernel logging output
- **QEMU monitor** - Inspect virtual hardware state
- **GDB remote debugging** - Full debugger over virtual serial
- **Printf debugging** - Still useful with `kprintf()`
- **Assertions** - Catch bugs early

---

## Setup: QEMU + GDB

### How It Works

QEMU can act as a **GDB stub** - it freezes the VM and lets GDB control execution over TCP.

```
┌─────────────────┐         ┌──────────────────┐
│   GDB Client    │◄───────►│   QEMU (GDB Stub)│
│  (Controller)   │  TCP    │   (AutomationOS)  │
│                 │  :1234  │                   │
└─────────────────┘         └──────────────────┘
```

GDB can:
- Pause/resume execution
- Set breakpoints
- Single-step through code
- Inspect registers and memory
- Call stack analysis

---

## Step 1: Build with Debug Symbols

Debug symbols include source file names, line numbers, and variable names.

### Check Current Flags

```bash
cd ~/Desktop/AutomationOS
cat Makefile | grep CFLAGS
```

You should see `-g` for debug symbols:

```makefile
CFLAGS = -g -ffreestanding -nostdlib -fno-builtin ...
```

If not, add `-g`:

```makefile
CFLAGS = -g -O0 -ffreestanding -nostdlib ...
```

**Note:** `-O0` disables optimization, making debugging easier (but code slower).

### Rebuild with Debug Info

```bash
make clean
make all
```

### Verify Debug Symbols

```bash
x86_64-elf-readelf --debug-dump=info build/kernel.elf | head -50
```

You should see DWARF debug information.

---

## Step 2: Start QEMU in Debug Mode

```bash
make qemu-debug
```

This runs:

```bash
qemu-system-x86_64 \
    -cdrom build/AutomationOS.iso \
    -m 512M \
    -serial stdio \
    -s \        # Listen on TCP :1234 for GDB
    -S          # Freeze at startup, wait for GDB
```

QEMU will start but **not boot** - it's waiting for GDB.

---

## Step 3: Connect GDB

Open a **second terminal**:

```bash
cd ~/Desktop/AutomationOS
gdb build/kernel.elf
```

GDB will load symbols and execute `.gdbinit`:

```
GNU gdb (GDB) 13.2
Reading symbols from build/kernel.elf...
(gdb)
```

### Connect to QEMU

```gdb
target remote :1234
```

You'll see:

```
Remote debugging using :1234
0x000000000000fff0 in ?? ()
```

This is the CPU reset vector (BIOS entry point).

---

## Step 4: Basic GDB Commands

### Essential Commands

| Command | Short | Description |
|---------|-------|-------------|
| `continue` | `c` | Resume execution |
| `next` | `n` | Step over (next line) |
| `step` | `s` | Step into (follow calls) |
| `finish` | `fin` | Run until function returns |
| `break` | `b` | Set breakpoint |
| `info breakpoints` | `i b` | List breakpoints |
| `delete` | `d` | Delete breakpoint |
| `print` | `p` | Print variable/expression |
| `x` | - | Examine memory |
| `info registers` | `i r` | Show all registers |
| `backtrace` | `bt` | Show call stack |

### Set a Breakpoint

```gdb
break kernel_main
```

This sets a breakpoint at the `kernel_main()` function.

### Continue Execution

```gdb
continue
```

The OS will boot and stop at `kernel_main`.

---

## Step 5: Inspect Kernel State

### View Registers

```gdb
info registers
```

Output:

```
rax            0x0                 0
rbx            0x0                 0
rcx            0x0                 0
rdx            0x0                 0
rsi            0xffff800000001000  -140737488289792
rdi            0xffff800000100000  -140737487831040
rsp            0xffff800000200000  0xffff800000200000
rip            0xffff800000001234  0xffff800000001234 <kernel_main+4>
cs             0x8                 8
ss             0x10                16
...
```

### Print Variables

```gdb
print total_memory
```

### View Memory

```gdb
# Examine 16 bytes at address as hex
x/16xb 0xFFFF800000000000

# Examine 4 quad-words (64-bit) at stack pointer
x/4gx $rsp

# Examine as string
x/s 0xFFFF800000100000
```

### View Source Code

```gdb
list
```

Shows the current source lines.

```gdb
list kernel_main
```

Shows `kernel_main()` function.

---

## Step 6: Step Through Code

### Single-Stepping

```gdb
# Continue to kernel_main
break kernel_main
continue

# Step line by line
next
next
next
```

Use `n` or `next` to execute one line at a time, stepping **over** function calls.

### Stepping Into Functions

```gdb
break kernel_main
continue

# Step into the next function call
step
```

Use `s` or `step` to follow execution **into** function calls.

### Example Session

```gdb
(gdb) break kernel_main
Breakpoint 1 at 0xffffffff80001234: file kernel/main.c, line 42.

(gdb) continue
Continuing.

Breakpoint 1, kernel_main () at kernel/main.c:42
42          kprintf("[KERNEL] AutomationOS kernel starting...\n");

(gdb) next
43          gdt_init();

(gdb) next
44          pmm_init(boot_info->memory_map, boot_info->memory_map_count);

(gdb) step
pmm_init (mmap=0xffff800000001000, mmap_count=8) at kernel/core/mem/pmm.c:56
56          kprintf("[PMM] Initializing physical memory manager...\n");

(gdb) finish
Run till exit from #0  pmm_init (...) at kernel/core/mem/pmm.c:56

(gdb) next
45          vmm_init();
```

---

## Step 7: Inspect the Stack

### View Backtrace

```gdb
backtrace
```

Or shorter:

```gdb
bt
```

Output:

```
#0  pmm_alloc_page () at kernel/core/mem/pmm.c:120
#1  0xffffffff80002345 in vmm_init () at kernel/core/mem/vmm.c:78
#2  0xffffffff80001456 in kernel_main () at kernel/main.c:44
#3  0xffffffff80000123 in boot_main () at boot/loader.c:234
```

This shows the **call stack** - how you got to the current function.

### Inspect Stack Frames

```gdb
# Move up one frame
up

# Move down one frame
down

# Jump to specific frame
frame 2
```

### Example

```gdb
(gdb) bt
#0  sys_write () at kernel/core/syscall/handlers.c:89
#1  0xffffffff80003456 in syscall_dispatch () at kernel/core/syscall/syscall.c:50
#2  0xffffffff80000789 in syscall_entry () at kernel/arch/x86_64/syscall.asm:45

(gdb) frame 1
#1  0xffffffff80003456 in syscall_dispatch () at kernel/core/syscall/syscall.c:50
50          int64_t result = handler(arg1, arg2, arg3, arg4, arg5, arg6);

(gdb) print syscall_num
$1 = 3

(gdb) print arg1
$2 = 1

(gdb) print arg2
$3 = 0x400000
```

---

## Step 8: Hardware Breakpoints

### Why Hardware Breakpoints?

Software breakpoints replace instructions with `int3`. Hardware breakpoints use CPU debug registers - they work even when code is not in memory or write-protected.

### Set Hardware Breakpoint

```gdb
hbreak pmm_alloc_page
```

### Watch Points

Break when a memory location is **read** or **written**:

```gdb
# Break when variable changes
watch my_variable

# Break when memory address is written
watch *(uint64_t*)0xFFFF800000100000

# Break on read
rwatch my_variable

# Break on read or write
awatch my_variable
```

---

## Step 9: Debug Userspace Programs

### The Challenge

Userspace programs run at different addresses with different symbols.

### Load Userspace Symbols

```gdb
# While kernel is running, load shell symbols
add-symbol-file build/userspace/shell/shell 0x400000
```

The address `0x400000` is where userspace programs are loaded (check your linker script).

### Set Userspace Breakpoint

```gdb
break shell_main
continue
```

Now you can debug shell code!

### Switch Between Kernel and User Symbols

GDB will automatically switch based on the current execution context.

---

## Step 10: Common Debugging Scenarios

### Scenario 1: Kernel Panic

**Symptom:** System crashes with a panic message.

```
[KERNEL PANIC] Page fault at 0x0000000000000000
RIP: 0xFFFFFFFF80001234
```

**Debug:**

```gdb
# Set breakpoint before panic
break kernel_panic

# Or set breakpoint at the faulting instruction
break *0xFFFFFFFF80001234

continue
```

When it breaks:

```gdb
# View registers
info registers

# View stack
bt

# Inspect the faulting address
x/16xg 0x0000000000000000
```

### Scenario 2: Infinite Loop

**Symptom:** System hangs, no output.

**Debug:**

```gdb
# Interrupt execution (Ctrl+C in GDB)
^C

# See where it's stuck
bt
list
```

### Scenario 3: Null Pointer Dereference

**Symptom:** Page fault when accessing address 0x0.

**Debug:**

```gdb
# Break on page fault handler
break page_fault_handler

continue

# When it breaks, check the faulting address
print fault_addr

# Check where the null pointer came from
bt
frame 1
print ptr
```

### Scenario 4: Memory Corruption

**Symptom:** Variables have wrong values, random crashes.

**Debug:**

```gdb
# Use watchpoint to catch the write
watch my_struct->field

continue

# When it breaks, see who modified it
bt
```

### Scenario 5: Syscall Not Working

**Symptom:** Syscall returns error or wrong value.

**Debug:**

```gdb
# Break at syscall dispatcher
break syscall_dispatch

continue

# Check parameters
print syscall_num
print arg1
print arg2

# Step into handler
step
```

---

## Step 11: Advanced Techniques

### Conditional Breakpoints

```gdb
# Break only when condition is true
break pmm_alloc_page if order > 5

# Break on nth hit
ignore 1 99    # Skip first 99 hits of breakpoint 1
```

### Commands on Breakpoint

```gdb
# Run commands when breakpoint hits
break kernel_main
commands
    print total_memory
    continue
end
```

### Python Scripting

GDB has Python API:

```python
# In GDB:
python
def print_processes():
    # Walk process list
    process = gdb.parse_and_eval("process_list_head")
    while process != 0:
        print(f"PID: {process['pid']}, Name: {process['name'].string()}")
        process = process['next']
end

# Call it
python print_processes()
```

### Pretty Printers

Create custom formatters for complex structures:

```python
class ProcessPrinter:
    def __init__(self, val):
        self.val = val
    
    def to_string(self):
        pid = self.val['pid']
        name = self.val['name'].string()
        return f"Process(pid={pid}, name='{name}')"

# Register
import gdb.printing
gdb.printing.register_pretty_printer(None, ProcessPrinter)
```

---

## Step 12: Debugging Without GDB

### Serial Console Logging

Add `kprintf()` everywhere:

```c
void pmm_alloc_page(void) {
    kprintf("[DEBUG] pmm_alloc_page() called\n");
    kprintf("[DEBUG] free_list_head = %p\n", free_list_head);
    
    // ... code ...
    
    kprintf("[DEBUG] Allocated page at %p\n", page);
    return page;
}
```

### Assertions

Catch bugs early:

```c
#define ASSERT(condition) \
    do { \
        if (!(condition)) { \
            kprintf("[ASSERT FAILED] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            kernel_panic("Assertion failed"); \
        } \
    } while(0)

// Usage
void* page = pmm_alloc_page();
ASSERT(page != NULL);
```

### QEMU Monitor

Press `Ctrl+A, C` in QEMU to get monitor prompt:

```
(qemu) info registers
(qemu) x/16x 0xFFFF800000000000
(qemu) info mem
(qemu) info tlb
```

---

## Exercises

### Easy

1. Set a breakpoint at `kernel_main` and step through initialization
2. Print the value of `total_memory` after PMM initialization
3. View the stack backtrace when the shell starts

### Medium

4. Debug a null pointer dereference (introduce one on purpose)
5. Use a watchpoint to catch when a global variable changes
6. Set a breakpoint that only fires on the 10th call to a function

### Hard

7. Debug a race condition between timer interrupt and syscall
8. Write a GDB script to print all active processes
9. Debug userspace and kernel code in the same session

---

## Common Issues

### GDB can't find symbols

**Problem:** `No symbol table is loaded`.

**Solution:**
```bash
# Rebuild with debug symbols
make clean
CFLAGS="-g -O0" make all

# Load symbols in GDB
file build/kernel.elf
```

### Breakpoint not hit

**Problem:** Breakpoint set but never triggers.

**Solutions:**
- Check the symbol name: `info functions kernel_main`
- Try address: `break *0xFFFFFFFF80001234`
- Verify code is actually called: Add `kprintf()` before breakpoint

### GDB disconnects

**Problem:** `Remote connection closed`.

**Solution:** QEMU crashed. Check QEMU output for panic message.

### Can't step through optimized code

**Problem:** `next` jumps around weirdly.

**Solution:** Build with `-O0`:

```makefile
CFLAGS = -g -O0 -ffreestanding ...
```

---

## Summary

You've learned:

✅ How to set up GDB with QEMU  
✅ Essential GDB commands  
✅ Inspecting registers, memory, and stack  
✅ Setting breakpoints and watchpoints  
✅ Debugging kernel and userspace  
✅ Advanced techniques (conditional breakpoints, Python)  
✅ Alternative debugging methods (serial logging, assertions)  

**Key Concepts:**

- **Remote debugging** connects GDB to QEMU over TCP
- **Debug symbols** (`-g`) enable source-level debugging
- **Breakpoints** pause execution at specific points
- **Watchpoints** break when memory changes
- **Call stack** shows function call history
- **Serial logging** is your friend when GDB isn't available

---

## Debugging Cheat Sheet

```gdb
# Connection
target remote :1234

# Breakpoints
break kernel_main
break *0xFFFF800000001234
hbreak pmm_alloc_page
watch my_variable

# Execution
continue (c)
next (n)
step (s)
finish (fin)

# Inspection
info registers (i r)
print my_var (p my_var)
x/16xb $rsp
backtrace (bt)
frame 1

# Symbols
info functions
info variables
add-symbol-file build/userspace/shell/shell 0x400000
```

---

## Next Steps

Ready to modify the kernel?

1. **Tutorial 5: Adding a Syscall** - Implement your own syscall
2. **Tutorial 6: Writing a Driver** - Create a device driver
3. **Tutorial 7: Memory Management** - Deep dive into PMM/VMM

---

## Resources

- [GDB Manual](https://sourceware.org/gdb/current/onlinedocs/gdb/)
- [QEMU Documentation](https://www.qemu.org/docs/master/)
- [OSDev Wiki: Debugging](https://wiki.osdev.org/Debugging)
- [.gdbinit](../../.gdbinit) - Project GDB config

---

**Next Tutorial:** [05_ADDING_SYSCALL.md](05_ADDING_SYSCALL.md) - Add a new system call

---

*Last Updated: 2026-05-26*
