# Phase 4: Init Process Implementation Plan

**Objective:** Enable kernel-to-userspace transition by creating the init process (PID 1)

**Current Status:** Kernel initializes all subsystems but never spawns init. System enters idle loop at line 141 of kernel/kernel.c.

**Target:** Boot from kernel → init process → shell

## Architecture Overview

```
kernel_main()
    ↓
[Current: All subsystems initialized]
    ↓
[NEW] VFS Initialization
    ↓
[NEW] Mount Initrd
    ↓
[NEW] Load /sbin/init ELF
    ↓
[NEW] Create Init Process (PID 1)
    ↓
[NEW] Setup User Mode Context
    ↓
[NEW] Start Scheduler
    ↓
Init Process Runs (Userspace)
    ↓
Init Spawns Shell
```

## Required Components

### Component 1: Virtual Filesystem (VFS)

**File:** `kernel/fs/vfs.c`, `kernel/include/vfs.h`

**Purpose:** Provide unified interface to mount and access files from initrd

**Key Functions:**

```c
// VFS initialization
void vfs_init(void);

// Mount a filesystem at a path
int vfs_mount(const char* device, const char* path, const char* fstype);

// Mount root filesystem
int vfs_mount_root(const char* fstype, const char* device);

// File operations
int vfs_open(const char* path, int flags);
int vfs_read(int fd, void* buffer, size_t size);
int vfs_close(int fd);

// Directory operations
int vfs_stat(const char* path, struct stat* st);
int vfs_readdir(int fd, struct dirent* entry);
```

**Data Structures:**

```c
// VFS node (inode)
typedef struct vfs_node {
    char name[256];
    uint32_t inode;
    uint32_t size;
    uint32_t type;  // FILE, DIRECTORY, etc.
    uint32_t permissions;
    uint32_t uid, gid;
    struct vfs_node* parent;
    struct vfs_node* next;
    struct vfs_node* children;
    
    // Operations
    int (*read)(struct vfs_node*, uint64_t offset, uint64_t size, void* buffer);
    int (*write)(struct vfs_node*, uint64_t offset, uint64_t size, void* buffer);
    int (*open)(struct vfs_node*);
    int (*close)(struct vfs_node*);
} vfs_node_t;

// Mount point
typedef struct mount_point {
    char path[256];
    char device[256];
    char fstype[64];
    vfs_node_t* root;
    struct mount_point* next;
} mount_point_t;
```

**Implementation Steps:**

1. Create basic VFS structure (2 hours)
2. Implement path resolution (1 hour)
3. Implement mount/unmount (1 hour)
4. Implement file operations (2 hours)
5. Test with mock filesystem (1 hour)

**Estimated Time:** 7 hours

### Component 2: Initrd Filesystem Driver

**File:** `kernel/fs/initrd.c`, `kernel/include/initrd.h`

**Purpose:** Parse TAR format initrd and expose files via VFS

**TAR Format (POSIX ustar):**

```c
// TAR header structure (512 bytes)
typedef struct tar_header {
    char filename[100];    // File name
    char mode[8];          // File mode (octal)
    char uid[8];           // Owner user ID (octal)
    char gid[8];           // Owner group ID (octal)
    char size[12];         // File size in bytes (octal)
    char mtime[12];        // Last modification time (octal)
    char checksum[8];      // Header checksum (octal)
    char typeflag;         // File type (0=file, 5=directory)
    char linkname[100];    // Link name (for symlinks)
    char magic[6];         // "ustar\0"
    char version[2];       // "00"
    char uname[32];        // Owner user name
    char gname[32];        // Owner group name
    char devmajor[8];      // Device major number
    char devminor[8];      // Device minor number
    char prefix[155];      // Filename prefix
    char padding[12];      // Padding to 512 bytes
} PACKED tar_header_t;
```

**Key Functions:**

```c
// Mount initrd at address/size
int initrd_mount(void* addr, uint64_t size);

// Parse TAR archive and build VFS tree
vfs_node_t* initrd_parse_tar(void* tar_data, uint64_t size);

// Read file from initrd
int initrd_read(vfs_node_t* node, uint64_t offset, uint64_t size, void* buffer);

// Utility: convert octal string to int
uint64_t tar_octal_to_int(const char* str, size_t len);
```

**Implementation Steps:**

1. Parse TAR headers (2 hours)
2. Build VFS tree from TAR (2 hours)
3. Implement read operations (1 hour)
4. Test with sample initrd (1 hour)

**Estimated Time:** 6 hours

### Component 3: Userspace ELF Loader

