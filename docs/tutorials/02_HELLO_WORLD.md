# Tutorial 2: Hello World - Your First User Program

**Difficulty:** Beginner  
**Time:** 20-30 minutes  
**Prerequisites:** Tutorial 1 (Getting Started)  

---

## Introduction

In this tutorial, you'll write your first userspace program for AutomationOS. You'll learn:

- How userspace programs work
- The AutomationOS C library (libc)
- How to compile and link programs
- How to load programs into the OS
- Basic system calls

By the end, you'll have written and run "Hello, World!" on your own operating system!

---

## Understanding Userspace vs Kernel Space

Before we start coding, let's understand where programs run:

### Memory Privilege Levels

x86_64 CPUs have **4 privilege levels (rings)**:

- **Ring 0** (kernel mode): Full hardware access, all instructions
- **Ring 3** (user mode): Restricted access, can't directly access hardware

AutomationOS uses:
- **Ring 0** for the kernel
- **Ring 3** for user programs

### Why Separate Kernel and User Space?

**Security:** User programs can't crash the kernel or access other processes' memory.

**Stability:** A buggy program crashes itself, not the whole OS.

**Abstraction:** Programs don't need to know hardware details.

### How They Communicate

User programs request services via **system calls (syscalls)**:

```
User Program                    Kernel
  (Ring 3)                    (Ring 0)
     |                            |
     | write(1, "hello", 5)       |
     |--------------------------->|
     |     (syscall instruction)  |
     |                            |
     |    (kernel writes to       |
     |     serial console)        |
     |                            |
     |<---------------------------|
     |    (return to user mode)   |
     |                            |
```

---

## Step 1: Explore the libc

AutomationOS includes a minimal C library in `userspace/libc/`.

### Available Functions

Let's see what's available:

```bash
cd ~/Desktop/AutomationOS
cat userspace/libc/stdio.h
```

You'll see:

```c
#ifndef STDIO_H
#define STDIO_H

int printf(const char* format, ...);
int puts(const char* str);
int putchar(int ch);
int getchar(void);

#endif
```

And system calls in `syscall.h`:

```bash
cat userspace/libc/syscall.h
```

```c
// System call numbers
#define SYS_EXIT    0
#define SYS_FORK    1
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_GETPID  4

// System call wrappers
void exit(int status);
int fork(void);
int read(int fd, void* buf, size_t count);
int write(int fd, const void* buf, size_t count);
int getpid(void);
```

### How printf Works

Let's peek at the implementation:

```bash
cat userspace/libc/stdio.c | head -40
```

You'll see `printf()` is implemented using the `write()` syscall, which ultimately calls the kernel to output to the serial console.

---

## Step 2: Create Your Program Directory

Let's create a new directory for user programs:

```bash
mkdir -p userspace/bin
cd userspace/bin
```

This is where we'll put utilities like `hello`, `echo`, `ls`, etc.

---

## Step 3: Write hello.c

Create a new file:

```bash
nano hello.c
```

Enter this code:

```c
#include "../libc/stdio.h"
#include "../libc/syscall.h"

// Entry point for userspace programs
// No main() - we start at _start
void _start(void) {
    printf("Hello, World!\n");
    printf("I'm running in userspace!\n");
    printf("My PID is: %d\n", getpid());
    
    exit(0);  // Exit successfully
}
```

Save and exit (Ctrl+X, Y, Enter in nano).

### Understanding the Code

**Why `_start()` instead of `main()`?**

In normal C programs, `main()` is called by the C runtime. AutomationOS userspace programs are **freestanding** - there's no runtime, so we use `_start()` as the entry point.

**Why `exit(0)`?**

Programs must explicitly exit using the `exit()` syscall. This tells the kernel to clean up the process and return control to the parent (init).

---

## Step 4: Create a Makefile

We need to tell the build system how to compile our program.

Create `Makefile`:

```bash
nano Makefile
```

Enter:

```makefile
# Compiler and flags
CC = x86_64-elf-gcc
CFLAGS = -ffreestanding -nostdlib -fno-builtin -mno-red-zone -Wall -Wextra -std=gnu11
LDFLAGS = -nostdlib -static

# Paths
LIBC = ../libc/libc.a
BUILD_DIR = ../../build/userspace/bin

# Target
TARGET = hello

# Build
all: $(TARGET)

$(TARGET): hello.c $(LIBC)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/$(TARGET) hello.c $(LIBC)
	@echo "Built: $(BUILD_DIR)/$(TARGET)"

clean:
	rm -f $(BUILD_DIR)/$(TARGET)

.PHONY: all clean
```

Save and exit.

### Understanding the Flags

- `-ffreestanding` - No hosted environment (no OS)
- `-nostdlib` - Don't link standard library
- `-fno-builtin` - Don't use GCC built-in functions
- `-mno-red-zone` - Required for kernel space (we don't need it in userspace but it's safe)
- `-static` - Static linking (no dynamic libraries)

---

## Step 5: Build Your Program

