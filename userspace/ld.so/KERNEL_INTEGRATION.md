## Kernel Integration Guide for Dynamic Linker

This document describes the changes needed to integrate ld.so with the AutomationOS kernel.

## Overview

The kernel ELF loader currently only supports static executables (ET_EXEC). To support dynamic linking, it must:

1. Detect dynamically linked executables (ET_DYN with PT_INTERP)
2. Load the interpreter (ld.so) specified in PT_INTERP
3. Set up the auxiliary vector with program information
4. Transfer control to ld.so instead of the main program

## Required Changes

### 1. Update ELF Header Validation

**File:** `kernel/fs/elf_loader.c`

**Current code:**
```c
// Check file type (must be executable)
if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
    kprintf("[ELF] Not an executable (type=%d)\n", ehdr->e_type);
    return 0;
}
```

**Updated code:**
```c
// Check file type (must be executable or position-independent executable)
if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
    kprintf("[ELF] Not an executable (type=%d)\n", ehdr->e_type);
    return 0;
}

// ET_DYN executables are position-independent (PIE)
int is_pie = (ehdr->e_type == ET_DYN);
```

### 2. Find PT_INTERP Segment

**Add this function to `elf_loader.c`:**

```c
/**
 * Find PT_INTERP segment (dynamic linker path)
 *
 * @param ehdr ELF header
 * @param elf_data Pointer to ELF file data
 * @param interp_out Output buffer for interpreter path
 * @param interp_size Size of output buffer
 * @return 0 if found, negative if not found
 */
static int elf_find_interpreter(const elf64_ehdr_t* ehdr, const void* elf_data,
                                 char* interp_out, size_t interp_size) {
    const elf64_phdr_t* phdr = (const elf64_phdr_t*)((uint8_t*)elf_data + ehdr->e_phoff);

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_INTERP) {
            // Found interpreter path
            const char* interp_path = (const char*)((uint8_t*)elf_data + phdr[i].p_offset);
            size_t path_len = phdr[i].p_filesz;

            if (path_len >= interp_size) {
                return -1;  // Path too long
            }

            memcpy(interp_out, interp_path, path_len);
            interp_out[path_len] = '\0';

            kprintf("[ELF] Interpreter: %s\n", interp_out);
            return 0;
        }
    }

    return -1;  // No PT_INTERP found
}
```

### 3. Load Interpreter (ld.so)

**Add to `elf_load()` function:**

```c
// Check for PT_INTERP (dynamic linker)
char interp_path[256];
int has_interp = (elf_find_interpreter(ehdr, elf_data, interp_path, sizeof(interp_path)) == 0);

uint64_t interp_entry = 0;
uint64_t interp_base = 0;

if (has_interp) {
    kprintf("[ELF] Loading interpreter: %s\n", interp_path);

    // Load interpreter ELF file
    uint64_t interp_size = 0;
    void* interp_data = initrd_get_file(interp_path, &interp_size);

    if (!interp_data) {
        kprintf("[ELF] ERROR: Interpreter not found: %s\n", interp_path);
        return ELF_ERR_NOT_FOUND;
    }

    // Validate interpreter ELF
    const elf64_ehdr_t* interp_ehdr = (const elf64_ehdr_t*)interp_data;
    if (!elf_validate_header(interp_ehdr)) {
        kprintf("[ELF] ERROR: Invalid interpreter ELF\n");
        return ELF_ERR_INVALID;
    }

    // Choose base address for interpreter (e.g., 0x7000'0000'0000)
    interp_base = 0x700000000000ULL;

    // Load interpreter PT_LOAD segments
    const elf64_phdr_t* interp_phdr = (const elf64_phdr_t*)((uint8_t*)interp_data + interp_ehdr->e_phoff);

    for (int i = 0; i < interp_ehdr->e_phnum; i++) {
        if (interp_phdr[i].p_type == PT_LOAD) {
            // Adjust addresses by base
            elf64_phdr_t adjusted_phdr = interp_phdr[i];
            adjusted_phdr.p_vaddr += interp_base;

            int ret = elf_load_segment(&adjusted_phdr, interp_data);
            if (ret != ELF_SUCCESS) {
                return ret;
            }
        }
    }

    // Interpreter entry point
    interp_entry = interp_base + interp_ehdr->e_entry;
    kprintf("[ELF] Interpreter loaded at base=0x%016lx entry=0x%016lx\n",
            interp_base, interp_entry);
}
```