**File:** `kernel/loader/elf_user.c`, `kernel/include/elf_loader.h`

**Purpose:** Load ELF64 binaries into user process address space

**Key Functions:**

```c
// Load ELF binary from VFS into process
// Returns entry point address or NULL on failure
void* elf_load_userspace(process_t* proc, const char* path);

// Load ELF from memory buffer
void* elf_load_from_memory(process_t* proc, void* elf_data, uint64_t size);

// Parse ELF headers
int elf_parse_headers(void* elf_data, elf_header_t** ehdr, elf_program_header_t** phdrs);

// Load PT_LOAD segments into process memory
int elf_load_segments(process_t* proc, void* elf_data, elf_program_header_t* phdrs, int phnum);
```

**ELF Structures (reuse from bootloader):**

```c
typedef struct {
    uint32_t e_ident_magic;      // 0x7F 'E' 'L' 'F'
    uint8_t e_ident_class;       // 1=32-bit, 2=64-bit
    uint8_t e_ident_data;        // 1=little-endian, 2=big-endian
    uint8_t e_ident_version;     // ELF version
    uint8_t e_ident_pad[9];
    uint16_t e_type;             // 1=relocatable, 2=executable, 3=shared
    uint16_e_machine;          // 0x3E = x86-64
    uint32_t e_version;
    uint64_t e_entry;            // Entry point address
    uint64_t e_phoff;            // Program header offset
    uint64_t e_shoff;            // Section header offset
    uint32_t e_flags;
    uint16_t e_ehsize;           // ELF header size
    uint16_t e_phentsize;        // Program header entry size
    uint16_t e_phnum;            // Number of program headers
    uint16_t e_shentsize;        // Section header entry size
    uint16_t e_shnum;            // Number of section headers
    uint16_t e_shstrndx;         // Section header string table index
} PACKED elf_header_t;

typedef struct {
    uint32_t p_type;             // Segment type (PT_LOAD=1)
    uint32_t p_flags;            // Segment flags (R=4, W=2, X=1)
    uint64_t p_offset;           // Offset in file
    uint64_t p_vaddr;            // Virtual address
    uint64_t p_paddr;            // Physical address (ignored)
    uint64_t p_filesz;           // Size in file
    uint64_t p_memsz;            // Size in memory
    uint64_t p_align;            // Alignment
} PACKED elf_program_header_t;
```

**Loading Process:**

1. Open file via VFS
2. Read ELF header (52 bytes)
3. Validate magic, class (64-bit), machine (x86-64)
4. Read program headers
5. For each PT_LOAD segment:
   - Allocate pages in process address space
   - Map pages (setup page tables)
   - Read segment data from file
   - Zero BSS region (if p_memsz > p_filesz)
6. Return entry point (e_entry)

**Implementation Steps:**

1. Port ELF structures from bootloader (1 hour)
2. Implement VFS-based reading (1 hour)
3. Implement user space memory allocation (2 hours)
4. Implement page table setup for user space (3 hours)
5. Test with simple user binary (1 hour)

**Estimated Time:** 8 hours

### Component 4: User Mode Setup

**File:** `kernel/core/sched/process.c` (extend existing)

**Purpose:** Setup process for user mode execution

**Required Changes:**

```c
// In process_create(), add user mode support:

// Allocate user page directory (CR3)
proc->user_cr3 = (uint64_t)pmm_alloc_page();
if (!proc->user_cr3) {
    // cleanup...
    return NULL;
}

// Setup user page tables
// Identity map kernel (higher half)
// Map user space (0x400000 - 0x800000 for now)
paging_setup_user_space(proc);

// Allocate user stack (8KB)
proc->user_stack = pmm_alloc_page();
if (!proc->user_stack) {
    // cleanup...
    return NULL;
}

// Map user stack at canonical user address (e.g., 0x7FFFFFFFE000)
#define USER_STACK_TOP 0x7FFFFFFFE000
paging_map_page(proc->user_cr3, USER_STACK_TOP - PAGE_SIZE, 
                (uint64_t)proc->user_stack, 
                PAGE_USER | PAGE_WRITE | PAGE_PRESENT);

// Setup initial context for user mode
proc->context.cs = 0x23;  // User code segment (GDT entry 3, RPL=3)
proc->context.ss = 0x1B;  // User data segment (GDT entry 4, RPL=3)
proc->context.rflags = 0x202;  // IF=1, IOPL=0
proc->context.rsp = USER_STACK_TOP - 16;  // Top of user stack (aligned)
proc->context.rip = entry_point;  // User entry point
proc->context.cr3 = proc->user_cr3;  // User page directory
```