```bash
make
```

**Expected output:**

```
mkdir -p ../../build/userspace/bin
x86_64-elf-gcc -ffreestanding -nostdlib -fno-builtin -mno-red-zone -Wall -Wextra -std=gnu11 -nostdlib -static -o ../../build/userspace/bin/hello hello.c ../libc/libc.a
Built: ../../build/userspace/bin/hello
```

Check what was created:

```bash
ls -lh ../../build/userspace/bin/hello
file ../../build/userspace/bin/hello
```

You should see it's an ELF 64-bit executable.

---

## Step 6: Add to ISO (Manual Method)

For now, we'll manually add our program to the ISO. In a later tutorial, we'll automate this.

### Option A: Modify the ISO Build Script

Edit the ISO generation script:

```bash
nano ../../scripts/build-iso.py
```

Find the section that copies userspace binaries and add:

```python
# Copy userspace binaries
shutil.copy("build/userspace/init/init", os.path.join(iso_root, "init"))
shutil.copy("build/userspace/shell/shell", os.path.join(iso_root, "shell"))
shutil.copy("build/userspace/bin/hello", os.path.join(iso_root, "hello"))  # Add this line
```

### Option B: Add to Root Makefile

Edit the root Makefile:

```bash
nano ../../Makefile
```

Add a new target:

```makefile
userspace-bin:
	$(MAKE) -C userspace/bin

userspace: userspace-bin
	$(MAKE) -C userspace
```

---

## Step 7: Test in Isolation (Without Booting)

Before adding to the OS, let's verify our program compiles and links correctly.

### Check the Symbols

```bash
x86_64-elf-nm ../../build/userspace/bin/hello
```

You should see:
- `_start` - Your entry point
- `printf` - From libc
- `getpid` - Syscall wrapper
- `exit` - Syscall wrapper

### Check Dependencies

```bash
x86_64-elf-readelf -l ../../build/userspace/bin/hello
```

This shows the program segments. You should see:
- `LOAD` segments for code and data
- No `INTERP` segment (no dynamic linker)

---

## Step 8: Integrate with Init/Shell

For now, AutomationOS doesn't have a full filesystem or `exec()` from shell. We need to modify init or shell to call your program directly.

### Method 1: Add to Shell Built-ins

Edit the shell to call your hello program:

```bash
nano ../shell/shell.c
```

Add a new built-in command:

```c
// Add to built-in commands array
static builtin_t builtins[] = {
    {"echo", cmd_echo},
    {"help", cmd_help},
    {"exit", cmd_exit},
    {"clear", cmd_clear},
    {"pid", cmd_pid},
    {"hello", cmd_hello},  // Add this
    {NULL, NULL}
};

// Add the command function
static int cmd_hello(int argc, char** argv) {
    (void)argc;
    (void)argv;
    
    // For now, just print the output
    printf("Hello, World!\n");
    printf("I'm running in userspace!\n");
    printf("My PID is: %d\n", getpid());
    
    return 0;
}
```

### Method 2: Create a Combined Test Program

Let's create a simpler approach - a test program that demonstrates userspace features:

```bash
cd ../
nano test_program.c
```

```c
#include "libc/stdio.h"
#include "libc/syscall.h"
#include "libc/string.h"

void test_syscalls(void) {
    printf("\n=== System Call Test ===\n");
    
    // Test getpid
    int pid = getpid();
    printf("My PID: %d\n", pid);
    
    // Test write directly
    const char* msg = "Direct write syscall test\n";
    write(1, msg, strlen(msg));
    
    // Test read (will fail but demonstrates syscall)
    char buf[32];
    printf("\nTesting read syscall (will timeout)...\n");
    // Note: read() is not fully implemented yet in Phase 1
}

void test_libc(void) {
    printf("\n=== libc Test ===\n");
    
    // Test string functions
    char str1[32] = "Hello";
    char str2[32] = "World";
    
    printf("strlen(\"%s\") = %d\n", str1, strlen(str1));
    printf("strcmp(\"%s\", \"%s\") = %d\n", str1, str2, strcmp(str1, str2));
    
    // Test format specifiers
    printf("Integer: %d\n", 42);
    printf("Unsigned: %u\n", 4294967295U);
    printf("Hex: 0x%x\n", 0xDEADBEEF);
    printf("Pointer: %p\n", (void*)0xFFFFFFFF80000000);
    printf("Character: %c\n", 'A');
}

void _start(void) {
    printf("\n========================================\n");
    printf("  AutomationOS Userspace Test Program\n");
    printf("========================================\n");
    
    printf("\nHello, World!\n");
    printf("I'm running in userspace (Ring 3)!\n");
    
    test_libc();
    test_syscalls();
    
    printf("\n========================================\n");
    printf("  All tests completed!\n");
    printf("========================================\n\n");
    
    exit(0);
}
```

---

## Step 9: Rebuild and Test

```bash
cd ~/Desktop/AutomationOS
make clean
make all
make qemu
```

