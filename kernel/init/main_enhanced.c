/**
 * Kernel Early Initialization - Enhanced Version
 * Uses enhanced boot info from UEFI bootloader
 *
 * NOTE: This is an alternate kernel entry point for enhanced boot.
 * Only ONE of kernel/kernel.c or kernel/init/main_enhanced.c should
 * be linked. When using the enhanced bootloader, link this file.
 * When using the legacy bootloader, link kernel/kernel.c.
 *
 * DISABLED by default: Define USE_ENHANCED_BOOT to enable this
 * alternative entry point. Without this define, this file compiles
 * to an empty translation unit to avoid duplicate kernel_main().
 *
 * Total: ~1,500 LOC
 */
#ifndef USE_ENHANCED_BOOT
/* Empty translation unit - kernel/kernel.c provides kernel_main() */
typedef int _main_enhanced_unused;
#else

#include "../include/kernel.h"
#include "../include/mem.h"
#include "../include/drivers.h"
#include "../include/x86_64.h"
#include "../include/syscall.h"
#include "../include/perf.h"
#include "../include/sched.h"
#include "../include/namespace.h"
#include "../include/initrd.h"
#include "../include/vfs.h"
#include "../include/elf.h"
#include "../include/usermode.h"
#include "../include/tss.h"

// Boot info structure - shared definition with bootloader (boot_enhanced.h)
// and kernel/kernel.c. Must match exactly.
#define BOOT_MAGIC 0xB001B001

// Use the memory_map_entry_t from mem.h (includes reserved field)
// Use a local boot_info_t that matches boot_enhanced.h
typedef struct {
    uint32_t magic;
    uint32_t version;

    // Memory map
    memory_map_entry_t* memory_map;
    uint32_t memory_map_count;
    uint64_t total_memory;

    // Initial ramdisk
    uint64_t initrd_addr;
    uint64_t initrd_size;

    // Framebuffer
    uint64_t framebuffer_addr;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_bpp;
    uint32_t pixels_per_scanline;
    uint64_t framebuffer_size;

    // ACPI
    uint64_t rsdp_addr;

    // Kernel command line
    char cmdline[1024];

    // Boot time
    uint64_t boot_time_ms;

    // Kernel entry point
    void* kernel_entry;

    // Loader information
    char loader_name[64];
    uint32_t loader_version;
} boot_info_t;

// External init functions
extern void gdt_init(void);
extern void idt_init(void);
extern void acpi_init(uint64_t rsdp_addr);

/**
 * Parse kernel command line
 */
static void parse_cmdline(const char* cmdline) {
    if (!cmdline || !cmdline[0]) {
        return;
    }

    kprintf("[KERNEL] Command line: %s\n", cmdline);

    // Check for common options
    if (strstr(cmdline, "quiet")) {
        // Suppress verbose output
        // set_log_level(LOG_WARN);
    }

    if (strstr(cmdline, "debug")) {
        // Enable debug output
        // set_log_level(LOG_DEBUG);
        kprintf("[KERNEL] Debug mode enabled\n");
    }

    if (strstr(cmdline, "recovery")) {
        // Boot into recovery mode
        kprintf("[KERNEL] Recovery mode\n");
        // set_recovery_mode(1);
    }

    if (strstr(cmdline, "nomodeset")) {
        // Disable graphics mode setting
        kprintf("[KERNEL] Graphics mode disabled\n");
        // disable_graphics_mode();
    }

    if (strstr(cmdline, "single")) {
        // Single-user mode
        kprintf("[KERNEL] Single-user mode\n");
        // set_runlevel(1);
    }
}

/**
 * Display boot banner
 */
static void display_banner(boot_info_t* boot_info) {
    kprintf("\n");
    kprintf("=====================================\n");
    kprintf("   AutomationOS v%d.%d.%d\n",
            KERNEL_VERSION_MAJOR,
            KERNEL_VERSION_MINOR,
            KERNEL_VERSION_PATCH);
    kprintf("=====================================\n");
    kprintf("\n");

    kprintf("[BOOT] Loader: %s\n", boot_info->loader_name);
    kprintf("[BOOT] Total Memory: %lu MB\n",
            boot_info->total_memory / (1024 * 1024));
    kprintf("[BOOT] Memory Regions: %u\n",
            boot_info->memory_map_count);

    if (boot_info->framebuffer_addr) {
        kprintf("[BOOT] Framebuffer: %ux%u @ 0x%016lx\n",
                boot_info->framebuffer_width,
                boot_info->framebuffer_height,
                boot_info->framebuffer_addr);
    }

    if (boot_info->initrd_addr) {
        kprintf("[BOOT] Initrd: %lu bytes @ 0x%016lx\n",
                boot_info->initrd_size,
                boot_info->initrd_addr);
    }

    if (boot_info->rsdp_addr) {
        kprintf("[BOOT] ACPI RSDP: 0x%016lx\n",
                boot_info->rsdp_addr);
    }

    kprintf("\n");
}