### 4. Set Up Auxiliary Vector

**Add auxiliary vector structure to stack setup:**

```c
/**
 * Setup auxiliary vector on user stack
 *
 * Auxiliary vector provides information from kernel to ld.so.
 */
static void elf_setup_auxv(uint64_t* sp_ptr, const elf64_ehdr_t* ehdr,
                           const void* elf_data, uint64_t interp_base) {
    uint64_t sp = *sp_ptr;
    
    elf64_auxv_t* auxv = (elf64_auxv_t*)sp;

    // AT_PHDR - Program headers address
    auxv->a_type = AT_PHDR;
    auxv->a_un.a_val = (uint64_t)elf_data + ehdr->e_phoff;
    auxv++;

    // AT_PHENT - Size of program header entry
    auxv->a_type = AT_PHENT;
    auxv->a_un.a_val = ehdr->e_phentsize;
    auxv++;

    // AT_PHNUM - Number of program headers
    auxv->a_type = AT_PHNUM;
    auxv->a_un.a_val = ehdr->e_phnum;
    auxv++;

    // AT_PAGESZ - System page size
    auxv->a_type = AT_PAGESZ;
    auxv->a_un.a_val = PAGE_SIZE;
    auxv++;

    // AT_BASE - Interpreter base address
    if (interp_base) {
        auxv->a_type = AT_BASE;
        auxv->a_un.a_val = interp_base;
        auxv++;
    }

    // AT_ENTRY - Program entry point
    auxv->a_type = AT_ENTRY;
    auxv->a_un.a_val = ehdr->e_entry;
    auxv++;

    // AT_NULL - End of vector
    auxv->a_type = AT_NULL;
    auxv->a_un.a_val = 0;
    auxv++;

    *sp_ptr = (uint64_t)auxv;
}
```

**Update `elf_setup_stack()` to include auxv:**

```c
static int elf_setup_stack(int argc, char** argv, uint64_t* stack_out,
                           const elf64_ehdr_t* ehdr, const void* elf_data,
                           uint64_t interp_base) {
    // ... existing code ...

    uint64_t sp = USER_STACK_TOP;
    sp &= ~0xFULL;  // 16-byte align

    // Push auxiliary vector
    sp -= sizeof(elf64_auxv_t) * 16;  // Reserve space
    uint64_t auxv_start = sp;
    elf_setup_auxv(&sp, ehdr, elf_data, interp_base);

    // Push NULL (end of envp)
    sp -= 8;
    *(uint64_t*)sp = 0;

    // TODO: Push environment variables
    // For now, just NULL

    // Push NULL (end of argv)
    sp -= 8;
    *(uint64_t*)sp = 0;

    // Push argv pointers (reverse order)
    for (int i = argc - 1; i >= 0; i--) {
        sp -= 8;
        *(uint64_t*)sp = (uint64_t)argv[i];  // TODO: Copy strings to stack
    }

    // Push argc
    sp -= 8;
    *(uint64_t*)sp = argc;

    // Final alignment
    sp &= ~0xFULL;

    *stack_out = sp;
    return ELF_SUCCESS;
}
```

### 5. Choose Entry Point

**Update `elf_load()` to return correct entry point:**

```c
// Determine entry point
if (has_interp && interp_entry) {
    // Jump to interpreter, which will jump to main program
    *entry_out = interp_entry;
    kprintf("[ELF] Entry point: ld.so at 0x%016lx\n", interp_entry);
} else {
    // Static executable, jump directly to main entry
    *entry_out = ehdr->e_entry;
    kprintf("[ELF] Entry point: main at 0x%016lx\n", ehdr->e_entry);
}
```

## Additional Syscalls Needed

The dynamic linker requires these syscalls for file I/O:

### 1. open() - Syscall 2

```c
int sys_open(const char* path, int flags, int mode) {
    // Open file from VFS
    // Return file descriptor
}
```

### 2. read() - Syscall 0

```c
ssize_t sys_read(int fd, void* buf, size_t count) {
    // Read from file descriptor
    // Return bytes read
}
```

### 3. mmap() - Syscall 9

```c
void* sys_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    // Map file into memory
    // Allocate pages and map to virtual address
    // Return mapped address
}
```

### 4. munmap() - Syscall 11