In the shell, type:

```bash
aos> hello
```

You should see:

```
Hello, World!
I'm running in userspace!
My PID is: 2
```

---

## Understanding What Happened

Let's trace the execution:

### 1. Compilation

```
hello.c  --(compiler)-->  hello.o  --(linker + libc.a)-->  hello (ELF)
```

### 2. Loading

When shell runs `hello`:
1. Shell calls `fork()` to create a child process
2. Child calls `execve("hello")` to load the program
3. Kernel loads ELF, sets up memory, jumps to `_start`

### 3. Execution

```
_start() called
  |
  v
printf("Hello, World!\n")
  |
  v
write(1, "Hello, World!\n", 14)  [syscall]
  |
  v
KERNEL: sys_write() writes to serial console
  |
  v
Back to userspace
  |
  v
getpid()  [syscall]
  |
  v
KERNEL: sys_getpid() returns current PID
  |
  v
Back to userspace
  |
  v
exit(0)  [syscall]
  |
  v
KERNEL: sys_exit() terminates process
```

---

## Step 10: Experiment!

Try modifying your program:

### Add More Output

```c
void _start(void) {
    printf("========================================\n");
    printf("         My First Program!\n");
    printf("========================================\n\n");
    
    printf("Testing printf format specifiers:\n");
    printf("  Integer: %d\n", 42);
    printf("  Hex: 0x%x\n", 255);
    printf("  Character: %c\n", 'A');
    printf("  String: %s\n", "AutomationOS rocks!");
    
    printf("\nSystem information:\n");
    printf("  Process ID: %d\n", getpid());
    printf("  Running in userspace (Ring 3)\n");
    
    exit(0);
}
```

### Create a Counter

```c
void _start(void) {
    printf("Counting to 10:\n");
    
    for (int i = 1; i <= 10; i++) {
        printf("  %d\n", i);
    }
    
    printf("Done!\n");
    exit(0);
}
```

### Test String Functions

```c
#include "../libc/string.h"

void _start(void) {
    char buf1[32];
    char buf2[32];
    
    strcpy(buf1, "Hello");
    strcpy(buf2, "World");
    
    printf("buf1: %s\n", buf1);
    printf("buf2: %s\n", buf2);
    printf("Length of buf1: %d\n", strlen(buf1));
    printf("Compare: %d\n", strcmp(buf1, buf2));
    
    strcat(buf1, ", ");
    strcat(buf1, buf2);
    printf("Concatenated: %s\n", buf1);
    
    exit(0);
}
```

---

## Common Issues

### "undefined reference to _start"

**Problem:** Linker can't find the entry point.

**Solution:** Make sure you have `void _start(void)` (not `int main()`).

### "undefined reference to printf"

**Problem:** Not linking libc.

**Solution:** Check Makefile includes `$(LIBC)` in the link command.

### Program doesn't run in shell

**Problem:** Shell doesn't have built-in or can't find binary.

**Solution:** For now, add as built-in command to shell.c.

### Crashes with "Page fault"

**Problem:** Accessing invalid memory.

**Solution:** Check you're not dereferencing NULL or accessing kernel memory.

---

## Key Takeaways

You've learned:

✅ The difference between kernel and user space  
✅ How to write freestanding C programs  
✅ How to use AutomationOS libc  
✅ How system calls work  
✅ How to compile and link programs  
✅ How to test userspace programs  

**Important Concepts:**

- **Freestanding vs Hosted**: Freestanding has no OS, hosted has libc/OS
- **Entry Point**: `_start()` is the raw entry, `main()` is C runtime entry
- **System Calls**: The bridge between user and kernel space
- **Static Linking**: All code included in the binary (no shared libraries)

---

## Next Steps

Ready to dive deeper?

1. **Tutorial 3: System Calls** - Learn all syscalls in detail
2. **Tutorial 4: Debugging** - Debug userspace programs with GDB
3. **Tutorial 5: Adding a Syscall** - Add your own syscall to the kernel

---

## Exercises

Try these challenges:

### Easy

1. Write a program that prints your name in ASCII art
2. Create a calculator program (add, subtract, multiply, divide)
3. Print the multiplication table (1-10)

### Medium

4. Write a program that counts characters in a string
5. Implement a simple number guessing game (hardcoded number)
6. Create a program that converts between temperature scales

### Hard

7. Write a mini unit testing framework
8. Implement a command-line argument parser
9. Create a program that demonstrates all available syscalls

**Hint:** For exercises, create separate `.c` files and Makefiles in `userspace/bin/`.

---

## Resources

- [API Reference](../API_REFERENCE.md) - All syscalls and functions
- [userspace/libc/](../../userspace/libc/) - libc source code
- [Development Guide](../DEVELOPMENT_GUIDE.md) - Advanced dev topics

---

**Next Tutorial:** [03_SYSTEM_CALLS.md](03_SYSTEM_CALLS.md) - Deep dive into system calls

---

*Last Updated: 2026-05-26*