/**
 * Initialize memory management
 */
static void init_memory(boot_info_t* boot_info) {
    PERF_TIMER_START();

    // Initialize physical memory manager
    pmm_init(boot_info->memory_map, boot_info->memory_map_count);

    // Initialize virtual memory manager
    vmm_init();

    // Initialize kernel heap
    heap_init();

    PERF_TIMER_END("memory_init");

    kprintf("[KERNEL] Free memory: %u MB\n",
            (uint32_t)(pmm_get_free_memory() / (1024 * 1024)));
}

/**
 * Initialize CPU structures
 */
static void init_cpu(void) {
    PERF_TIMER_START();

    // Initialize GDT
    gdt_init();

    // Initialize IDT
    idt_init();

    // Initialize SYSCALL/SYSRET MSRs
    syscall_msr_init();

    // Initialize syscall handler table
    syscall_init();

    PERF_TIMER_END("cpu_init");
}

/**
 * Initialize devices and drivers
 */
static void init_devices(boot_info_t* boot_info) {
    PERF_TIMER_START();

    // Initialize timer (100Hz)
    pit_init(100);

    // Initialize framebuffer
    if (boot_info->framebuffer_addr) {
        framebuffer_init(boot_info->framebuffer_addr,
                        boot_info->framebuffer_width,
                        boot_info->framebuffer_height,
                        boot_info->framebuffer_pitch);
        framebuffer_clear(0x000000);  // Black
    }

    // Initialize PS/2 keyboard
    ps2_init();

    // Initialize ACPI
    if (boot_info->rsdp_addr) {
        acpi_init(boot_info->rsdp_addr);
    }

    // Initialize PCI
    pci_init();

    // Initialize all drivers
    // driver_init_all();

    PERF_TIMER_END("device_init");
}

/**
 * Initialize process subsystem
 */
static void init_processes(void) {
    PERF_TIMER_START();

    // Initialize namespace system
    namespace_init();

    // Initialize process management
    process_init();

    // Initialize scheduler
    scheduler_init();

    PERF_TIMER_END("process_init");
}

/**
 * Mount root filesystem
 *
 * Initializes VFS, mounts ramfs as root, then populates from initrd.
 */
static void mount_root(boot_info_t* boot_info) {
    PERF_TIMER_START();

    // Initialize VFS subsystem (must happen before any mount)
    vfs_init();

    // Mount root filesystem (ramfs)
    if (vfs_mount("none", "/", "ramfs") < 0) {
        kprintf("[KERNEL] ERROR: Failed to mount root filesystem\n");
    }

    if (boot_info->initrd_addr && boot_info->initrd_size) {
        // Initialize and mount initrd (extracts TAR into VFS)
        initrd_init(boot_info->initrd_addr, boot_info->initrd_size);

        // Mount initrd as temporary root
        if (initrd_mount() == 0) {
            kprintf("[KERNEL] Initrd mounted successfully\n");

            // List files in initrd
            uint64_t total_files, total_size;
            initrd_get_stats(&total_files, &total_size);
            kprintf("[KERNEL] Initrd contains %lu files (%lu bytes)\n",
                    total_files, total_size);
        } else {
            kprintf("[KERNEL] Warning: Failed to mount initrd\n");
        }
    } else {
        kprintf("[KERNEL] No initrd available\n");
    }

    // TODO: Mount real root filesystem from disk
    // vfs_mount("/dev/sda1", "/", "autofs");

    PERF_TIMER_END("mount_root");
}

/**
 * Start init process
 *
 * Loads /init ELF from initrd, sets up user stack, and transitions to user mode.
 * Tries multiple init paths for robustness.
 */
