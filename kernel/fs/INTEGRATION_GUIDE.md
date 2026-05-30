# ELF Loader Integration Guide

## Quick Start

### Step 1: Build Userspace Init

```bash
cd userspace
make init
```

This creates `userspace/init/init` ELF64 executable.

### Step 2: Add Init to Initrd

Ensure your initrd build process includes the init program:

```bash
cd boot
# Add init to initrd.tar
tar -cf initrd.tar init
```

### Step 3: Integrate into Kernel Boot Sequence

Add to your kernel's `main()` or boot sequence:

```c
#include "include/elf.h"
#include "include/sched.h"
#include "include/initrd.h"

void kernel_main(void) {
    // ... existing initialization ...
    
    // Initialize subsystems
    pmm_init(...);
    vmm_init();
    heap_init();
    process_init();
    scheduler_init();
    
    // Initialize initrd
    initrd_init(initrd_addr, initrd_size);
    initrd_mount();
    
    // Launch /init process
    kprintf("\n[KERNEL] Launching /init...\n");
    
    int ret = exec_launch_init();
    if (ret != 0) {
        kernel_panic("Failed to launch /init");
    }
    
    // Start scheduler
    kprintf("[KERNEL] Starting scheduler...\n");
    schedule();
    
    // Should never return
    kernel_panic("Scheduler returned");
}
```

## Integration Methods

### Method A: Direct Execution (Single Process)

Use this for simple testing or single-process systems:

```c
// Load and execute /init immediately (does NOT return)
exec_usermode("/init", 0, NULL);
```

**Pros**: Simple, immediate
**Cons**: Blocking, no multitasking

### Method B: Process Creation (Multi-Process)

Use this for full scheduler integration:

```c
// Create process
char* argv[] = { "init", NULL };
process_t* init_proc = exec_create_process("init", "init", 1, argv);

if (!init_proc) {
    kernel_panic("Failed to create init process");
}

// Add to scheduler
scheduler_add_process(init_proc);

// Start scheduler
schedule();
```

**Pros**: Full multitasking, scheduler integration
**Cons**: Requires working scheduler

### Method C: Convenience Wrapper (Recommended)

Use the provided convenience function:

```c
// One-liner: creates process and adds to scheduler
if (exec_launch_init() != 0) {
    kernel_panic("Failed to launch /init");
}

// Start scheduler
schedule();
```

**Pros**: Simplest API, recommended for most use cases
**Cons**: Less control over process creation

## Testing Without Full Boot

### Test 1: Validate ELF Header

```c
void test_elf_loader(void) {
    uint64_t size;
    void* elf_data = initrd_get_file("init", &size);
    
    if (!elf_data) {
        kprintf("ERROR: /init not found in initrd\n");
        return;
    }
    
    const elf64_ehdr_t* ehdr = (const elf64_ehdr_t*)elf_data;
    
    if (elf_validate_header(ehdr)) {
        kprintf("✓ Valid ELF64 executable\n");
    } else {
        kprintf("✗ Invalid ELF format\n");
    }
}
```

### Test 2: Print ELF Information

```c
#include "include/elf.h"

void test_elf_info(void) {
    elf_print_info("init");
}
```

Output:
```
[ELF] File: init
  Size: 8192 bytes
  Type: 0x2
  Machine: 0x3e
  Entry: 0x0000000000401000
  Program headers: 2
  Section headers: 0
  Program Headers:
    [0] type=0x1 vaddr=0x0000000000400000 memsz=0x1000 flags=0x5
    [1] type=0x1 vaddr=0x0000000000401000 memsz=0x1000 flags=0x6
```

### Test 3: Load Without Executing

```c
void test_elf_load(void) {
    uint64_t entry, stack;
    int ret = elf_load("init", 0, NULL, &entry, &stack);
    
    if (ret == ELF_SUCCESS) {
        kprintf("✓ Loaded successfully\n");
        kprintf("  Entry: 0x%016lx\n", entry);
        kprintf("  Stack: 0x%016lx\n", stack);
    } else {
        kprintf("✗ Load failed: error %d\n", ret);
    }
}
```

### Test 4: Run Full Test Suite

```c
#include "include/elf.h"

void kernel_main(void) {
    // ... initialization ...
    
    // Run ELF loader tests
    elf_run_tests();
    
    // Continue with normal boot...
}
```

## Debugging

### Enable Verbose Logging

The ELF loader already has verbose logging. Look for `[ELF]` prefix in kernel output:

```
[ELF] Loading ELF: init
[ELF] Found file: init (size=8192 bytes)
[ELF] Valid ELF64 executable
[ELF]   Entry point: 0x0000000000401000
[ELF]   Program headers: 2 entries at offset 0x40
[ELF]   Segment: vaddr=0x0000000000400000 size=0x1000 pages=1 flags=R-X
[ELF]   Segment: vaddr=0x0000000000401000 size=0x1000 pages=1 flags=RW-
[ELF] Setting up user stack: 0x00007FFF00000000 - 0x00007FFFFFFFE000 (2048 pages)
[ELF] Stack initialized, RSP=0x00007FFFFFFFE000
[ELF] Load complete: entry=0x0000000000401000 stack=0x00007FFFFFFFE000
[EXEC] Jumping to user mode: entry=0x0000000000401000 stack=0x00007FFFFFFFE000
```

### Common Issues

#### Issue 1: File Not Found
```
[ELF] File not found: init
```

**Solution**: Ensure init is in initrd:
```bash
tar -tf initrd.tar | grep init
```

#### Issue 2: Invalid ELF
```
[ELF] Invalid magic number
```