**Page Table Setup:**

```c
// kernel/arch/x86_64/paging.c

// Setup user space page tables
void paging_setup_user_space(process_t* proc) {
    uint64_t pml4 = proc->user_cr3;
    
    // 1. Identity map kernel (higher half: 0xFFFFFFFF80000000+)
    //    Copy kernel mappings from kernel_cr3
    uint64_t* kernel_pml4 = (uint64_t*)read_cr3();
    uint64_t* user_pml4 = (uint64_t*)pml4;
    
    // Copy upper half entries (kernel space)
    for (int i = 256; i < 512; i++) {
        user_pml4[i] = kernel_pml4[i];
    }
    
    // 2. Map user space (lower half: 0x400000 - 0x800000)
    //    Will be filled in by ELF loader
    //    For now, just ensure user space is accessible
}

// Map a single page in process address space
int paging_map_page(uint64_t cr3, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    // Extract page table indices from virtual address
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx = (vaddr >> 21) & 0x1FF;
    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
    
    // Walk page tables, allocating as needed
    uint64_t* pml4 = (uint64_t*)cr3;
    
    // PML4 -> PDPT
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        uint64_t new_pdpt = (uint64_t)pmm_alloc_page();
        memset((void*)new_pdpt, 0, PAGE_SIZE);
        pml4[pml4_idx] = new_pdpt | flags | PAGE_PRESENT;
    }
    uint64_t* pdpt = (uint64_t*)(pml4[pml4_idx] & ~0xFFF);
    
    // PDPT -> PD
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        uint64_t new_pd = (uint64_t)pmm_alloc_page();
        memset((void*)new_pd, 0, PAGE_SIZE);
        pdpt[pdpt_idx] = new_pd | flags | PAGE_PRESENT;
    }
    uint64_t* pd = (uint64_t*)(pdpt[pdpt_idx] & ~0xFFF);
    
    // PD -> PT
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        uint64_t new_pt = (uint64_t)pmm_alloc_page();
        memset((void*)new_pt, 0, PAGE_SIZE);
        pd[pd_idx] = new_pt | flags | PAGE_PRESENT;
    }
    uint64_t* pt = (uint64_t*)(pd[pd_idx] & ~0xFFF);
    
    // PT -> Page
    pt[pt_idx] = paddr | flags | PAGE_PRESENT;
    
    return 0;
}
```

**Implementation Steps:**

1. Implement user CR3 allocation (1 hour)
2. Implement page table walker (2 hours)
3. Implement page mapping function (2 hours)
4. Setup user stack mapping (1 hour)
5. Setup segment registers (1 hour)
6. Test context switch to user mode (2 hours)

**Estimated Time:** 9 hours

### Component 5: Kernel Integration

**File:** `kernel/kernel.c` (lines 134-143)

**Purpose:** Wire everything together in kernel_main()

**Implementation:**

```c
// kernel/kernel.c (replace lines 134-143)

kprintf("[KERNEL] Starting init process...\n");

// 1. Initialize VFS
PERF_TIMER_START();
vfs_init();
PERF_TIMER_END("vfs_init");
kprintf("[KERNEL] VFS initialized\n");

// 2. Mount initrd as root
if (!boot_info->initrd_addr || !boot_info->initrd_size) {
    kernel_panic("No initrd provided by bootloader");
}

PERF_TIMER_START();
int ret = initrd_mount((void*)boot_info->initrd_addr, boot_info->initrd_size);
if (ret < 0) {
    kernel_panic("Failed to mount initrd");
}
PERF_TIMER_END("initrd_mount");
kprintf("[KERNEL] Initrd mounted (%lu bytes)\n", boot_info->initrd_size);

ret = vfs_mount_root("initrd", "/");
if (ret < 0) {
    kernel_panic("Failed to mount root filesystem");
}
kprintf("[KERNEL] Root filesystem mounted\n");

// 3. List initrd contents (debug)
kprintf("[KERNEL] Initrd contents:\n");
vfs_list_directory("/");

// 4. Create init process structure
PERF_TIMER_START();
process_t* init = process_create("init", NULL);  // Entry point set later
if (!init) {
    kernel_panic("Failed to create init process");
}

if (init->pid != 1) {
    kernel_panic("Init must be PID 1 (got %d)", init->pid);
}
PERF_TIMER_END("process_create");
kprintf("[KERNEL] Init process created (PID %d)\n", init->pid);

// 5. Load init binary from initrd
PERF_TIMER_START();
void* init_entry = elf_load_userspace(init, "/sbin/init");
if (!init_entry) {
    kernel_panic("Failed to load /sbin/init");
}
PERF_TIMER_END("elf_load");
kprintf("[KERNEL] Init binary loaded (entry: %p)\n", init_entry);

// 6. Setup init context (entry point now known)
init->context.rip = (uint64_t)init_entry;

// 7. Mark as ready to run
init->state = PROCESS_READY;
kprintf("[KERNEL] Init process ready\n");

// 8. Add to scheduler
scheduler_add_process(init);
kprintf("[KERNEL] Init added to scheduler\n");

// 9. Calculate total boot time
uint64_t boot_end = rdtsc();
uint64_t boot_cycles = boot_end - boot_start;
kprintf("\n");
kprintf("[BOOT] Kernel initialization complete\n");
kprintf("[BOOT] Total time: %llu cycles (%.2f ms)\n",
        boot_cycles, cycles_to_ms(boot_cycles));
kprintf("\n");

// 10. Enable interrupts and start scheduling
sti();
kprintf("[KERNEL] Starting scheduler...\n");
scheduler_start();  // Never returns

// Should never reach here
kernel_panic("Scheduler returned to kernel_main");
```

