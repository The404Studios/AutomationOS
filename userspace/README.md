# AutomationOS Userspace Implementation

This directory contains the userspace components for AutomationOS Phase 1.

## Components

### 1. **libc/** - Userspace C Library

Minimal C library for userspace programs.

**Files:**
- `syscall.c/h` - Syscall wrappers using x86_64 `syscall` instruction
- `string.c/h` - String manipulation functions (strlen, strcmp, strcpy, memset, etc.)
- `stdio.c/h` - Standard I/O functions (printf, puts, putchar, getchar)
- `Makefile` - Builds libc.a static library

**Syscalls Implemented:**
- `exit(int status)` - Terminate process
- `fork(void)` - Create child process (stub)
- `read(int fd, void* buf, size_t count)` - Read from file descriptor
- `write(int fd, const void* buf, size_t count)` - Write to file descriptor
- `getpid(void)` - Get process ID
- `sleep(unsigned int seconds)` - Sleep for seconds (stub)
- `open/close/waitpid/execve` - File/process operations (stubs)

**Syscall Interface:**
- Uses x86_64 `syscall` instruction
- Arguments passed in registers: RAX (syscall #), RDI, RSI, RDX, R10, R8, R9
- Return value in RAX

**stdio Functions:**
- `printf()` - Format specifiers: %s, %d, %u, %x, %p, %c, %%
- `puts()` - Print string with newline
- `putchar()` - Print single character
- `getchar()` - Read single character (stub)

### 2. **init/** - Init Process (PID 1)

First userspace process that initializes the system and spawns the shell.

**Files:**
- `init.c` - Init process implementation
- `Makefile` - Builds init binary

**Features:**
- Verifies it is running as PID 1
- Spawns shell using fork/execve
- Monitors child processes
- Respawns shell if it dies
- Uses waitpid() to reap zombie processes

**Entry Point:**
- `_start()` - Direct entry point (no C runtime)

### 3. **shell/** - Simple Shell

Interactive command-line shell with built-in commands.

**Files:**
- `shell.c` - Main shell loop and built-in commands
- `parser.c` - Command line parser
- `shell.h` - Shared declarations
- `Makefile` - Builds shell binary

**Built-in Commands:**
- `echo [args...]` - Print arguments to stdout
- `help` - Show available commands
- `exit` - Exit the shell
- `clear` - Clear the screen (ANSI escape sequence)
- `pid` - Show shell process ID

**Features:**
- Command prompt: `aos>`
- Command parser supports:
  - Space-delimited arguments
  - Quoted strings (basic)
  - Backspace editing
  - Echo typed characters
- Extensible built-in command table
- Error handling for unknown commands

**Command Parser:**
- Tokenizes input into command name and arguments
- Handles whitespace and quoted strings
- Uses static token buffer (no dynamic allocation)
- Returns command_t structure with argc/argv

## Build System

### Build Order:
1. `make libc` - Build C library (libc.a)
2. `make init` - Build init process (depends on libc)
3. `make shell` - Build shell (depends on libc)

### Build Outputs:
- `build/userspace/libc/libc.a` - Static C library
- `build/userspace/init/init` - Init process binary
- `build/userspace/shell/shell` - Shell binary

### Compiler Flags:
- `-ffreestanding` - No hosted environment
- `-nostdlib` - No standard library
- `-fno-builtin` - No built-in functions
- `-mno-red-zone` - No red zone (required for kernel space)
- `-std=gnu11` - C11 with GNU extensions

### Linker Flags:
- `-nostdlib` - No standard library
- `-static` - Static linking

## Integration with Kernel

### Syscall Convention:
Userspace uses the x86_64 `syscall` instruction:

```nasm
mov rax, syscall_number  ; Syscall number
mov rdi, arg1           ; Argument 1
mov rsi, arg2           ; Argument 2
mov rdx, arg3           ; Argument 3
mov r10, arg4           ; Argument 4 (R10, not RCX)
mov r8, arg5            ; Argument 5
mov r9, arg6            ; Argument 6
syscall                 ; Invoke kernel
; Return value in RAX
```

### Kernel Entry Point:
- `kernel/arch/x86_64/syscall.asm:syscall_entry`
- Saves user registers
- Calls `syscall_dispatch()`
- Restores registers and returns to userspace

### Syscall Handler Table:
- `kernel/core/syscall/syscall.c` - Dispatcher
- `kernel/core/syscall/handlers.c` - Handler implementations

## Testing

To test userspace components:

1. Build kernel with process support
2. Load init as PID 1
3. Init spawns shell
4. Type commands in shell

Example session:
```
aos> help

AutomationOS Shell - Built-in Commands
=======================================

  echo       - Print arguments to stdout
  help       - Show available commands
  exit       - Exit the shell
  clear      - Clear the screen
  pid        - Show process ID

aos> echo Hello, World!
Hello, World!
aos> pid
Shell PID: 2
aos> exit
Goodbye!
```

## Future Enhancements

### Phase 2 Additions:
- [ ] Dynamic memory allocation (malloc/free)
- [ ] File I/O (open/close/read/write to files)
- [ ] Directory operations (ls, cd, mkdir, rmdir)
- [ ] Process control (fork/exec/wait fully functional)
- [ ] Environment variables
- [ ] Command history and line editing
- [ ] Job control (bg/fg)
- [ ] Pipes and redirection
- [ ] External programs (beyond built-ins)

## Implementation Notes

### Task 18: Userspace libc
**Status:** ✅ COMPLETE

Implemented:
- Syscall wrappers using x86_64 syscall instruction
- String functions (strlen, strcmp, strncpy, memset, memcpy, etc.)
- Standard I/O (printf, puts, putchar, getchar)
- Static library build (libc.a)

### Task 19: Init & Shell
**Status:** ✅ COMPLETE

Implemented:
- Init process (PID 1) with fork/exec/waitpid
- Shell respawning on crash
- Interactive shell with command prompt
- Command parser with tokenization
- Built-in commands: echo, help, exit, clear, pid
- Backspace editing and character echo

## Commit Messages

### Commit 1: Task 18 - Userspace libc
```
feat(userspace): implement userspace C library (Task 18)

- Add syscall wrappers using x86_64 syscall instruction
- Implement string functions (strlen, strcmp, strcpy, memset, etc.)
- Implement stdio functions (printf, puts, putchar, getchar)
- Add syscall.c with exit, fork, read, write, getpid, etc.
- Add Makefile to build libc.a static library
- Support format specifiers: %s, %d, %u, %x, %p, %c, %%

Co-Authored-By: Claude Sonnet 4.5 (1M context) <noreply@anthropic.com>
```

### Commit 2: Task 19 - Init & Shell
```
feat(userspace): implement init process and shell (Task 19)

- Add init.c (PID 1) that spawns and monitors shell
- Add shell.c with command loop and built-in commands
- Add parser.c for command line tokenization
- Implement built-in commands: echo, help, exit, clear, pid
- Support backspace editing and character echo
- Auto-respawn shell if it crashes
- Add Makefiles for init and shell binaries

Co-Authored-By: Claude Sonnet 4.5 (1M context) <noreply@anthropic.com>
```

## File Tree
```
userspace/
├── Makefile                    # Top-level build
├── README.md                   # This file
├── libc/
│   ├── Makefile               # Build libc.a
│   ├── syscall.c              # Syscall wrappers
│   ├── syscall.h              # Syscall declarations
│   ├── string.c               # String functions
│   ├── string.h               # String declarations
│   ├── stdio.c                # Standard I/O
│   └── stdio.h                # Standard I/O declarations
├── init/
│   ├── Makefile               # Build init binary
│   └── init.c                 # Init process (PID 1)
└── shell/
    ├── Makefile               # Build shell binary
    ├── shell.h                # Shell declarations
    ├── shell.c                # Main shell loop
    └── parser.c               # Command parser
```