static void start_init(void) {
    kprintf("[KERNEL] Starting init process...\n");

    // Initialize TSS for user mode transitions
    tss_init();

    // Try to load init ELF executable
    uint64_t init_entry = 0;
    uint64_t init_stack = 0;
    int load_result = -1;

    // Try multiple init paths (matching what might be in the initrd)
    const char* init_paths[] = { "init", "/init", "/sbin/init", NULL };

    for (int i = 0; init_paths[i] != NULL; i++) {
        load_result = elf_load(init_paths[i], 0, NULL, &init_entry, &init_stack);
        if (load_result == 0) {
            kprintf("[KERNEL] Found init at %s\n", init_paths[i]);
            break;
        }
    }

    if (load_result != 0) {
        kprintf("[KERNEL] ERROR: Could not find init in initrd\n");
        kprintf("[KERNEL] Tried: init, /init, /sbin/init\n");
        kprintf("[KERNEL] Entering idle loop (no init)\n");
        while (1) hlt();
    }

    kprintf("[KERNEL] Init loaded: entry=0x%016lx\n", init_entry);

    // Allocate user stack (8KB = 2 pages)
    uint64_t stack_base = (uint64_t)pmm_alloc_page();
    uint64_t stack_base2 = (uint64_t)pmm_alloc_page();
    if (!stack_base || !stack_base2) {
        kprintf("[KERNEL] ERROR: Failed to allocate user stack\n");
        while (1) hlt();
    }

    const uint64_t USER_STACK_TOP = 0x7FFFFFFFE000ULL;
    vmm_map_page((void*)(USER_STACK_TOP - 4096), (void*)stack_base,
                 PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
    vmm_map_page((void*)(USER_STACK_TOP - 8192), (void*)stack_base2,
                 PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

    uint64_t user_stack_ptr = USER_STACK_TOP;

    kprintf("[KERNEL] Jumping to init at 0x%016lx\n", init_entry);

    // Switch to user mode (never returns)
    start_usermode(init_entry, user_stack_ptr);

    // Should never reach here
    kernel_panic("Init returned to kernel!");
}

/**
 * Main kernel entry point
 */
void kernel_main(boot_info_t* boot_info) {
    // Start total boot time measurement
    uint64_t boot_start = rdtsc();

    // Validate boot info
    if (boot_info->magic != BOOT_MAGIC) {
        // Panic - invalid boot info
        while (1) {
            __asm__ volatile("hlt");
        }
    }

    // Initialize serial console first for debug output
    serial_init();

    // Display boot banner
    display_banner(boot_info);

    // Parse kernel command line
    parse_cmdline(boot_info->cmdline);

    // Initialize CPU structures (GDT, IDT, syscalls)
    kprintf("[1/6] Initializing CPU...\n");
    init_cpu();

    // Initialize memory management (PMM, VMM, heap)
    kprintf("[2/6] Initializing memory...\n");
    init_memory(boot_info);

    // Calibrate CPU frequency
    kprintf("\n");
    perf_calibrate_cpu_freq();
    kprintf("\n");

    // Initialize devices and drivers
    kprintf("[3/6] Initializing devices...\n");
    init_devices(boot_info);

    // Initialize process subsystem
    kprintf("[4/6] Initializing processes...\n");
    init_processes();

    // Mount root filesystem
    kprintf("[5/6] Mounting filesystems...\n");
    mount_root(boot_info);

    // Start init process
    kprintf("[6/6] Starting init...\n");
    start_init();

    // Calculate and display total boot time
    uint64_t boot_end = rdtsc();
    uint64_t boot_cycles = boot_end - boot_start;
    kprintf("\n");
    kprintf("[BOOT] Kernel initialization complete\n");
    kprintf("[BOOT] Total time: %llu cycles (%.2f ms)\n",
            boot_cycles, cycles_to_ms(boot_cycles));
    kprintf("\n");

    // Enable interrupts
    sti();

    kprintf("[KERNEL] System ready\n");
    kprintf("[KERNEL] Free memory: %u MB\n",
            (uint32_t)(pmm_get_free_memory() / (1024 * 1024)));

    // Idle loop (until scheduler takes over)
    kprintf("[KERNEL] Entering idle loop\n");
    while (1) {
        hlt();
    }
}

/**
 * Kernel panic handler
 */
void kernel_panic(const char* message) {
    // Disable interrupts
    cli();

    // Print panic message
    kprintf("\n");
    kprintf("=====================================\n");
    kprintf("   KERNEL PANIC\n");
    kprintf("=====================================\n");
    kprintf("\n");
    kprintf("Panic: %s\n", message);
    kprintf("\n");
    kprintf("System halted.\n");

    // Halt
    while (1) {
        hlt();
    }
}

/**
 * Early console output (before full initialization)
 */
void early_printk(const char* message) {
    // Write to serial port (COM1)
    const char* p = message;
    while (*p) {
        // Wait for transmitter to be ready
        while ((inb(0x3F8 + 5) & 0x20) == 0) {
            // Wait
        }

        // Send character
        outb(0x3F8, *p);
        p++;
    }
}

/**
 * Debug helper - dump memory map
 */
void debug_dump_memory_map(boot_info_t* boot_info) {
    kprintf("\n");
    kprintf("Memory Map:\n");
    kprintf("===========\n");

    for (uint32_t i = 0; i < boot_info->memory_map_count; i++) {
        memory_map_entry_t* entry = &boot_info->memory_map[i];

        kprintf("  Region %2u: 0x%016lx - 0x%016lx (%8lu KB) Type %u\n",
                i,
                entry->base,
                entry->base + entry->length,
                entry->length / 1024,
                entry->type);
    }

    kprintf("\n");
}

/**
 * Debug helper - dump initrd contents
 */
void debug_dump_initrd(void) {
    kprintf("\n");
    kprintf("Initrd Contents:\n");
    kprintf("================\n");

    initrd_list_files();

    kprintf("\n");
}

#endif /* USE_ENHANCED_BOOT */