**Implementation Steps:**

1. Add VFS/initrd calls (1 hour)
2. Add ELF loading call (1 hour)
3. Add process setup (1 hour)
4. Add error handling (1 hour)
5. Test integration (2 hours)

**Estimated Time:** 6 hours

## Testing Strategy

### Test 1: VFS Only

**Objective:** Verify VFS can be initialized

```c
void test_vfs() {
    vfs_init();
    kprintf("[TEST] VFS initialized\n");
    
    // Create mock filesystem
    vfs_node_t* root = vfs_create_node("/", VFS_DIRECTORY);
    vfs_node_t* bin = vfs_create_node("/bin", VFS_DIRECTORY);
    vfs_node_t* init = vfs_create_node("/bin/init", VFS_FILE);
    
    // Test path resolution
    vfs_node_t* found = vfs_resolve_path("/bin/init");
    if (found == init) {
        kprintf("[TEST] Path resolution OK\n");
    }
}
```

### Test 2: Initrd Mounting

**Objective:** Verify initrd can be parsed and mounted

**Create test initrd:**

```bash
# Create minimal initrd with just /sbin/init
mkdir -p initrd_test/sbin
echo "Hello from init" > initrd_test/sbin/init
tar -cf test.tar -C initrd_test .
```

**Test code:**

```c
void test_initrd() {
    // Load test.tar into memory
    void* tar_data = load_file("test.tar");
    uint64_t tar_size = get_file_size("test.tar");
    
    // Mount
    int ret = initrd_mount(tar_data, tar_size);
    if (ret == 0) {
        kprintf("[TEST] Initrd mounted\n");
        
        // List contents
        vfs_list_directory("/");
        
        // Read /sbin/init
        int fd = vfs_open("/sbin/init", O_RDONLY);
        if (fd >= 0) {
            char buffer[256];
            int bytes = vfs_read(fd, buffer, sizeof(buffer));
            kprintf("[TEST] Read %d bytes: %s\n", bytes, buffer);
            vfs_close(fd);
        }
    }
}
```

### Test 3: ELF Loading

**Objective:** Verify ELF loader can load user binary

**Create minimal user program:**

```c
// test_user.c
void _start() {
    // Exit immediately
    asm volatile("mov $60, %rax; xor %rdi, %rdi; syscall");
}
```

```bash
# Compile as static user binary
x86_64-elf-gcc -nostdlib -static -Ttext=0x400000 test_user.c -o test_user
```

**Test code:**

```c
void test_elf_loader() {
    process_t* proc = process_create("test", NULL);
    
    void* entry = elf_load_userspace(proc, "/test_user");
    if (entry) {
        kprintf("[TEST] ELF loaded, entry: %p\n", entry);
        kprintf("[TEST] Process CR3: %p\n", (void*)proc->context.cr3);
        kprintf("[TEST] Process RSP: %p\n", (void*)proc->context.rsp);
    }
}
```

### Test 4: User Mode Execution

**Objective:** Verify process can execute in user mode

**Simple test program that does syscall:**