```c
int sys_munmap(void* addr, size_t length) {
    // Unmap memory region
    // Free pages
    return 0;
}
```

### 5. stat() - Syscall 4

```c
int sys_stat(const char* path, struct stat* statbuf) {
    // Get file information
    // Check if file exists
    return 0;
}
```

## Testing the Integration

### 1. Build ld.so

```bash
cd userspace/ld.so
make
make install  # Copies to initrd/lib64/
```

### 2. Build a Test Shared Library

```bash
# Create test library
echo 'int add(int a, int b) { return a + b; }' > libtest.c
x86_64-elf-gcc -fPIC -c libtest.c -o libtest.o
x86_64-elf-ld -shared -o libtest.so.1 libtest.o -soname libtest.so.1
```

### 3. Build a Test Program

```bash
# Create test program
cat > test.c << 'EOF'
extern int add(int a, int b);

int _start(void) {
    int result = add(2, 3);
    // Exit with result
    __asm__ volatile("mov $1, %rax; mov %0, %rdi; syscall" : : "r"((long)result));
}
EOF

# Compile and link dynamically
x86_64-elf-gcc -fPIC -c test.c -o test.o
x86_64-elf-ld -dynamic-linker /lib64/ld-linux-x86-64.so.2 \
    -o test test.o -L. -ltest
```

### 4. Add to Initrd and Boot

```bash
# Add to initrd
cp libtest.so.1 ../../initrd/lib/
cp test ../../initrd/bin/

# Rebuild initrd
bash scripts/mkinitrd.sh

# Boot
bash scripts/run-qemu.sh
```

### 5. Verify in Shell

```
> /bin/test
# Should exit with code 5 (2+3)
```

## Debugging Tips

### Enable Debug Output

In `elf_loader.c`:
```c
#define ELF_DEBUG 1
```

In `ld.so/linker.c`:
```c
linker_ctx.debug = 1;
```

### Check Auxiliary Vector

Add debug output in `ld_main.c`:
```c
// Print auxiliary vector
elf64_auxv_t* a = auxv;
while (a->a_type != AT_NULL) {
    kprintf("auxv[%d] = 0x%lx\n", a->a_type, a->a_un.a_val);
    a++;
}
```

### Verify Relocations

Enable relocation debug:
```c
#define RELOC_DEBUG 1
```

## Common Issues

### Issue: "Interpreter not found"
**Solution:** Ensure ld.so is in initrd at `/lib64/ld-linux-x86-64.so.2`

### Issue: "Invalid ELF header" for interpreter
**Solution:** Check ld.so was built correctly with `-shared -fpie`

### Issue: Segfault in ld.so
**Solution:** 
- Verify auxiliary vector is set up correctly
- Check stack alignment (must be 16-byte aligned)
- Ensure ld.so was loaded at correct base address

### Issue: "Undefined symbol"
**Solution:**
- Ensure shared library is in search path (`/lib`, `/usr/lib`)
- Check library was linked with `-shared`
- Verify symbols are exported (not static)

### Issue: Relocation fails
**Solution:**
- Check relocation type is supported
- Verify symbol exists in dependency
- Enable debug output to see which relocation fails

## Performance Considerations

### Load Time
- Each shared library: ~5-10ms
- Symbol resolution: ~0.1ms per symbol
- Total overhead: ~20-50ms for typical program

### Memory Overhead
- ld.so itself: ~64KB
- Per-library metadata: ~8KB
- PLT/GOT: ~16 bytes per function

### Recommendations
- Use RTLD_LAZY for fast startup
- Combine related libraries
- Use GNU hash for faster symbol lookup

## Next Steps

After basic integration works:

1. **Convert libc to shared library**
   - See `CONVERT_LIBC_SHARED.md`

2. **Convert libgui to shared library**
   - Similar process as libc

3. **Update build system**
   - Add `-fPIC` to all library builds
   - Generate `.so` instead of `.a`

4. **Test with real applications**
   - Terminal, file manager, etc.
   - Measure load time and memory usage

## References

- ELF Specification: https://refspecs.linuxfoundation.org/elf/elf.pdf
- System V ABI: https://refspecs.linuxfoundation.org/elf/x86_64-abi-0.99.pdf
- Linux Kernel ELF Loader: `fs/binfmt_elf.c`
- glibc ld.so: `elf/rtld.c`