**Solution**: Check init was built correctly:
```bash
file userspace/init/init
# Should output: ELF 64-bit LSB executable, x86-64
```

#### Issue 3: Out of Memory
```
[ELF]   ERROR: Out of physical memory
```

**Solution**: Ensure PMM is initialized and has sufficient memory:
```c
kprintf("Free memory: %lu MB\n", pmm_get_free_memory() / (1024*1024));
```

#### Issue 4: Triple Fault After User Mode Jump

**Symptoms**: System resets/triple faults immediately after "Jumping to user mode"

**Possible Causes**:
1. GDT not initialized properly
2. User segments not set up correctly
3. Page tables not mapped for user space
4. Stack not properly aligned

**Debug Steps**:
```c
// Before jumping to user mode:
kprintf("CR3: 0x%016lx\n", read_cr3());
kprintf("Entry: 0x%016lx\n", entry);
kprintf("Stack: 0x%016lx\n", stack);

// Check stack alignment
if (stack & 0xF) {
    kprintf("WARNING: Stack not 16-byte aligned!\n");
}

// Check entry point is mapped
void* phys = vmm_get_physical((void*)entry);
kprintf("Entry physical: %p\n", phys);
```

## Memory Requirements

### Per-Process Overhead

- **User Stack**: 8 MB (2048 pages)
- **Code/Data Segments**: Variable (typically 1-100 KB)
- **BSS**: Variable (zero-filled)

### Example: Minimal Init

```
Code:  4 KB (1 page)
Data:  4 KB (1 page)
Stack: 8 MB (2048 pages)
Total: ~8 MB (2050 pages)
```

### Multiple Processes

For 10 processes:
- 10 × 8 MB = 80 MB for stacks
- 10 × 8 KB = 80 KB for code/data (typical)
- **Total: ~81 MB**

## Performance Considerations

### Load Time

Typical load times on modern x86_64:

- **ELF Parsing**: <1 ms
- **Page Allocation**: 50 ms (for 2048 pages)
- **Memory Copy**: 1-5 ms (for <1 MB executable)
- **Total**: ~50-60 ms per process

### Optimization Tips

1. **Pre-allocate pages**: Allocate stack pages in batch
2. **Lazy loading**: Only load segments on first access (page fault handler)
3. **Shared pages**: Share read-only code segments between processes
4. **Smaller stack**: Reduce from 8 MB to 1 MB for most processes

## Next Steps

After basic ELF loading works:

1. **Syscall Interface** - Implement system calls (read, write, exit, fork, exec)
2. **Dynamic Linker** - Support shared libraries (ET_DYN, PT_INTERP)
3. **argv/envp** - Pass arguments and environment properly
4. **Per-Process Page Tables** - Create separate CR3 for each process
5. **Copy-on-Write** - Optimize fork() with COW
6. **Memory Protection** - NX bit, ASLR, stack canaries

## Example Boot Sequence

Complete example of kernel boot with ELF loader:

```c
void kernel_main(uint64_t multiboot_addr) {
    // 1. Early initialization
    serial_init();
    framebuffer_init();
    
    kprintf("AutomationOS Kernel\n");
    
    // 2. Memory management
    pmm_init(memory_map, map_count);
    vmm_init();
    heap_init();
    
    kprintf("Memory: %lu MB free\n", pmm_get_free_memory() / (1024*1024));
    
    // 3. CPU features
    gdt_init();
    idt_init();
    
    // 4. Process management
    process_init();
    scheduler_init();
    
    // 5. Load initrd
    uint64_t initrd_addr = ...;  // From multiboot
    uint64_t initrd_size = ...;
    
    initrd_init(initrd_addr, initrd_size);
    initrd_mount();
    
    kprintf("Initrd: %lu files\n", initrd_count);
    
    // 6. Launch init
    kprintf("\n=== Launching /init ===\n\n");
    
    if (exec_launch_init() != 0) {
        kernel_panic("Failed to launch /init");
    }
    
    // 7. Enable interrupts and start scheduling
    sti();
    
    kprintf("Starting scheduler...\n");
    schedule();
    
    // Should never return
    kernel_panic("Scheduler returned");
}
```

## API Reference

### Core Functions

```c
// Load ELF from initrd
int elf_load(const char* path, int argc, char** argv,
             uint64_t* entry_out, uint64_t* stack_out);

// Validate ELF header
int elf_validate_header(const elf64_ehdr_t* ehdr);

// Print ELF information (debugging)
void elf_print_info(const char* path);

// Execute ELF in user mode (does NOT return)
void exec_usermode(const char* path, int argc, char** argv);

// Create process from ELF
process_t* exec_create_process(const char* path, const char* name,
                                int argc, char** argv);

// Launch /init (convenience)
int exec_launch_init(void);

// Run test suite
void elf_run_tests(void);
```

### Error Codes

```c
#define ELF_SUCCESS       0   // Success
#define ELF_ERR_NOT_FOUND -1  // File not found
#define ELF_ERR_INVALID   -2  // Invalid ELF format
#define ELF_ERR_ARCH      -3  // Wrong architecture
#define ELF_ERR_NOMEM     -4  // Out of memory
#define ELF_ERR_PERM      -5  // Permission denied
```

## Support

If you encounter issues:

1. Check kernel logs for `[ELF]` and `[EXEC]` messages
2. Run `elf_run_tests()` to verify basic functionality
3. Verify initrd contains init: `initrd_list_files()`
4. Check memory: `pmm_get_free_memory()`
5. Verify GDT/IDT are initialized before loading

## License

Same as kernel (MIT).
