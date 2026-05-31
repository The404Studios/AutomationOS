#include "include/kernel.h"
#include "include/mem.h"
#include "include/drivers.h"
#include "include/x86_64.h"
#include "include/syscall.h"
#include "include/perf.h"
#include "include/sched.h"
#include "include/namespace.h"
#include "include/vfs.h"
#include "include/initrd.h"
#include "include/elf.h"
#include "include/usermode.h"
#include "include/tss.h"
#include "include/string.h"
#include "include/ipc.h"

typedef struct {
    uint32_t magic;
    uint32_t version;
    memory_map_entry_t* memory_map;
    uint32_t memory_map_count;
    uint64_t total_memory;
    uint64_t initrd_addr;
    uint64_t initrd_size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_bpp;
    uint32_t pixels_per_scanline;
    uint64_t framebuffer_size;
    uint64_t rsdp_addr;
    char cmdline[256];
    uint64_t boot_time_ms;
} boot_info_t;

/* Multiboot1 info structure */
typedef struct __attribute__((packed)) {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint8_t color_info[6];
} multiboot_info_t;

typedef struct __attribute__((packed)) {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} multiboot_mmap_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t cmdline;
    uint32_t padding;
} multiboot_module_t;

#define MULTIBOOT_MAGIC 0x2BADB002
#define MAX_MMAP_ENTRIES 32

static memory_map_entry_t mb_memory_map[MAX_MMAP_ENTRIES];
static boot_info_t mb_boot_info;

extern void gdt_init(void);
extern void idt_init(void);

static boot_info_t* parse_multiboot(uint64_t mb_info_addr) {
    multiboot_info_t* mb = (multiboot_info_t*)(uintptr_t)mb_info_addr;

    memset(&mb_boot_info, 0, sizeof(boot_info_t));
    mb_boot_info.magic = 0xB001B001;
    mb_boot_info.version = 1;

    uint32_t count = 0;
    uint64_t total = 0;

    if (mb->flags & (1 << 6)) {
        /* Parse GRUB memory map */
        uint32_t offset = 0;
        uint8_t* mmap_base = (uint8_t*)(uintptr_t)mb->mmap_addr;

        kprintf("[BOOT] Parsing multiboot memory map (%u bytes)...\n", mb->mmap_length);

        while (offset < mb->mmap_length && count < MAX_MMAP_ENTRIES) {
            multiboot_mmap_entry_t* entry = (multiboot_mmap_entry_t*)(mmap_base + offset);

            /* A valid multiboot mmap entry has size >= 20 (the bytes after the
             * size field). A malformed/zero size would make offset advance by
             * only 4 and walk off the end of the buffer, parsing kernel memory
             * as fake entries. Stop cleanly. */
            if (entry->size < 20) {
                break;
            }

            mb_memory_map[count].base = entry->addr;
            mb_memory_map[count].length = entry->len;
            mb_memory_map[count].type = entry->type;

            if (entry->type == 1) {
                total += entry->len;
                kprintf("  [%u] 0x%lx - 0x%lx (%lu MB) Available\n",
                    count,
                    (unsigned long)entry->addr,
                    (unsigned long)(entry->addr + entry->len),
                    (unsigned long)(entry->len / (1024*1024)));
            }

            count++;
            offset += entry->size + 4;
        }
    } else if (mb->flags & 1) {
        /* Fallback: use mem_lower/mem_upper */
        kprintf("[BOOT] Using basic memory info\n");
        mb_memory_map[0].base = 0;
        mb_memory_map[0].length = (uint64_t)mb->mem_lower * 1024;
        mb_memory_map[0].type = 1;

        mb_memory_map[1].base = 0x100000;
        mb_memory_map[1].length = (uint64_t)mb->mem_upper * 1024;
        mb_memory_map[1].type = 1;

        count = 2;
        total = mb_memory_map[0].length + mb_memory_map[1].length;
    }

    mb_boot_info.memory_map = mb_memory_map;
    mb_boot_info.memory_map_count = count;
    mb_boot_info.total_memory = total;

    kprintf("[BOOT] Total memory: %lu MB (%u regions)\n",
        (unsigned long)(total / (1024*1024)), count);

    /* Parse multiboot modules (initrd) if available (bit 3 in flags) */
    if (mb->flags & (1 << 3)) {
        if (mb->mods_count > 0 && mb->mods_addr != 0) {
            multiboot_module_t* mods = (multiboot_module_t*)(uintptr_t)mb->mods_addr;

            kprintf("[BOOT] Found %u multiboot module(s)\n", mb->mods_count);

            /* First module is assumed to be initrd */
            if (mods[0].mod_start && mods[0].mod_end > mods[0].mod_start) {
                mb_boot_info.initrd_addr = (uint64_t)mods[0].mod_start;
                mb_boot_info.initrd_size = (uint64_t)(mods[0].mod_end - mods[0].mod_start);

                kprintf("[BOOT] Initrd detected:\n");
                kprintf("  Address: 0x%lx\n", (unsigned long)mb_boot_info.initrd_addr);
                kprintf("  Size: %lu bytes (%lu KB)\n",
                    (unsigned long)mb_boot_info.initrd_size,
                    (unsigned long)(mb_boot_info.initrd_size / 1024));
            }
        } else {
            kprintf("[BOOT] No multiboot modules loaded\n");
        }
    } else {
        kprintf("[BOOT] No module information in multiboot\n");
    }

    /* Parse framebuffer info if available (bit 12 in flags) */
    if (mb->flags & (1 << 12)) {
        mb_boot_info.framebuffer_addr = mb->framebuffer_addr;
        mb_boot_info.framebuffer_width = mb->framebuffer_width;
        mb_boot_info.framebuffer_height = mb->framebuffer_height;
        mb_boot_info.framebuffer_pitch = mb->framebuffer_pitch;
        mb_boot_info.framebuffer_bpp = mb->framebuffer_bpp;
        mb_boot_info.framebuffer_size = (uint64_t)mb->framebuffer_pitch * mb->framebuffer_height;

        kprintf("[BOOT] Framebuffer detected:\n");
        kprintf("  Address: 0x%lx\n", (unsigned long)mb->framebuffer_addr);
        kprintf("  Resolution: %ux%u\n", mb->framebuffer_width, mb->framebuffer_height);
        kprintf("  Pitch: %u bytes\n", mb->framebuffer_pitch);
        kprintf("  BPP: %u bits\n", mb->framebuffer_bpp);
        kprintf("  Type: %u\n", mb->framebuffer_type);
    } else {
        kprintf("[BOOT] No framebuffer info available\n");
    }

    return &mb_boot_info;
}