```c
// test_syscall.c
void _start() {
    // Write "Hello" to console (syscall 1)
    const char* msg = "Hello from userspace!\n";
    asm volatile(
        "mov $1, %%rax;"     // syscall number (write)
        "mov $1, %%rdi;"     // fd (stdout)
        "mov %0, %%rsi;"     // buffer
        "mov $22, %%rdx;"    // length
        "syscall;"
        :
        : "r"(msg)
        : "rax", "rdi", "rsi", "rdx"
    );
    
    // Exit (syscall 60)
    asm volatile(
        "mov $60, %%rax;"    // syscall number (exit)
        "xor %%rdi, %%rdi;"  // status = 0
        "syscall;"
        :
        :
        : "rax", "rdi"
    );
}
```

**Test code:**

```c
void test_usermode() {
    process_t* proc = process_create("test_user", NULL);
    void* entry = elf_load_userspace(proc, "/test_user");
    
    if (entry) {
        proc->state = PROCESS_READY;
        scheduler_add_process(proc);
        
        // Start scheduler
        sti();
        scheduler_start();
    }
}
```

**Expected output:**

```
[KERNEL] Starting scheduler...
[SCHEDULER] Switching to process 1 (test_user)
Hello from userspace!
[SCHEDULER] Process 1 exited with status 0
```

### Test 5: Full Init Boot

**Objective:** Boot real init process

**Build init binary:**

```bash
cd userspace/init
make
# Creates init.elf
```

**Create initrd with init:**

```bash
cd tools
./mkinitrd -o ../build/initrd.img -f ../userspace/init/init -f ../userspace/libc/libc.so
```

**Add to ESP:**

```bash
sudo mount esp.img mnt
sudo cp build/initrd.img mnt/EFI/BOOT/
sudo umount mnt
```

**Boot and verify:**

```
[BOOTLOADER] Loading initrd...
[BOOTLOADER] Initrd loaded (1024 KB)
[KERNEL] Initrd mounted
[KERNEL] Init process created (PID 1)
[KERNEL] Init binary loaded
[KERNEL] Starting scheduler...

=====================================
   AutomationOS Init Process (PID 1)
=====================================

[INIT] Running as PID 1
[INIT] Initializing system...
[INIT] Spawning shell...
[INIT] Shell started with PID 2

$ 
```

## Implementation Timeline

| Task | Time Estimate | Dependencies |
|------|--------------|--------------|
| VFS Core | 7 hours | None |
| Initrd Driver | 6 hours | VFS |
| ELF Loader | 8 hours | VFS |
| User Mode Setup | 9 hours | Process management |
| Kernel Integration | 6 hours | All above |
| Testing & Debug | 8 hours | All above |
| **Total** | **44 hours** | |

**Realistic timeline:** 5-6 full days of development

## Success Criteria

- [ ] VFS initializes and mounts initrd
- [ ] Initrd contents can be listed and read
- [ ] /sbin/init ELF loads successfully
- [ ] Init process created with PID 1
- [ ] Process executes in user mode (ring 3)
- [ ] Init prints startup message
- [ ] Init can fork() and execve()
- [ ] Shell spawns and displays prompt
- [ ] System boots from UEFI → kernel → init → shell

## Risk Mitigation

### Risk 1: Page Table Corruption
**Mitigation:** Validate page tables after each mapping, add assertions

### Risk 2: Triple Fault on User Mode Switch
**Mitigation:** Test with minimal user program first, verify GDT/TSS setup

### Risk 3: Syscall Handler Failures
**Mitigation:** Test syscalls incrementally (exit, write, fork, exec)

### Risk 4: Memory Leaks in ELF Loader
**Mitigation:** Track all allocations, implement cleanup on failure

### Risk 5: Initrd Parsing Errors
**Mitigation:** Validate TAR format, add checksums, test with known-good initrd

## Next Steps

1. **Start with VFS (lowest risk)**
   - Implement basic data structures
   - Test path resolution
   - Add mount/unmount

2. **Add Initrd driver**
   - Parse TAR headers
   - Build VFS tree
   - Test reading files

3. **Implement ELF loader**
   - Port from bootloader
   - Add user space memory allocation
   - Test with simple binary

4. **Setup user mode**
   - Implement page table management
   - Setup segment registers
   - Test context switch

5. **Integrate and test**
   - Wire everything in kernel_main()
   - Create test initrd
   - Boot and debug

6. **Full system test**
   - Boot with real init
   - Verify shell spawns
   - Measure boot time

**Start Date:** TBD  
**Target Completion:** 5-6 days from start  
**First Milestone:** VFS + Initrd working (2 days)  
**Second Milestone:** ELF loader working (2 days)  
**Final Milestone:** Init process boots (1-2 days)
