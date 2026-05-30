# AutomationOS API Reference

**Version:** 0.1.0  
**Phase:** 1 - Core Foundation  
**Last Updated:** 2026-05-26

---

## Table of Contents

1. [Memory Management API](#memory-management-api)
2. [Process Management API](#process-management-api)
3. [System Call API](#system-call-api)
4. [Driver API](#driver-api)
5. [Architecture-Specific API](#architecture-specific-api)
6. [Kernel Library API](#kernel-library-api)
7. [Userspace libc API](#userspace-libc-api)

---

## Memory Management API

### Physical Memory Manager (PMM)

**Header:** `kernel/include/mem.h`

#### `pmm_init()`

Initialize the physical memory manager with the boot memory map.

```c
void pmm_init(memory_map_entry_t* mmap, uint32_t mmap_count);
```

**Parameters:**
- `mmap`: Pointer to array of memory map entries from bootloader
- `mmap_count`: Number of entries in memory map

**Description:**
Initializes the buddy allocator with usable memory regions. Must be called early in kernel initialization.

**Example:**
```c
pmm_init(boot_info->memory_map, boot_info->memory_map_count);
```

---

#### `pmm_alloc_page()`

Allocate a single physical page (4KB).

```c
void* pmm_alloc_page(void);
```

**Returns:**
- Pointer to physical page address
- NULL on allocation failure (triggers kernel panic)

**Description:**
Allocates a 4KB page using the buddy allocator. Pages are allocated from the lowest available order.

**Example:**
```c
void* page = pmm_alloc_page();
if (page == NULL) {
    // Panic: out of memory
}
```

---

#### `pmm_free_page()`

Free a previously allocated physical page.

```c
void pmm_free_page(void* page);
```

**Parameters:**
- `page`: Physical address of page to free (must be page-aligned)

**Description:**
Returns a page to the free list. Adjacent free pages are coalesced using the buddy algorithm.

**Example:**
```c
pmm_free_page(page);
```

---

#### `pmm_get_total_memory()`

Get total physical memory in bytes.

```c
uint64_t pmm_get_total_memory(void);
```

**Returns:** Total memory in bytes

---

#### `pmm_get_used_memory()`

Get currently allocated memory in bytes.

```c
uint64_t pmm_get_used_memory(void);
```

**Returns:** Used memory in bytes

---

#### `pmm_get_free_memory()`

Get available free memory in bytes.

```c
uint64_t pmm_get_free_memory(void);
```

**Returns:** Free memory in bytes

**Example:**
```c
uint64_t free_mb = pmm_get_free_memory() / (1024 * 1024);
kprintf("Free memory: %u MB\n", (uint32_t)free_mb);
```

---

### Virtual Memory Manager (VMM)

**Header:** `kernel/include/mem.h`

#### `vmm_init()`

Initialize the virtual memory manager.

```c
void vmm_init(void);
```

**Description:**
Sets up 4-level paging structures (PML4 → PDPT → PD → PT). Maps kernel in higher half and sets up identity mapping for low memory.

**Example:**
```c
vmm_init();
```

---

#### `vmm_map_page()`

Map a virtual page to a physical page.

```c
void* vmm_map_page(void* virt, void* phys, uint32_t flags);
```

**Parameters:**
- `virt`: Virtual address (must be page-aligned)
- `phys`: Physical address (must be page-aligned)
- `flags`: Page flags (see below)

**Returns:**
- Virtual address on success
- NULL on failure

**Flags:**
- `PAGE_PRESENT` (0x01): Page is present
- `PAGE_WRITE` (0x02): Page is writable
- `PAGE_USER` (0x04): Accessible from user mode

**Example:**
```c
void* virt = (void*)0x400000;
void* phys = pmm_alloc_page();
vmm_map_page(virt, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
```

---

#### `vmm_unmap_page()`

Unmap a virtual page.

```c
void vmm_unmap_page(void* virt);
```

**Parameters:**
- `virt`: Virtual address to unmap (must be page-aligned)

**Description:**
Removes the mapping for a virtual page. Does not free the physical page.

---

#### `vmm_get_physical()`

Get physical address for a virtual address.

```c
void* vmm_get_physical(void* virt);
```

**Parameters:**
- `virt`: Virtual address

**Returns:**
- Physical address
- NULL if not mapped

---

### Kernel Heap

**Header:** `kernel/include/mem.h`

#### `heap_init()`

Initialize the kernel heap.

```c
void heap_init(void);
```

**Description:**
Sets up the slab allocator for dynamic kernel memory allocation.

---

#### `kmalloc()`

Allocate memory from kernel heap.

```c
void* kmalloc(size_t size);
```

**Parameters:**
- `size`: Number of bytes to allocate

**Returns:**
- Pointer to allocated memory
- NULL on failure (triggers kernel panic)

**Example:**
```c
char* buffer = kmalloc(1024);
// Use buffer...
kfree(buffer);
```

---

#### `kfree()`

Free heap-allocated memory.

```c
void kfree(void* ptr);
```

**Parameters:**
- `ptr`: Pointer returned by `kmalloc()` (or NULL)

**Description:**
Returns memory to the heap. Passing NULL is safe (no-op).

---

## Process Management API

### Process Table

**Header:** `kernel/include/sched.h`

#### `process_init()`

Initialize the process subsystem.

```c
void process_init(void);
```

**Description:**
Initializes the process table and allocates the idle process (PID 0).

---

#### `process_create()`

Create a new process.

```c
process_t* process_create(const char* name, void* entry_point);
```

**Parameters:**
- `name`: Process name (max 63 characters)
- `entry_point`: Function to execute

**Returns:**
- Pointer to process control block (PCB)
- NULL on failure

**Description:**
Allocates a new PCB, assigns a PID, sets up kernel and user stacks, and initializes the CPU context.

**Example:**
```c
process_t* init = process_create("init", &init_main);
scheduler_add_process(init);
```

---

#### `process_destroy()`

Destroy a process and free its resources.

```c
void process_destroy(process_t* proc);
```

**Parameters:**
- `proc`: Process to destroy

**Description:**
Frees kernel stack, user stack, page tables, and PCB. Removes from scheduler.

---

#### `process_get_by_pid()`

Look up a process by PID.

```c
process_t* process_get_by_pid(uint32_t pid);
```

**Parameters:**
- `pid`: Process ID

**Returns:**
- Pointer to PCB
- NULL if not found

---

#### `process_get_current()`

Get currently running process.

```c
process_t* process_get_current(void);
```

**Returns:** Current process PCB

---

#### `process_set_current()`

Set the current process (internal use).

```c
void process_set_current(process_t* proc);
```

---

### Scheduler

**Header:** `kernel/include/sched.h`

#### `scheduler_init()`

Initialize the scheduler.

```c
void scheduler_init(void);
```

**Description:**
Sets up the ready queue and configures the timer interrupt for preemptive scheduling.

---

#### `scheduler_add_process()`

Add a process to the ready queue.

```c
void scheduler_add_process(process_t* proc);
```

**Parameters:**
- `proc`: Process to add

**Description:**
Marks the process as READY and adds it to the round-robin queue.

---

#### `scheduler_remove_process()`

Remove a process from the ready queue.

```c
void scheduler_remove_process(process_t* proc);
```

**Parameters:**
- `proc`: Process to remove

---

#### `schedule()`

Scheduler tick function (called from timer interrupt).

```c
void schedule(void);
```

**Description:**
Decrements current process time slice. If expired, picks next process and performs context switch.

**Note:** This is called automatically from the PIT interrupt handler. Do not call directly.

---

#### `scheduler_pick_next()`

Select the next process to run (round-robin).

```c
process_t* scheduler_pick_next(void);
```

**Returns:** Next process to run

---

### Context Switching

**Header:** `kernel/include/sched.h`

#### `context_switch()`

Perform a context switch between processes.

```c
void context_switch(process_t* from, process_t* to);
```

**Parameters:**
- `from`: Current process (context will be saved)
- `to`: Next process (context will be restored)

**Description:**
Low-level assembly function that saves/restores CPU registers and switches page tables (CR3).

**Note:** Internal function - called by scheduler. Do not call directly.

---

## System Call API

### System Call Interface

**Header:** `kernel/include/syscall.h`

#### `syscall_init()`

Initialize the system call subsystem.

```c
void syscall_init(void);
```

**Description:**
Sets up the syscall handler table and configures the `syscall` instruction (MSRs).

---

#### `syscall_dispatch()`

Dispatch a system call to its handler.

```c
int64_t syscall_dispatch(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);
```

**Parameters:**
- `syscall_num`: System call number (0-255)
- `arg1` - `arg6`: System call arguments

**Returns:** System call return value (or error code)

**Note:** Internal function - called from `syscall_entry` in assembly.

---

### System Call Handlers

#### `sys_exit()`

Terminate the current process.

```c
int64_t sys_exit(uint64_t status, ...);
```

**Parameters:**
- `status`: Exit code

**Returns:** Does not return

**Syscall Number:** `SYS_EXIT` (0)

---

#### `sys_fork()`

Create a child process.

```c
int64_t sys_fork(uint64_t arg1, ...);
```

**Returns:**
- Child PID in parent process
- 0 in child process
- Negative error code on failure

**Syscall Number:** `SYS_FORK` (1)

---

#### `sys_read()`

Read from a file descriptor.

```c
int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count, ...);
```

**Parameters:**
- `fd`: File descriptor (0 = stdin)
- `buf`: Buffer to read into
- `count`: Maximum bytes to read

**Returns:**
- Number of bytes read
- Negative error code on failure

**Syscall Number:** `SYS_READ` (2)

---

#### `sys_write()`

Write to a file descriptor.

```c
int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count, ...);
```

**Parameters:**
- `fd`: File descriptor (1 = stdout, 2 = stderr)
- `buf`: Buffer to write from
- `count`: Number of bytes to write

**Returns:**
- Number of bytes written
- Negative error code on failure

**Syscall Number:** `SYS_WRITE` (3)

**Example:**
```c
const char* msg = "Hello, world!\n";
sys_write(1, (uint64_t)msg, strlen(msg), 0, 0, 0);
```

---

#### `sys_getpid()`

Get current process ID.

```c
int64_t sys_getpid(void);
```

**Returns:** Current process PID

**Syscall Number:** `SYS_GETPID` (8)

---

### Error Codes

```c
#define ESUCCESS    0   // Success
#define ENOTSUP    -1   // Not supported
#define EINVAL     -2   // Invalid argument
#define EBADF      -3   // Bad file descriptor
#define ENOMEM     -4   // Out of memory
#define ESRCH      -5   // No such process
```

---

## Driver API

### Serial Driver

**Header:** `kernel/include/drivers.h`

#### `serial_init()`

Initialize the serial console (COM1).

```c
void serial_init(void);
```

**Configuration:**
- Port: COM1 (0x3F8)
- Baud rate: 38400
- Data bits: 8
- Parity: None
- Stop bits: 1

---

#### `serial_putchar()`

Write a character to the serial port.

```c
void serial_putchar(char c);
```

**Parameters:**
- `c`: Character to write

---

#### `serial_write()`

Write a string to the serial port.

```c
void serial_write(const char* str, size_t len);
```

**Parameters:**
- `str`: String to write
- `len`: Length of string

---

### PS/2 Keyboard Driver

**Header:** `kernel/include/drivers.h`

#### `ps2_init()`

Initialize the PS/2 keyboard controller.

```c
void ps2_init(void);
```

---

#### `ps2_getchar()`

Read a character from the keyboard (blocking).

```c
char ps2_getchar(void);
```

**Returns:** ASCII character

**Description:**
Blocks until a key is pressed. Handles scancode translation to ASCII.

---

### Framebuffer Driver

**Header:** `kernel/include/drivers.h`

#### `framebuffer_init()`

Initialize the framebuffer.

```c
void framebuffer_init(void* fb_addr, uint32_t width, uint32_t height, uint32_t pitch);
```

**Parameters:**
- `fb_addr`: Physical address of framebuffer
- `width`: Width in pixels
- `height`: Height in pixels
- `pitch`: Bytes per scanline

---

#### `framebuffer_clear()`

Clear the screen to a solid color.

```c
void framebuffer_clear(uint32_t color);
```

**Parameters:**
- `color`: RGB color (0xRRGGBB)

---

#### `framebuffer_putchar()`

Draw a character at (x, y).

```c
void framebuffer_putchar(char c, uint32_t x, uint32_t y, uint32_t color);
```

**Parameters:**
- `c`: Character to draw
- `x`: X coordinate (character cells)
- `y`: Y coordinate (character cells)
- `color`: RGB color

---

### Timer Driver (PIT)

**Header:** `kernel/include/drivers.h`

#### `pit_init()`

Initialize the Programmable Interval Timer.

```c
void pit_init(uint32_t frequency);
```

**Parameters:**
- `frequency`: Timer frequency in Hz (e.g., 100 for 10ms ticks)

---

#### `timer_get_ticks()`

Get the number of timer ticks since boot.

```c
uint64_t timer_get_ticks(void);
```

**Returns:** Tick count

---

#### `timer_get_frequency()`

Get the configured timer frequency.

```c
uint32_t timer_get_frequency(void);
```

**Returns:** Frequency in Hz

---

#### `timer_sleep()`

Sleep for a specified number of milliseconds.

```c
void timer_sleep(uint32_t ms);
```

**Parameters:**
- `ms`: Milliseconds to sleep

**Description:**
Busy-wait loop. Not suitable for long delays.

---

## Architecture-Specific API

### x86_64 Functions

**Header:** `kernel/include/x86_64.h`

#### `gdt_init()`

Initialize the Global Descriptor Table.

```c
void gdt_init(void);
```

---

#### `idt_init()`

Initialize the Interrupt Descriptor Table.

```c
void idt_init(void);
```

---

#### Port I/O

Read/write to I/O ports.

```c
uint8_t inb(uint16_t port);
void outb(uint16_t port, uint8_t value);
uint16_t inw(uint16_t port);
void outw(uint16_t port, uint16_t value);
uint32_t inl(uint16_t port);
void outl(uint16_t port, uint32_t value);
```

**Example:**
```c
outb(0x3F8, 'A');  // Write 'A' to COM1
```

---

#### CPU Control

```c
void cli(void);     // Disable interrupts
void sti(void);     // Enable interrupts
void hlt(void);     // Halt CPU until interrupt
```

---

#### Control Registers

```c
uint64_t read_cr0(void);
void write_cr0(uint64_t value);
uint64_t read_cr2(void);  // Page fault address
uint64_t read_cr3(void);  // Page directory
void write_cr3(uint64_t value);
uint64_t read_cr4(void);
void write_cr4(uint64_t value);
```

---

## Kernel Library API

### String Functions

**Header:** `kernel/lib/string.c`

```c
void* memset(void* s, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
int memcmp(const void* s1, const void* s2, size_t n);
size_t strlen(const char* s);
char* strcpy(char* dest, const char* src);
int strcmp(const char* s1, const char* s2);
```

---

### Kernel Printf

**Header:** `kernel/include/kernel.h`

#### `kprintf()`

Formatted kernel output.

```c
int kprintf(const char* format, ...);
```

**Supported Format Specifiers:**
- `%s` - String
- `%c` - Character
- `%d`, `%i` - Signed integer
- `%u` - Unsigned integer
- `%x` - Hexadecimal (lowercase)
- `%X` - Hexadecimal (uppercase)
- `%p` - Pointer

**Example:**
```c
kprintf("[KERNEL] Initialized %d subsystems\n", count);
kprintf("[MEM] Free memory: %p\n", free_mem);
```

---

### Panic and Assertions

**Header:** `kernel/include/kernel.h`

#### `kernel_panic()`

Trigger a kernel panic (unrecoverable error).

```c
void kernel_panic(const char* message) __attribute__((noreturn));
```

**Parameters:**
- `message`: Panic message

**Example:**
```c
if (critical_failure) {
    kernel_panic("Critical subsystem failure");
}
```

---

#### `ASSERT()`

Runtime assertion macro.

```c
#define ASSERT(cond) do { \
    if (!(cond)) kernel_panic("Assertion failed: " #cond); \
} while(0)
```

**Example:**
```c
ASSERT(ptr != NULL);
ASSERT(size > 0 && size < MAX_SIZE);
```

---

## Userspace libc API

### System Call Wrappers

**Header:** `userspace/libc/syscall.h`

```c
int exit(int status);
pid_t fork(void);
ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
pid_t getpid(void);
void sleep(unsigned int ms);
```

**Example:**
```c
#include <syscall.h>

int main(void) {
    write(1, "Hello, world!\n", 14);
    return 0;
}
```

---

### Standard I/O

**Header:** `userspace/libc/stdio.h`

```c
int printf(const char* format, ...);
int putchar(int c);
int puts(const char* s);
char* gets(char* s);
```

---

### String Functions

**Header:** `userspace/libc/string.h`

```c
size_t strlen(const char* s);
char* strcpy(char* dest, const char* src);
int strcmp(const char* s1, const char* s2);
void* memset(void* s, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
```

---

## Usage Examples

### Example 1: Allocating and Mapping Memory

```c
// Allocate a physical page
void* phys_page = pmm_alloc_page();

// Map it to user space
void* virt_addr = (void*)0x400000;
vmm_map_page(virt_addr, phys_page, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

// Use the memory
memset(virt_addr, 0, PAGE_SIZE);

// Clean up
vmm_unmap_page(virt_addr);
pmm_free_page(phys_page);
```

---

### Example 2: Creating a Process

```c
void init_main(void) {
    printf("Init process started\n");
    
    // Fork and exec shell
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        execve("/bin/shell", NULL, NULL);
    } else {
        // Parent process
        waitpid(pid, NULL, 0);
    }
}

// In kernel
process_t* init = process_create("init", &init_main);
scheduler_add_process(init);
```

---

### Example 3: Writing a Simple Driver

```c
#include "drivers.h"

static uint16_t io_base = 0x2F8;  // COM2

void com2_init(void) {
    outb(io_base + 1, 0x00);  // Disable interrupts
    outb(io_base + 3, 0x80);  // Enable DLAB
    outb(io_base + 0, 0x03);  // Set divisor
    outb(io_base + 1, 0x00);
    outb(io_base + 3, 0x03);  // 8n1
    outb(io_base + 2, 0xC7);  // Enable FIFO
    kprintf("[COM2] Initialized\n");
}

void com2_write(char c) {
    while ((inb(io_base + 5) & 0x20) == 0);
    outb(io_base, c);
}
```

---

## API Conventions

### Naming

- **Kernel functions:** `subsystem_action()` (e.g., `pmm_alloc_page`)
- **System calls:** `sys_name()` (e.g., `sys_write`)
- **Driver functions:** `driver_action()` (e.g., `serial_init`)
- **Internal helpers:** `_function()` or `__function()` (leading underscore)

### Return Values

- **Success:** 0 or positive value (e.g., bytes written, PID)
- **Error:** Negative error code (e.g., `EINVAL`, `ENOMEM`)
- **Pointers:** NULL on failure, valid pointer on success

### Error Handling

```c
// Kernel code
void* ptr = kmalloc(size);
if (ptr == NULL) {
    return ENOMEM;  // Propagate error
}

// Or panic for critical errors
ASSERT(ptr != NULL);
```

---

## Thread Safety

**Phase 1 Status:** Most kernel APIs are NOT thread-safe.

- **Safe:** PMM allocation (protected by implicit spinlocks)
- **Unsafe:** Process table, scheduler queue, driver globals

Phase 2 will add:
- Spinlocks for critical sections
- Per-CPU data structures
- RCU (Read-Copy-Update) for read-heavy structures

---

## Performance Notes

- **Fast paths:** `pmm_alloc_page()`, `vmm_map_page()`, system calls
- **Slow paths:** `process_create()` (allocates stacks), `context_switch()` (TLB flush)
- **Avoid:** Frequent allocation/deallocation, excessive system calls in tight loops

---

**End of API Reference**