/*
 * T410_SAFE_BOOT -- diagnostic toggle for the post-splash hang on real hardware.
 * When defined, the kernel SKIPS the NVIDIA GPU register probe and the AHCI/SATA
 * + diskfs init -- both poke real devices that QEMU never exercises and are the
 * prime suspects for a boot that freezes right after the welcome splash. A
 * RAM-rooted boot needs neither to reach the desktop. Delete this line to
 * restore the full driver set once the regression is pinned down.
 */
#define T410_SAFE_BOOT 1

/* ---- Visible boot progress markers ------------------------------------
 * Paint "[N] label" lines to the framebuffer under the boot splash. On real
 * hardware the serial kprintf log isn't visible, so if boot hangs in some
 * device-init step, the LAST marker on screen tells you exactly which step
 * froze. Markers stack downward and are harmlessly overwritten by the
 * compositor once the desktop comes up. No-op until the framebuffer is live. */
static int      g_boot_fb_ok = 0;
static uint32_t g_boot_fb_h  = 0;
static int      g_boot_step  = 0;
static void boot_mark(const char *label) {
    if (!g_boot_fb_ok) return;
    uint32_t y = ((g_boot_fb_h > 200u) ? (g_boot_fb_h / 2u + 56u) : 56u)
               + (uint32_t)g_boot_step * 20u;
    g_boot_step++;
    char buf[80];
    int p = 0, n = g_boot_step;
    buf[p++] = '[';
    if (n >= 10) buf[p++] = (char)('0' + (n / 10) % 10);
    buf[p++] = (char)('0' + n % 10);
    buf[p++] = ']'; buf[p++] = ' ';
    for (int i = 0; label[i] && p < 78; i++) buf[p++] = label[i];
    buf[p] = 0;
    /* scale 2 (16px) green text -- readable in a phone photo of the panel. */
    framebuffer_puts_scaled(buf, 40u, y, 0x0000FF00u, 2u);  /* green = progress */
}

void kernel_main(void* raw_info) {
    serial_init();

    kprintf("\n");
    kprintf("=====================================\n");
    kprintf("   AutomationOS v0.1.0\n");
    kprintf("   created by fourzerofour & claude\n");
    kprintf("   The kernel is ALIVE!\n");
    kprintf("=====================================\n");
    kprintf("\n");

    boot_info_t* boot_info = NULL;

    if (raw_info) {
        kprintf("[BOOT] Multiboot info at 0x%lx\n", (unsigned long)(uintptr_t)raw_info);
        boot_info = parse_multiboot((uint64_t)(uintptr_t)raw_info);
    }

    if (!boot_info || boot_info->memory_map_count == 0) {
        kprintf("[KERNEL] ERROR: No memory map available\n");
        kprintf("[KERNEL] Cannot initialize memory management\n");
        while(1) __asm__ volatile("hlt");
    }

    uint64_t boot_start = rdtsc();
    uint64_t __perf_start = 0;

    kprintf("\n[KERNEL] Initializing subsystems...\n\n");

    kprintf("[KERNEL] Initializing GDT...\n");
    gdt_init();
    kprintf("[KERNEL] GDT initialized\n");

    kprintf("[KERNEL] Initializing IDT...\n");
    idt_init();
    kprintf("[KERNEL] IDT initialized\n");

    // Reserve initrd memory BEFORE PMM init (prevents PMM from overwriting it)
    if (boot_info->initrd_addr && boot_info->initrd_size) {
        extern void pmm_reserve_initrd(uint64_t start, uint64_t size);
        pmm_reserve_initrd(boot_info->initrd_addr, boot_info->initrd_size);
        kprintf("[KERNEL] Reserved initrd memory: 0x%lx - 0x%lx (%lu KB)\n",
                (unsigned long)boot_info->initrd_addr,
                (unsigned long)(boot_info->initrd_addr + boot_info->initrd_size),
                (unsigned long)(boot_info->initrd_size / 1024));
    }

    kprintf("[KERNEL] Initializing PMM...\n");
    pmm_init(boot_info->memory_map, boot_info->memory_map_count);
    kprintf("[KERNEL] PMM initialized\n");

    kprintf("[KERNEL] Initializing VMM...\n");
    vmm_init();
    kprintf("[KERNEL] VMM initialized\n");

    // Initialize lazy TLB shootdown (reduces IPI overhead by 60-80%)
    kprintf("[KERNEL] Initializing lazy TLB shootdown...\n");
    extern void tlb_init(void);
    tlb_init();
    kprintf("[KERNEL] Lazy TLB shootdown initialized\n");

    // Add remaining physical memory pages above 1GB now that VMM/paging
    // has extended identity mapping to cover all RAM
    kprintf("[KERNEL] Adding remaining physical memory pages...\n");
    pmm_add_remaining_pages(boot_info->memory_map, boot_info->memory_map_count);
    kprintf("[KERNEL] Remaining pages added\n");

    kprintf("[KERNEL] Initializing heap...\n");
    heap_init();
    kprintf("[KERNEL] Heap initialized\n");

    // Copy-on-write page refcount table (fork #20). Allocated from the PMM now
    // that it + heap are up; if this fails CoW disables itself and fork falls
    // back to eager copying.
    extern void cow_init(void);
    cow_init();

    // Verify on-demand heap growth works (forces one extension, then frees).
    extern void heap_selftest(void);
    heap_selftest();

    // Verify the slab object-cache allocator (additive; complements kmalloc).
    extern int slab_selftest(void);
    slab_selftest();

    // Benchmark slab allocator efficiency (demonstrates 100x page alloc reduction).
    extern void heap_slab_benchmark(void);
    heap_slab_benchmark();

    /* Enumerate the PCI bus (needed by HDA audio, AHCI storage, NVMe, NICs). */
    kprintf("[KERNEL] Scanning PCI bus...\n");
    extern void pci_init(void);
    pci_init();

    /* SMP brick 0: READ-ONLY ACPI MADT enumeration. Makes the kernel AWARE of
     * how many CPUs the firmware reports. The system stays SINGLE-CORE -- this
     * only logs the count (no AP bring-up). The identity map covers low ACPI
     * memory at this point (boot.asm + the VMM extension above). */
    extern int madt_count_cpus(void);
    kprintf("[SMP] detected %d cpus\n", madt_count_cpus());

#ifdef SMP_FOUNDATION
    /* SMP brick 1 (GATED, SMP=1 build only): bring the BOOTSTRAP PROCESSOR's
     * Local APIC online and log its APIC ID + version. This is groundwork for
     * later AP bring-up -- the system STILL runs single-core here.
     *
     * SAFE SCOPE: lapic_init() only (a) sets the IA32_APIC_BASE global-enable
     * bit, (b) writes SIVR (software-enable + spurious vector 0xFF) and TPR=0,
     * then (c) reads the APIC ID/version. It does NOT mask/disable the 8259 PIC,
     * does NOT program the IOAPIC, does NOT start the LAPIC timer, and does NOT
     * touch IDT routing -- so the existing PIC-delivered PIT (IRQ0) and PS/2
     * keyboard (IRQ1) keep working exactly as before. The LAPIC MMIO base
     * (~0xFEE00000) is already identity-mapped by vmm_init()'s 1GB->16GB
     * extension above, so the register reads cannot fault. */
    extern void lapic_init(void);
    extern uint32_t lapic_get_id(void);
    extern uint32_t lapic_get_version(void);
    lapic_init();
    kprintf("[SMP] BSP local APIC online: id=%u version=0x%x\n",
            lapic_get_id(), lapic_get_version());

    /* SMP brick 2 (GATED, SMP=1 build only): CALIBRATE the BSP's LAPIC timer
     * frequency -- GROUNDWORK ONLY. We MEASURE how fast the LAPIC timer counts;
     * we do NOT arm it as a tick. The PIC-delivered PIT (IRQ0) remains the one
     * and only scheduler tick, so sleep/preempt/scheduler timing is UNCHANGED.
     *
     * SAFETY: this is the safe calibration SUBSET only -- it intentionally does
     * NOT call lapic_timer_init()/lapic_timer_periodic() because those go on to
     * arm PERIODIC mode (unmasked LVT @ vector 0x20) and install a running tick,
     * which is forbidden before a much later brick. Here we:
     *   (a) write the timer LVT MASKED the whole time (LAPIC_TIMER_MASKED),
     *   (b) set divide=16, load the initial count to 0xFFFFFFFF and let it
     *       count DOWN across a known TSC busy-wait (udelay equivalent; no PIT),
     *   (c) read the current-count decrement, compute ticks/second, then
     *   (d) leave the timer disarmed: LVT MASKED + initial-count 0.
     * No LVT is unmasked, no interrupt vector is enabled, no IRQ is expected,
     * no handler is registered, and the 8259/PIT/IOAPIC are never touched. */
    {
        extern uint32_t lapic_read(uint32_t reg);
        extern void lapic_write(uint32_t reg, uint32_t value);
        extern uint64_t rdtsc(void);

        const uint32_t LVT_TIMER      = 0x0320; /* LAPIC_TIMER  (LVT) */
        const uint32_t TIMER_ICR      = 0x0380; /* initial count      */
        const uint32_t TIMER_CCR      = 0x0390; /* current count      */
        const uint32_t TIMER_DCR      = 0x03E0; /* divide config      */
        const uint32_t TIMER_MASKED   = 0x00010000;
        const uint32_t TIMER_DIV_16   = 0x03;
        const uint32_t DIVISOR        = 16;     /* matches TIMER_DIV_16 */

        /* Keep the timer LVT MASKED for the whole measurement so even if it
         * were to wrap it can deliver nothing. */
        lapic_write(LVT_TIMER, TIMER_MASKED);
        lapic_write(TIMER_DCR, TIMER_DIV_16);

        /* Busy-wait a known 10ms window using the TSC (NOT the PIT). 3 GHz TSC
         * estimate (perf.h convention) -> 3000 cycles/us. This only defines the
         * measurement window; the value reported is the LAPIC count rate. */
        const uint64_t CAL_US    = 10000;          /* 10ms              */
        const uint64_t TSC_PER_US = 3000;          /* ~3 GHz estimate   */
        lapic_write(TIMER_ICR, 0xFFFFFFFFu);       /* start counting down */
        uint64_t tsc_start = rdtsc();
        uint64_t tsc_target = CAL_US * TSC_PER_US;
        while ((rdtsc() - tsc_start) < tsc_target) {
            __asm__ volatile("pause");
        }
        uint32_t counted = 0xFFFFFFFFu - lapic_read(TIMER_CCR);

        /* Disarm: stop the count and keep the LVT masked. */
        lapic_write(TIMER_ICR, 0);
        lapic_write(LVT_TIMER, TIMER_MASKED);

        /* counted = post-divider ticks in 10ms. Input bus frequency =
         * counted * DIVISOR / 0.01s = counted * DIVISOR * 100. */
        uint64_t hz = (uint64_t)counted * DIVISOR * (1000000ULL / CAL_US);
        kprintf("[SMP] LAPIC timer calibrated: %lu Hz (PIT remains system tick)\n",
                (unsigned long)hz);
    }

    /* SMP brick 3 (GATED, SMP=1 build only): bring ONE application processor
     * (CPU 1) online as a bare HEARTBEAT. It reaches long mode, marks itself
     * online, and hlts forever. NO scheduling, NO AP timer, NO per-CPU runqueue,
     * NO migration -- those are later bricks. The BSP keeps booting normally.
     *
     * THE HARD RULE dominates: AP failure must NEVER stop or hang the BSP boot.
     * try_start_cpu1() waits on a BOUNDED ~100 ms TSC deadline polling a shared
     * MEMORY flag (never an infinite spin, never a panic, never MMIO-polling). On
     * timeout it returns 0 and we log + continue single-core. Either way we fall
     * through and finish booting the BSP. madt_count_cpus() (brick 0) gives the
     * count; we only attempt the AP when the firmware reports >= 2 CPUs. */
    {
        extern int try_start_cpu1(void);
        int smp_cpu_count = madt_count_cpus();
        if (smp_cpu_count >= 2) {
            if (try_start_cpu1()) {
                kprintf("[SMP] CPU 1 online\n");        /* AP is now hlt-parked */
            } else {
                kprintf("[SMP] AP failed to start, continuing single-core\n");
            }
        } else {
            kprintf("[SMP] single-core (firmware reports %d cpu); no AP to start\n",
                    smp_cpu_count);
        }
        /* Fall through and finish booting the BSP normally -- no matter what. */
    }
#endif

    /* Initialize framebuffer if available */
    if (boot_info->framebuffer_addr && boot_info->framebuffer_width > 0) {
        kprintf("[KERNEL] Initializing framebuffer...\n");

        /* Map framebuffer physical address into kernel virtual space */
        /* For high addresses (>1GB), we need explicit page mappings */
        uint64_t fb_phys = boot_info->framebuffer_addr;
        uint64_t fb_size = (uint64_t)boot_info->framebuffer_pitch * boot_info->framebuffer_height;
        if (fb_phys >= 0x40000000ULL) {
            /* Framebuffer above 1GB — identity-map it */
            uint64_t fb_pages = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;
            kprintf("[KERNEL] Mapping framebuffer: 0x%lx (%lu pages)\n",
                    (unsigned long)fb_phys, (unsigned long)fb_pages);
            for (uint64_t i = 0; i < fb_pages; i++) {
                vmm_map_page((void*)(fb_phys + i * PAGE_SIZE),
                             (void*)(fb_phys + i * PAGE_SIZE),
                             PAGE_PRESENT | PAGE_WRITE);
            }
        }

        framebuffer_init(
            boot_info->framebuffer_addr,
            boot_info->framebuffer_width,
            boot_info->framebuffer_height,
            boot_info->framebuffer_pitch
        );
        kprintf("[KERNEL] Framebuffer initialized at 0x%lx (%ux%u)\n",
            (unsigned long)boot_info->framebuffer_addr,
            boot_info->framebuffer_width,
            boot_info->framebuffer_height);

        /* Enable on-screen boot progress markers (see boot_mark). */
        g_boot_fb_ok = 1;
        g_boot_fb_h  = boot_info->framebuffer_height;

        /* Boot splash: a centered welcome that stays on screen until the
         * userspace compositor takes over the framebuffer. */
        kprintf("[KERNEL] Drawing boot splash...\n");
        framebuffer_clear(0x00101826);  /* dark slate background */
        {
            const char* l1 = "Welcome to AutomationOS";
            const char* l2 = "created by fourzerofour & claude";
            uint32_t W = boot_info->framebuffer_width;
            uint32_t H = boot_info->framebuffer_height;
            uint32_t n1 = 0; while (l1[n1]) n1++;
            uint32_t n2 = 0; while (l2[n2]) n2++;
            uint32_t s1 = 3, s2 = 2;               /* glyph magnification     */
            uint32_t w1 = n1 * 8 * s1, w2 = n2 * 8 * s2;
            uint32_t x1 = (W > w1) ? (W - w1) / 2 : 0;
            uint32_t x2 = (W > w2) ? (W - w2) / 2 : 0;
            uint32_t y1 = (H > 80) ? (H / 2 - 40) : 0;
            uint32_t y2 = (H > 80) ? (H / 2 + 12) : 24;
            framebuffer_puts_scaled(l1, x1, y1, 0x00FFFFFF, s1);  /* white title  */
            framebuffer_puts_scaled(l2, x2, y2, 0x009FC8FF, s2);  /* blue credit  */
        }
        kprintf("[KERNEL] Boot splash drawn!\n");

        /* Map framebuffer for userspace access at a fixed virtual address */
        /* Userspace framebuffer at 0x40000000 (1GB mark, well within user space) */
        uint64_t fb_user_vaddr = 0x40000000ULL;
        uint64_t fb_pages = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;

        kprintf("[KERNEL] Mapping framebuffer for userspace: phys=0x%lx -> virt=0x%lx (%lu pages)\n",
                (unsigned long)fb_phys, (unsigned long)fb_user_vaddr, (unsigned long)fb_pages);

        for (uint64_t i = 0; i < fb_pages; i++) {
            vmm_map_page((void*)(fb_user_vaddr + i * PAGE_SIZE),
                         (void*)(fb_phys + i * PAGE_SIZE),
                         PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        }
        kprintf("[KERNEL] Framebuffer mapped for userspace at 0x%lx\n", (unsigned long)fb_user_vaddr);

    } else {
        kprintf("[KERNEL] No framebuffer available, skipping graphics init\n");
    }

    // Arm the PIT as a MONOTONIC TICK COUNTER only (1000 Hz). The IRQ0 handler
    // increments a tick counter and never reschedules, so cooperative scheduling
    // is preserved (preemptive scheduling is a later milestone). This gives
    // userspace a real time source via SYS_GET_TICKS_MS.
    boot_mark("timer (PIT)");
    extern void pit_init(uint32_t freq_hz);
    pit_init(1000);
    kprintf("[KERNEL] Timer: tick counter armed (1000 Hz, no preemption)\n");
    boot_mark("timer ok");

    // Storage: generic block layer + AHCI/SATA driver (#13). PMM, heap, PCI and
    // the timer are all up by now, which is everything ahci_init() needs. When
    // no AHCI controller is present (default QEMU 'pc' machine, diskless boot)
    // ahci_init() returns non-zero without touching MMIO, so this is a safe
    // no-op there. When a disk IS attached we do a read-only self-test: read
    // LBA 0 and log the MBR signature, which proves a real DMA read works
    // (used by scripts/smoke_ahci.sh to verify the driver end-to-end).
    /* GPU: detect the NVIDIA card (read-only) + log it. Safe no-op when no
     * NVIDIA device is present (e.g. QEMU std-VGA). Uses the firmware-set
     * framebuffer; never programs the display. */
#ifndef T410_SAFE_BOOT
    boot_mark("gpu (NVIDIA detect)");
    extern void nvidia_init(void);
    nvidia_init();
    boot_mark("gpu ok");
#else
    // SAFE BOOT (T410 regression hunt): skip the NVIDIA register probe. It maps
    // the real GPU's BAR0 and reads PMC_BOOT_0 -- a real-hardware MMIO access
    // QEMU never exercises and a prime suspect for the post-splash hang. The
    // firmware framebuffer (which already drew the splash) needs none of it.
    boot_mark("gpu SKIPPED (safe boot)");
#endif

#ifndef T410_SAFE_BOOT
    boot_mark("storage (AHCI/SATA disk)");
    kprintf("[KERNEL] Initializing block layer + AHCI/SATA...\n");
    extern void block_init(void);
    extern int  ahci_init(void);
    extern int  ahci_present(void);
    extern int  ahci_read(uint64_t lba, uint32_t count, void* buf);
    block_init();
    if (ahci_init() == 0 && ahci_present()) {
        uint8_t s0[512];
        if (ahci_read(0, 1, s0) == 0) {
            uint16_t sig = (uint16_t)(s0[510] | ((uint16_t)s0[511] << 8));
            kprintf("[AHCI] sector0 read OK: MBR sig=0x%04x "
                    "first8=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                    sig, s0[0], s0[1], s0[2], s0[3],
                    s0[4], s0[5], s0[6], s0[7]);
        } else {
            kprintf("[AHCI] sector0 read FAILED\n");
        }
    }

    // Persistence: durable on-disk superblock with a cross-boot counter. Needs
    // AHCI up (done above). No-op when no SATA disk is attached. The boot
    // counter incrementing across reboots of the same image proves durable
    // read+write+verify (see scripts/smoke_persist.sh).
    boot_mark("storage ok");
    boot_mark("diskfs (read disk)");
    kprintf("[KERNEL] Initializing persistent diskfs...\n");
    extern void diskfs_init(void);
    diskfs_init();
    boot_mark("diskfs ok");
#else
    // SAFE BOOT (T410 regression hunt): skip block/AHCI/SATA + diskfs entirely.
    // ahci_init() pokes the real SATA controller's MMIO (untestable in QEMU) and
    // the boot is RAM-rooted, so no disk is needed to reach the desktop.
    boot_mark("storage SKIPPED (safe boot)");
#endif

    // Networking: detect the Intel e1000 NIC, read its MAC, assign the static
    // QEMU user-net IPv4 (10.0.2.15). PCI is up, so this is everything net_init
    // needs. The e1000 BAR sits in the <4GB MMIO hole, which the 16GB identity
    // map covers. With no NIC present net_init() returns non-zero and leaves
    // networking down (SYS_NET_INFO then returns ENOTSUP) -- a safe no-op.
    // Networking re-enabled (was skipped during the T410 hang hunt). net_init()
    // is now fully BOUNDED: the ARP settle loop has a hard iteration cap and the
    // 82577LM PCH PHY bring-up uses short, capped spins, so on the T410 (where
    // the NIC doesn't yet link) it bails in well under a second instead of
    // hanging. In QEMU the e1000 links and resolves normally. GPU + disk remain
    // gated by T410_SAFE_BOOT above.
    boot_mark("network (e1000)");
    kprintf("[KERNEL] Initializing networking (e1000)...\n");
    extern int net_init(void);
    net_init();
    extern bool e1000_present(void);
    extern bool e1000_link_up(void);
    extern bool net_up(void);
    if (e1000_present()) {
        boot_mark(e1000_link_up() ? "network: NIC link UP"
                                  : "network: NIC link DOWN (cable?)");
    } else if (net_up()) {
        boot_mark("network ok (rtl8139)");
    } else {
        boot_mark("network: no NIC (offline)");
    }
    // Socket layer: kmalloc the socket table now (heap is up) so UDP/TCP are
    // ready for userspace. Safe even with no NIC -- sockets just can't send.
    extern void sock_init(void);
    sock_init();

    // CMOS real-time clock: logs the wall-clock boot time and backs SYS_TIME.
    extern void rtc_init(void);
    rtc_init();

    // Entropy source: RDRAND if present, else seeded xorshift128+ (backs SYS_RANDOM).
    extern void rng_init(void);
    rng_init();

    // System services: clipboard + notification queue (back SYS_CLIP_* / SYS_NOTIFY*).
    extern void clipboard_init(void);
    extern void notify_init(void);
    clipboard_init();
    notify_init();

    boot_mark("vfs");
    kprintf("[KERNEL] Initializing VFS...\n");
    vfs_init();
    kprintf("[KERNEL] VFS initialized\n");

    // Initialize filesystem drivers
    boot_mark("fs drivers (ext2/fat32)");
    kprintf("[KERNEL] Initializing filesystem drivers...\n");
    extern void ext2_init(void);
    extern void fat32_init(void);
    ext2_init();
    fat32_init();
    kprintf("[KERNEL] Filesystem drivers initialized\n");

    boot_mark("mount root (ramfs)");
    kprintf("[KERNEL] Mounting root filesystem (ramfs)...\n");
    if (vfs_mount("none", "/", "ramfs") == 0) {
        kprintf("[KERNEL] Root filesystem mounted\n");
    } else {
        kprintf("[KERNEL] ERROR: Failed to mount root filesystem\n");
    }

    // Create mount points for additional filesystems
    kprintf("[KERNEL] Creating mount points...\n");
    vfs_mkdir("/mnt", 0755);
    vfs_mkdir("/mnt/data", 0755);
    vfs_mkdir("/mnt/ext2", 0755);
    vfs_mkdir("/mnt/usb", 0755);
    vfs_mkdir("/home", 0755);
    kprintf("[KERNEL] Mount points created\n");

    kprintf("[KERNEL] Creating /dev directory...\n");
    if (vfs_mkdir("/dev", 0755) == 0) {
        kprintf("[KERNEL] /dev directory created\n");
    } else {
        kprintf("[KERNEL] Warning: /dev may already exist\n");
    }

    // Attempt to auto-mount detected filesystems if AHCI is available.
    // DISABLED: parsing a real, foreign/unknown on-disk ext2/fat32 at boot is
    // unvetted on real hardware and can hang the boot. The OS roots on the RAM
    // ramfs and needs no physical disk to reach the desktop ("boot to RAM").
    // Re-enable once ext2/fat32 mount is hardened + tested on real hardware.
    if (0 /* was: ahci_present() */) {
        boot_mark("disk automount (ext2/fat32)");
        kprintf("[KERNEL] Detecting and mounting filesystems...\n");

        // Try to mount ext2 from first SATA drive
        // Note: AHCI driver registers devices as "sata0", "sata1", etc.
        kprintf("[KERNEL] Attempting to mount sata0 as ext2...\n");
        if (vfs_mount("sata0", "/mnt/ext2", "ext2") == 0) {
            kprintf("[FS] Successfully mounted /mnt/ext2 as ext2\n");

            // Test filesystem access
            kprintf("[FS] Testing ext2 filesystem access...\n");
            vfs_stat_t stat;
            if (vfs_stat("/mnt/ext2", &stat) == 0) {
                kprintf("[FS] ext2 root directory accessible (inode=%lu)\n", (unsigned long)stat.st_ino);
            }
        } else {
            kprintf("[FS] No ext2 filesystem found on sata0 (or device not present)\n");
        }

        // Try to mount FAT32 from first SATA drive (alternative attempt)
        // In a partition-aware system, this would be sata0p1, sata0p2, etc.
        kprintf("[KERNEL] Attempting to mount sata0 as fat32...\n");
        if (vfs_mount("sata0", "/mnt/data", "fat32") == 0) {
            kprintf("[FS] Successfully mounted /mnt/data as FAT32\n");

            // Test filesystem access
            kprintf("[FS] Testing FAT32 filesystem access...\n");
            vfs_stat_t stat;
            if (vfs_stat("/mnt/data", &stat) == 0) {
                kprintf("[FS] FAT32 root directory accessible (inode=%lu)\n", (unsigned long)stat.st_ino);
            }
        } else {
            kprintf("[FS] No FAT32 filesystem found on sata0 (already mounted as ext2 or wrong format)\n");
        }

        kprintf("[KERNEL] Filesystem detection complete\n");
        kprintf("[FS] Note: Current implementation mounts entire drive, not partitions\n");
        kprintf("[FS] For partition support, implement MBR/GPT partition table parsing\n");
    } else {
        kprintf("[KERNEL] No block storage detected, skipping filesystem mount\n");
    }

    kprintf("[KERNEL] Creating PTY device nodes...\n");
    if (vfs_mkdir("/dev/pts", 0755) == 0) {
        kprintf("[KERNEL] /dev/pts directory created\n");
    } else {
        kprintf("[KERNEL] Warning: /dev/pts may already exist\n");
    }
    // TODO: Create /dev/ptmx and hook up PTY file operations
    kprintf("[KERNEL] PTY device nodes created\n");

    // Input + keyboard MUST init after VFS mount + /dev mkdir, so that
    // dev_input_init() can create /dev/input and ps2_init() can link the
    // /dev/input/eventN device nodes into the mounted ramfs tree.
    boot_mark("input subsystem");
    kprintf("[KERNEL] Initializing input subsystem...\n");
    extern void input_init(void);
    extern void dev_input_init(void);
    input_init();
    dev_input_init();
    kprintf("[KERNEL] Input subsystem initialized\n");

    boot_mark("keyboard/mouse (PS/2)");
    kprintf("[KERNEL] Initializing keyboard + mouse...\n");
    extern void ps2_init(void);
    ps2_init();
    kprintf("[KERNEL] Keyboard + mouse initialized\n");
    boot_mark("keyboard/mouse ok");

    boot_mark("pty");
    kprintf("[KERNEL] Initializing PTY subsystem...\n");
    extern void pty_init(void);
    pty_init();
    kprintf("[KERNEL] PTY subsystem initialized\n");

    kprintf("[KERNEL] Initializing process table...\n");
    process_init();
    kprintf("[KERNEL] Process table initialized\n");

    kprintf("[KERNEL] Initializing scheduler...\n");
    scheduler_init();
    kprintf("[KERNEL] Scheduler initialized\n");

    kprintf("[KERNEL] Initializing performance monitoring...\n");
    perf_init();
    kprintf("[KERNEL] Performance monitoring initialized\n");

    kprintf("\n");
    kprintf("[KERNEL] All subsystems initialized!\n");
    kprintf("[KERNEL] Free memory: %lu MB\n",
            (unsigned long)(pmm_get_free_memory() / (1024 * 1024)));

    uint64_t boot_end = rdtsc();
    uint64_t boot_cycles = boot_end - boot_start;
    kprintf("[BOOT] Total boot time: %lu.%02lu ms\n",
            (unsigned long)cycles_to_ms(boot_cycles),
            (unsigned long)cycles_to_ms_frac(boot_cycles));

    kprintf("\n");
    kprintf("=====================================\n");
    kprintf("   AutomationOS BOOT COMPLETE!\n");
    kprintf("   All kernel subsystems: ONLINE\n");
    kprintf("   created by fourzerofour & claude\n");
    kprintf("=====================================\n");
    kprintf("\n");

    if (boot_info->initrd_addr && boot_info->initrd_size) {
        kprintf("[KERNEL] Loading initrd...\n");
        initrd_init(boot_info->initrd_addr, boot_info->initrd_size);

        kprintf("[KERNEL] Mounting initrd as root filesystem...\n");
        if (initrd_mount() == 0) {
            kprintf("[KERNEL] Initrd mounted successfully\n");

            /* Create writable scratch dirs (/tmp, /var/tmp, /run) on the ramfs. */
            extern void vfs_fs_init(void);
            vfs_fs_init();

            /* List initrd contents for debugging */
            initrd_list_files();

            /* Try to load and execute /sbin/init */
            kprintf("[KERNEL] Loading /sbin/init...\n");

            uint64_t init_size = 0;
            void* init_data = initrd_get_file("sbin/init", &init_size);

            if (init_data && init_size > 0) {
                kprintf("[KERNEL] Found init: %lu bytes\n", (unsigned long)init_size);

                /* Load init as ELF binary */
                int pid = elf_load_and_exec(init_data, init_size, "/sbin/init");

                if (pid > 0) {
                    kprintf("[KERNEL] Init process started (PID %d)\n", pid);

                    /* Initialize TSS for ring 3 transitions */
                    kprintf("[KERNEL] Initializing TSS for usermode...\n");
                    tss_init();
                    kprintf("[KERNEL] TSS initialized\n");

                    /* Initialize syscall dispatch table */
                    extern void syscall_init(void);
                    syscall_init();

                    /* Initialize IPC subsystems (shared memory, message queues) */
                    extern void shm_init(void);
                    extern void msg_init(void);
                    shm_init();
                    msg_init();

                    /* Initialize SYSCALL/SYSRET MSRs for userspace */
                    extern void syscall_msr_init(void);
                    syscall_msr_init();

                    /* Start scheduler (will enable interrupts after TSS.RSP0 is set) */
                    boot_mark("starting services (scheduler)");
                    kprintf("[KERNEL] Starting scheduler...\n");
                    scheduler_start();

                    /* Should never reach here */
                    kprintf("[KERNEL] ERROR: Scheduler returned!\n");
                } else {
                    kprintf("[KERNEL] ERROR: Failed to load init (error %d)\n", pid);
                }
            } else {
                kprintf("[KERNEL] ERROR: /sbin/init not found in initrd\n");
            }
        } else {
            kprintf("[KERNEL] ERROR: Failed to mount initrd\n");
        }
    } else {
        kprintf("[KERNEL] No initrd detected\n");
        kprintf("[KERNEL] Desktop environment requires UEFI boot or initrd\n");
    }

    kprintf("[KERNEL] Kernel initialization complete\n");
    kprintf("[KERNEL] Entering idle loop\n");
    sti();
    while (1) {
        hlt();
    }
}
