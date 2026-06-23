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
#include "include/health_monitor.h"

/* Gate routine boot chatter (the "Initializing X... / X initialized" pairs).
 * Defined via -DBOOT_QUIET in the build flags. Banners, errors, and key
 * status lines are NOT gated — only the per-subsystem init noise. */
#ifdef BOOT_QUIET
#define BOOT_LOG(...) ((void)0)
#else
#define BOOT_LOG(...) kprintf(__VA_ARGS__)
#endif

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

        BOOT_LOG("[BOOT] Parsing multiboot memory map (%u bytes)...\n", mb->mmap_length);

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
                BOOT_LOG("  [%u] 0x%lx - 0x%lx (%lu MB) Available\n",
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
        BOOT_LOG("[BOOT] Using basic memory info\n");
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

    BOOT_LOG("[BOOT] Total memory: %lu MB (%u regions)\n",
        (unsigned long)(total / (1024*1024)), count);

    /* Parse multiboot modules (initrd) if available (bit 3 in flags) */
    if (mb->flags & (1 << 3)) {
        if (mb->mods_count > 0 && mb->mods_addr != 0) {
            multiboot_module_t* mods = (multiboot_module_t*)(uintptr_t)mb->mods_addr;

            BOOT_LOG("[BOOT] Found %u multiboot module(s)\n", mb->mods_count);

            /* First module is assumed to be initrd */
            if (mods[0].mod_start && mods[0].mod_end > mods[0].mod_start) {
                mb_boot_info.initrd_addr = (uint64_t)mods[0].mod_start;
                mb_boot_info.initrd_size = (uint64_t)(mods[0].mod_end - mods[0].mod_start);

                BOOT_LOG("[BOOT] Initrd detected:\n");
                BOOT_LOG("  Address: 0x%lx\n", (unsigned long)mb_boot_info.initrd_addr);
                BOOT_LOG("  Size: %lu bytes (%lu KB)\n",
                    (unsigned long)mb_boot_info.initrd_size,
                    (unsigned long)(mb_boot_info.initrd_size / 1024));
            }
        } else {
            BOOT_LOG("[BOOT] No multiboot modules loaded\n");
        }
    } else {
        BOOT_LOG("[BOOT] No module information in multiboot\n");
    }

    /* Parse framebuffer info if available (bit 12 in flags) */
    if (mb->flags & (1 << 12)) {
        mb_boot_info.framebuffer_addr = mb->framebuffer_addr;
        mb_boot_info.framebuffer_width = mb->framebuffer_width;
        mb_boot_info.framebuffer_height = mb->framebuffer_height;
        mb_boot_info.framebuffer_pitch = mb->framebuffer_pitch;
        mb_boot_info.framebuffer_bpp = mb->framebuffer_bpp;
        mb_boot_info.framebuffer_size = (uint64_t)mb->framebuffer_pitch * mb->framebuffer_height;

        BOOT_LOG("[BOOT] Framebuffer detected:\n");
        BOOT_LOG("  Address: 0x%lx\n", (unsigned long)mb->framebuffer_addr);
        BOOT_LOG("  Resolution: %ux%u\n", mb->framebuffer_width, mb->framebuffer_height);
        BOOT_LOG("  Pitch: %u bytes\n", mb->framebuffer_pitch);
        BOOT_LOG("  BPP: %u bits\n", mb->framebuffer_bpp);
        BOOT_LOG("  Type: %u\n", mb->framebuffer_type);
    } else {
        BOOT_LOG("[BOOT] No framebuffer info available\n");
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
#ifdef SMP_FOUNDATION
/* The SMP/AP result, captured during brick-3 bring-up (which runs BEFORE the
 * framebuffer is live) and painted as an on-screen boot marker AFTER the splash.
 * On real hardware (e.g. the T410) the serial [SMP] log is invisible, so this is
 * how you read whether CPU 1 came online: photograph the boot panel. */
static const char *g_smp_boot_status = "SMP: (no result)";
/* Brick 3.5: did CPU 1 (the AP) actually come online? Captured from try_start_cpu1()
 * during brick-3 bring-up (which runs BEFORE the framebuffer is live) and consulted
 * later by the BSP proof window: we only spin/display CPU1's heartbeat if the AP is
 * up, otherwise CPU1 stays 0 and we just run CPU0's counter (no hang). */
static int g_smp_ap_online = 0;
/* Brick 3.5: the two ISOLATED per-CPU heartbeat counters, defined in ap_boot.c on
 * SEPARATE 64-byte cache lines. cpu_hb[0]=BSP, cpu_hb[1]=AP. The BSP increments
 * ONLY cpu_hb[0] in its proof window and reads both for display; the AP increments
 * ONLY cpu_hb[1]. struct hb mirrors the ap_boot.c definition exactly. */
struct hb { volatile uint64_t v; char pad[56]; } __attribute__((aligned(64)));
extern struct hb cpu_hb[2];
/* tiny unsigned->decimal appender for the diagnostic framebuffer-geometry marker. */
static char *u32_dec(char *p, uint32_t v) {
    char t[10]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}
/* Brick 3.5: append a uint64 as decimal (for the "CPU0=<n> CPU1=<m>" proof line). */
static char *u64_dec(char *p, uint64_t v) {
    char t[20]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}
#endif
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

    BOOT_LOG("\n");
    BOOT_LOG("=====================================\n");
    BOOT_LOG("   AutomationOS v0.1.0\n");
    BOOT_LOG("   created by fourzerofour & claude\n");
    BOOT_LOG("   The kernel is ALIVE!\n");
    BOOT_LOG("=====================================\n");
    BOOT_LOG("\n");

    boot_info_t* boot_info = NULL;

    if (raw_info) {
        BOOT_LOG("[BOOT] Multiboot info at 0x%lx\n", (unsigned long)(uintptr_t)raw_info);
        boot_info = parse_multiboot((uint64_t)(uintptr_t)raw_info);
    }

    if (!boot_info || boot_info->memory_map_count == 0) {
        kprintf("[KERNEL] ERROR: No memory map available\n");
        kprintf("[KERNEL] Cannot initialize memory management\n");
        while(1) __asm__ volatile("hlt");
    }

    uint64_t boot_start = rdtsc();
    uint64_t __perf_start = 0;

    BOOT_LOG("\n[KERNEL] Initializing subsystems...\n\n");

    BOOT_LOG("[KERNEL] Initializing GDT...\n");
    gdt_init();
    BOOT_LOG("[KERNEL] GDT initialized\n");

    BOOT_LOG("[KERNEL] Initializing IDT...\n");
    idt_init();
    BOOT_LOG("[KERNEL] IDT initialized\n");

    // Reserve initrd memory BEFORE PMM init (prevents PMM from overwriting it)
    if (boot_info->initrd_addr && boot_info->initrd_size) {
        extern void pmm_reserve_initrd(uint64_t start, uint64_t size);
        pmm_reserve_initrd(boot_info->initrd_addr, boot_info->initrd_size);
        BOOT_LOG("[KERNEL] Reserved initrd memory: 0x%lx - 0x%lx (%lu KB)\n",
                (unsigned long)boot_info->initrd_addr,
                (unsigned long)(boot_info->initrd_addr + boot_info->initrd_size),
                (unsigned long)(boot_info->initrd_size / 1024));
    }

    BOOT_LOG("[KERNEL] Initializing PMM...\n");
    pmm_init(boot_info->memory_map, boot_info->memory_map_count);
    BOOT_LOG("[KERNEL] PMM initialized\n");

    BOOT_LOG("[KERNEL] Initializing VMM...\n");
    vmm_init();
    BOOT_LOG("[KERNEL] VMM initialized\n");

    // Initialize lazy TLB shootdown (reduces IPI overhead by 60-80%)
    BOOT_LOG("[KERNEL] Initializing lazy TLB shootdown...\n");
    extern void tlb_init(void);
    tlb_init();
    BOOT_LOG("[KERNEL] Lazy TLB shootdown initialized\n");

    // Add remaining physical memory pages above 1GB now that VMM/paging
    // has extended identity mapping to cover all RAM
    BOOT_LOG("[KERNEL] Adding remaining physical memory pages...\n");
    pmm_add_remaining_pages(boot_info->memory_map, boot_info->memory_map_count);
    BOOT_LOG("[KERNEL] Remaining pages added\n");

    BOOT_LOG("[KERNEL] Initializing heap...\n");
    heap_init();
    BOOT_LOG("[KERNEL] Heap initialized\n");

    // Copy-on-write page refcount table (fork #20). Allocated from the PMM now
    // that it + heap are up; if this fails CoW disables itself and fork falls
    // back to eager copying.
    extern void cow_init(void);
    cow_init();

    // Boot-time selftests: verify heap growth, slab cache, and slab efficiency.
    // Gated by BOOT_QUIET: each test prints many serial lines that add latency
    // on real hardware (the T410). The allocators are exercised normally by the
    // rest of the boot sequence anyway; these are diagnostic-only.
#ifndef BOOT_QUIET
    extern void heap_selftest(void);
    heap_selftest();

    extern int slab_selftest(void);
    slab_selftest();

    extern void heap_slab_benchmark(void);
    heap_slab_benchmark();
#endif

    /* Enumerate the PCI bus (needed by HDA audio, AHCI storage, NVMe, NICs). */
    BOOT_LOG("[KERNEL] Scanning PCI bus...\n");
    extern void pci_init(void);
    extern void pci_list(void);
    pci_init();

    /* Print the full PCI device map with human-readable names and driver status.
     * Invaluable for T410 debugging: shows every device the kernel sees, whether
     * we have a driver for it, and highlights any unknowns. Pure kprintf output,
     * no MMIO, no side effects -- safe on any machine including T410_SAFE_BOOT. */
    pci_list();

    /* ACPI: parse RSDP/RSDT/FADT/DSDT, decode _S5_ sleep type, enable ACPI mode.
     * This gives us the PM1a control port for poweroff (S5) and the reset
     * register for reboot. Also populates MADT/HPET/MCFG for later use.
     * Called after PCI (not a dependency, just ordering clarity) and before
     * the SMP MADT count so the full ACPI state is available. On failure
     * (no RSDP), poweroff/reboot fall back to QEMU magic ports + 8042. */
    boot_mark("ACPI");
    {
        extern int acpi_init(void);
        if (acpi_init() < 0) {
            BOOT_LOG("[KERNEL] WARNING: ACPI init failed (poweroff may not work)\n");
        } else {
            BOOT_LOG("[KERNEL] ACPI initialized (poweroff + reboot available)\n");
        }
    }
    boot_mark("ACPI ok");

    /* SMP brick 0: READ-ONLY ACPI MADT enumeration. Makes the kernel AWARE of
     * how many CPUs the firmware reports. The system stays SINGLE-CORE -- this
     * only logs the count (no AP bring-up). The identity map covers low ACPI
     * memory at this point (boot.asm + the VMM extension above). */
    extern int madt_count_cpus(void);
    BOOT_LOG("[SMP] detected %d cpus\n", madt_count_cpus());

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
    BOOT_LOG("[SMP] BSP local APIC online: id=%u version=0x%x\n",
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
        BOOT_LOG("[SMP] LAPIC timer calibrated: %lu Hz (PIT remains system tick)\n",
                (unsigned long)hz);
    }

#ifdef SMP_IPI
    /* SMP-G0 IPI-LINK (GATED, SMP=1 SMP_IPI=1 builds only): claim the IPI IDT
     * vectors (0x50-0x55) and arm the send paths. MUST run here -- after
     * lapic_init() (ipi_init captures the BSP APIC id and the gates' targets
     * EOI the LAPIC) and BEFORE try_start_cpu1() (the IDT is shared with the
     * AP, so every IPI gate exists before CPU1 is alive enough to take one).
     * ipi_init refuses (loud serial FATAL, subsystem stays disarmed) if any
     * vector is already claimed. Nothing SENDS an IPI yet: the one
     * IPI_RESCHEDULE proof fires from the G0 selftest after the F2 checkpoint,
     * and kernel_panic's ipi_stop_all_cpus is gated availability (SMP-R0). */
    {
        extern void ipi_init(void);
        ipi_init();
    }
#endif

#ifdef SMP_SCHED
    /* Brick B: install the per-CPU TSS array + both GDT TSS descriptors NOW, BEFORE
     * try_start_cpu1() -- the AP's ap_main() does `ltr 0x38` (gdt_ap_load_tss) as
     * its first action, so CPU1's TSS descriptor (gdt[7-8]) must already exist or
     * the ltr #GPs. The default build keeps its single tss_init() later (just before
     * scheduler_start); under SMP_SCHED that late call is #ifndef-skipped so we
     * initialize exactly once, here. The BSP entering ring 3 happens much later
     * (scheduler_start), so loading the BSP TSS early is harmless. */
    {
        extern void tss_init(void);
        BOOT_LOG("[SMP] Brick B: installing per-CPU TSS array before AP bring-up...\n");
        tss_init();
    }
#endif

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
                BOOT_LOG("[SMP] CPU 1 online\n");        /* AP reached long mode */
#ifdef SMP_SCHED
                /* Brick A checkpoint: a REAL cpu_id(). The BSP must report 0; the
                 * AP recorded its own cpu_id() into g_ap_observed_cpuid (expect 1)
                 * during ap_main. Printed here by the BSP so the AP never races the
                 * serial port. */
                {
                    extern uint32_t cpu_id(void);
                    extern volatile uint32_t g_ap_observed_cpuid;
                    extern volatile uint16_t g_ap_observed_tr;
                    uint16_t bsp_tr;
                    __asm__ volatile("str %0" : "=r"(bsp_tr));
                    BOOT_LOG("[SMP] Brick A cpu_id: BSP=%u (expect 0), AP=%u (expect 1)\n",
                            cpu_id(), g_ap_observed_cpuid);
                    BOOT_LOG("[SMP] Brick B TR: BSP=0x%x (expect 0x28), AP=0x%x (expect 0x38)\n",
                            bsp_tr, g_ap_observed_tr);
                }
#endif
                /* Brick 6: the AP now runs a managed kernel WORKER loop -- it
                 * spin-polls ONE job slot. Dispatch a SINGLE trusted job to CPU 1
                 * to PROVE it can execute BSP-dispatched code and return a correct
                 * result via shared memory. cpu1_run() submits the job and waits on
                 * a BOUNDED ~100 ms TSC deadline, so a wedged AP can never hang us.
                 *
                 * The job computes sum(1..1000) on CPU 1 and stores it in shared
                 * memory (g_worktest); 500500 is the known-correct answer. This is
                 * the first REAL work on the second core: one trusted fn, no
                 * scheduler, no arbitrary process, no migration. The AP spin-polls
                 * (it is NOT hlt-parked) because it has no wakeup IRQ yet -- a
                 * low-power IPI-wake is a future brick. */
#ifdef SMP_SCHED_DISPATCH
                /* SCHEDULER MODE (Brick F): CPU1 runs ap_scheduler_loop(), NOT the
                 * cpu1_job worker loop, so the coprocessor offload self-tests
                 * (worktest / matmul / rapid-offload) would submit jobs nobody
                 * services and just burn their deadlines. Skip them entirely. */
                BOOT_LOG("[SMP] CPU1 in SCHEDULER mode (Brick F): coprocessor "
                        "offload self-tests skipped\n");
                g_smp_ap_online = 1;
#else
                extern int cpu1_run(void (*fn)(void *), void *arg);
                extern volatile long g_worktest;
                extern void worktest(void *a);
                /* Initialize the cpu1_job slot's ownership descriptors to the
                 * OWNED birth state BEFORE the first cpu1_submit(). Without this,
                 * the descriptors sit in .bss as all-zeroes: magic==0 (not
                 * OWN_MAGIC), so the first own_transition() in cpu1_submit would
                 * trip own_validate()'s ASSERT_ALWAYS(magic==OWN_MAGIC) and panic.
                 * cpu1_submit re-inits per job too, but this seeds a valid slot for
                 * any pre-submit orphan check and makes the first submit's own_init
                 * a no-op rather than a from-garbage reset. */
                extern void cpu1_job_init(void);
                cpu1_job_init();
                if (cpu1_run(worktest, (void *)1000) && g_worktest == 500500) {
                    BOOT_LOG("[SMP] CPU 1 ran worker job: sum(1..1000)=%ld "
                            "(expected 500500)\n", g_worktest);
                    g_smp_boot_status = "SMP: CPU 1 WORKER OK";
                } else {
                    BOOT_LOG("[SMP] CPU 1 worker job FAILED or timed out "
                            "(got %ld)\n", g_worktest);
                    g_smp_boot_status = "SMP: CPU 1 worker FAIL";
                }
                g_smp_ap_online = 1;   /* brick 6: AP is up -> managed worker loop */

                /* SMP brick 8: split an INTEGER matrix multiply across CPU0 (BSP)
                 * and CPU1 (AP) and measure the real speedup -- the payoff of the
                 * SMP arc. Runs AFTER the brick-6 sum test and only now that CPU 1
                 * is confirmed online. The BSP submits the BOTTOM band to CPU1
                 * (cpu1_submit, non-blocking), computes the TOP band itself
                 * CONCURRENTLY, then cpu1_wait()s on a GENEROUS ~5s deadline (real
                 * work, not a liveness probe -- on timeout it logs FAIL, never
                 * hangs). It then verifies the dual-core result bit-for-bit against
                 * a single-core baseline and logs the split (with by_apic=1 PROVING
                 * apic-1 ran its band), both cycle counts, the integer speedup, and
                 * the verify result. INT-ONLY: no float/SSE on the AP path (the AP
                 * trampoline does not provably enable SSE state on CPU1). */
                extern void matmul_self_test(void);
                matmul_self_test();

                /* SMP brick 8.5: rapid-fire CPU1 offload stress test (100 sequential
                 * offloads in a tight loop to check for races in job slot reuse,
                 * ownership state transitions, and the cpu1_job_lock spinlock). */
                extern int test_rapid_cpu1_offload(void);
                test_rapid_cpu1_offload();
#endif /* !SMP_SCHED_DISPATCH */
            } else {
                BOOT_LOG("[SMP] AP failed to start, continuing single-core\n");
                g_smp_boot_status = "SMP: AP failed (single-core)";
            }
        } else {
            BOOT_LOG("[SMP] single-core (firmware reports %d cpu); no AP to start\n",
                    smp_cpu_count);
            g_smp_boot_status = "SMP: single-core (1 cpu)";
        }
        /* Fall through and finish booting the BSP normally -- no matter what. */
    }

    // Health monitor: initialize the state now, but DEFER starting its kernel
    // THREAD until AFTER /sbin/init is loaded. The monitor thread is a
    // process_create(); if it runs here (before init) it grabs PID 1, so init
    // ends up PID 2 and aborts ("Not PID 1!") -- which is exactly why the SMP
    // desktop failed to start. See the deferred health_monitor_start_thread()
    // right after init's elf_load_and_exec below.
    health_monitor_init();
#endif

    /* Initialize framebuffer if available */
    if (boot_info->framebuffer_addr && boot_info->framebuffer_width > 0) {
        BOOT_LOG("[KERNEL] Initializing framebuffer...\n");

        /* Map framebuffer physical address into kernel virtual space */
        /* For high addresses (>1GB), we need explicit page mappings */
        uint64_t fb_phys = boot_info->framebuffer_addr;
        uint64_t fb_size = (uint64_t)boot_info->framebuffer_pitch * boot_info->framebuffer_height;
        if (fb_phys >= 0x40000000ULL) {
            /* Framebuffer above 1GB — identity-map it */
            uint64_t fb_pages = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;
            BOOT_LOG("[KERNEL] Mapping framebuffer: 0x%lx (%lu pages)\n",
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
        /* T410 safety: verify framebuffer_init accepted the parameters.
         * If it refused (bogus geometry or address beyond 16 GB identity
         * map), the boot splash and on-screen boot_mark simply no-op. */
        {
            extern int framebuffer_get_info(fb_info_t*);
            fb_info_t fbi;
            if (framebuffer_get_info(&fbi) == 0) {
                BOOT_LOG("[KERNEL] Framebuffer OK at 0x%lx (%ux%u pitch=%u bpp=%u)\n",
                    (unsigned long)fbi.phys_base, fbi.width, fbi.height,
                    fbi.pitch, fbi.bpp);
            } else {
                BOOT_LOG("[KERNEL] WARNING: framebuffer_init REFUSED "
                        "(addr=0x%lx %ux%u pitch=%u)\n",
                        (unsigned long)boot_info->framebuffer_addr,
                        boot_info->framebuffer_width,
                        boot_info->framebuffer_height,
                        boot_info->framebuffer_pitch);
            }
        }

        /* Write-Combining: now that the FB physical base/size are known and the
         * pages are mapped (but BEFORE the splash + compositor hammer the
         * framebuffer), program a variable-range MTRR to mark the FB region WC
         * so pixel stores coalesce into PCIe bursts.  On the T410 (FB mapped UC
         * by firmware) this is a 10-50x compositor speedup.  In QEMU the FB is
         * already cached so WC is redundant -- the call just proves clean boot.
         * Runtime-safe: bails cleanly if no free MTRR slot or base unaligned. */
        BOOT_LOG("[KERNEL] Enabling framebuffer write-combining (MTRR)...\n");
        fb_enable_write_combining(fb_phys, fb_size);

        /* Enable on-screen boot progress markers (see boot_mark). */
        g_boot_fb_ok = 1;
        g_boot_fb_h  = boot_info->framebuffer_height;

        /* Boot splash: a centered welcome that stays on screen until the
         * userspace compositor takes over the framebuffer. */
        BOOT_LOG("[KERNEL] Drawing boot splash...\n");
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
        BOOT_LOG("[KERNEL] Boot splash drawn!\n");

        /* Cool loading animation: a fluid orbiting-dot spinner below the splash
         * title while the rest of boot proceeds. Bounded, rdtsc-timed (pre-PIT,
         * pre-scheduler -- single-threaded here), draws only to the framebuffer
         * (all serial/kprintf output is untouched, so headless smoke is unchanged).
         * OUTSIDE the SMP #ifdef so it runs on the shipping default kernel. */
        /* Boot spinner: brief enough to be visible but not a bottleneck.
         * On T410's UC framebuffer each frame is expensive -- keep it short. */
        framebuffer_boot_spinner(80);

#ifdef SMP_FOUNDATION
        /* Paint the SMP/AP result + the REAL framebuffer geometry at a FIXED spot
         * at the TOP-LEFT of the panel (bright cyan, scale 3, ABOVE the splash and
         * clear of the green device-marker stack -- which scrolls down past the
         * bottom edge after ~17 markers). This keeps them ALWAYS visible for a
         * phone photo on real hardware (the T410), where the serial log is invisible.
         * Overwritten by the compositor when the desktop comes up.
         *   line 1: "SMP: CPU 1 ONLINE" / "SMP: AP failed (single-core)" / "...1 cpu"
         *   line 2: "FB <w>x<h> pitch=<p>" -- if pitch != w*4 it's a left-bleed suspect */
        framebuffer_puts_scaled(g_smp_boot_status, 40u, 20u, 0x0000FFFFu, 3u);
        {
            static char fbm[48];
            char *p = fbm; const char *s = "FB ";
            while (*s) *p++ = *s++;
            p = u32_dec(p, boot_info->framebuffer_width);  *p++ = 'x';
            p = u32_dec(p, boot_info->framebuffer_height);
            s = " pitch="; while (*s) *p++ = *s++;
            p = u32_dec(p, boot_info->framebuffer_pitch);  *p = 0;
            framebuffer_puts_scaled(fbm, 40u, 60u, 0x0000FFFFu, 3u);
        }

        /* ---------------------------------------------------------------------
         * BRICK 3.5: per-CPU heartbeat PROOF WINDOW.
         *
         * The framebuffer is live and the SMP/FB markers are painted, but the
         * userspace compositor has NOT taken the framebuffer yet -- so painting
         * here is safe. For a bounded ~4 seconds (TSC deadline, same ~3 GHz
         * convention as ap_boot.c / brick 2), the BSP repeatedly increments ONLY
         * its own counter cpu_hb[0].v and repaints "CPU0=<n> CPU1=<m>" at a fixed
         * top-left spot (y=100, just below the markers) so CPU0's counter is
         * visibly climbing on real hardware.
         *
         * BRICK 6 update: the AP no longer pure-spins cpu_hb[1]. It bumps cpu_hb[1]
         * a SMALL fixed number of times (proof-of-life, so the "CPU1 ran" evidence
         * is still nonzero) and then runs a managed WORKER loop, spin-polling its
         * one job slot. By the time this window runs the brick-6 self-test job has
         * already completed, so no further job is pending -- cpu_hb[1] holds the
         * small proof-of-life constant while CPU0 climbs. The AP's OWN ap1_idle_ticks
         * counter, however, climbs the whole time: it busy-polls the slot (it has no
         * wakeup IRQ yet, so it cannot `hlt`-park between jobs -- IPI-wake is a later
         * brick). If the AP did NOT come online (single-core / brick-3 failure)
         * cpu_hb[1] stays 0 and we don't hang. NO scheduler, NO run queue, NO IPI.
         * ------------------------------------------------------------------- */
        {
            const uint64_t HB_TSC_PER_US = 3000ULL;          /* ~3 GHz estimate    */
            const uint64_t HB_WINDOW_US  = 4000000ULL;        /* ~4 seconds         */
            uint64_t hb_start    = rdtsc();
            uint64_t hb_deadline = HB_WINDOW_US * HB_TSC_PER_US;
            uint64_t repaint_gate = 0;                        /* throttle repaints  */
            BOOT_LOG("[SMP] heartbeat proof window: BSP spins cpu_hb[0], "
                    "AP %s (~4s)\n",
                    g_smp_ap_online ? "spin-polling worker slot (cpu_hb[1] frozen, "
                                      "idle_ticks climbing)"
                                    : "absent (single-core)");
            while ((rdtsc() - hb_start) < hb_deadline) {
                /* BSP touches ONLY its own isolated counter. */
                cpu_hb[0].v++;

                /* Condition-driven early exit: once the AP has proven it is alive
                 * (cpu_hb[1] > 0) there is no reason to burn another 3+ seconds
                 * spinning. Break out immediately -- the final values are logged
                 * below, and the on-screen paint already shows the proof. On
                 * single-core (no AP) cpu_hb[1] stays 0 and we spin the full
                 * window so the on-screen CPU0 counter is still visible. */
                if (cpu_hb[1].v > 0) break;

                /* Repaint the live line every ~65536 BSP increments so the on-screen
                 * "CPU0=.. CPU1=.." is readable (and we don't spend the whole window
                 * inside the glyph blitter). framebuffer_puts_scaled is BSP-only and
                 * the compositor isn't up yet, so this paint is safe. */
                if ((cpu_hb[0].v - repaint_gate) >= 65536ULL) {
                    repaint_gate = cpu_hb[0].v;
                    char hbline[64];
                    char *q = hbline; const char *s = "CPU0=";
                    while (*s) *q++ = *s++;
                    q = u64_dec(q, cpu_hb[0].v);
                    s = " CPU1="; while (*s) *q++ = *s++;
                    q = u64_dec(q, cpu_hb[1].v);
                    *q = 0;
                    framebuffer_puts_scaled(hbline, 40u, 100u, 0x0000FF00u, 2u);
                }
                __asm__ volatile("pause");
            }

            /* Headless verification hook: the framebuffer paint is invisible to
             * headless QEMU, so ALSO emit the final values on serial. Brick 6: on
             * -smp 2 CPU0 is large (it spins+paints) while CPU1 is a SMALL constant
             * (the proof-of-life count -- the AP ran its self-test job then resumed
             * polling); on -smp 1 CPU1 stays 0 (no AP). Also report the AP's idle
             * poll count (ap1_idle_ticks now CLIMBS: the AP busy-polls its worker
             * slot because it has no wakeup IRQ yet -- IPI-wake is a later brick). */
            extern volatile uint64_t ap1_idle_ticks;
            BOOT_LOG("[SMP] heartbeat: CPU0=%lu CPU1=%lu (proof-of-life); "
                    "CPU1 idle_ticks=%lu (worker spin-poll)\n",
                    cpu_hb[0].v, cpu_hb[1].v, ap1_idle_ticks);
#ifdef SMP_SCHED
            /* Brick E checkpoint: CPU1 armed its LAPIC timer (100 Hz, vector 0x40)
             * and did sti before this ~4s window. ap_timer_ticks must have CLIMBED
             * (≈400) -- proving CPU1 takes interrupts and EOIs the LAPIC correctly
             * (a wrong EOI would have wedged the BSP's IRQ0 -> frozen desktop). */
            {
                extern volatile uint64_t ap_timer_ticks;
                BOOT_LOG("[SMP] Brick E: CPU1 LAPIC timer ticks=%lu (expect ~400 over 4s; "
                        ">0 proves CPU1 takes IRQs + EOIs LAPIC)\n", ap_timer_ticks);
#ifdef SMP_SCHED_DISPATCH
                {
                    extern volatile uint64_t ap_dbg_stage;
                    BOOT_LOG("[SMP] DBG: ap_dbg_stage=%lu (1=enter,2=pre-switch,3=loop running)\n",
                            ap_dbg_stage);
                }
#endif
            }
#ifdef SMP_SCHED_DISPATCH
            /* Brick F2 checkpoint: ap_kthread_counter climbing proves CPU1 actually
             * context-switched INTO the pinned kernel thread and is running it in
             * parallel with CPU0's desktop. Read it twice ~spaced to show motion. */
            {
                extern volatile uint64_t ap_kthread_counter;
                uint64_t c1 = ap_kthread_counter;
                for (volatile int d = 0; d < 2000000; d++) { __asm__ volatile("pause"); }
                uint64_t c2 = ap_kthread_counter;
                BOOT_LOG("[SMP] Brick F2: AP kthread counter %lu -> %lu (delta=%lu; "
                        ">0 proves CPU1 ran the pinned kernel thread)\n",
                        c1, c2, c2 - c1);
            }
#ifdef SMP_IPI
            /* SMP-G0 IPI-LINK acceptance: BSP sends ONE IPI_RESCHEDULE to CPU1,
             * CPU1's handler counts it + EOIs, BSP bounded-polls (~100 ms) and
             * prints "IPILINK: PASS ipi_resched=1 cpu1_count=1". Placed HERE
             * because Brick E/F2 just proved CPU1 is taking interrupts (IF=1),
             * and the BSP is still in its serial-safe pre-desktop window. The
             * handler takes NO scheduling action -- IPI-wake is SMP-G1. Gated
             * on SMP_SCHED_DISPATCH (enclosing #ifdef) because only the real
             * cpu_id() makes CPU1's handler count into ipi_stats[1]. */
            {
                extern void ipi_link_selftest(void);
                ipi_link_selftest();
            }
#endif
#endif
#endif
        }
#endif

        /* Map framebuffer for userspace access at a fixed virtual address */
        /* Userspace framebuffer at 0x40000000 (1GB mark, well within user space) */
        uint64_t fb_user_vaddr = 0x40000000ULL;
        uint64_t fb_pages = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;

        BOOT_LOG("[KERNEL] Mapping framebuffer for userspace: phys=0x%lx -> virt=0x%lx (%lu pages)\n",
                (unsigned long)fb_phys, (unsigned long)fb_user_vaddr, (unsigned long)fb_pages);

        for (uint64_t i = 0; i < fb_pages; i++) {
            vmm_map_page((void*)(fb_user_vaddr + i * PAGE_SIZE),
                         (void*)(fb_phys + i * PAGE_SIZE),
                         PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        }
        BOOT_LOG("[KERNEL] Framebuffer mapped for userspace at 0x%lx\n", (unsigned long)fb_user_vaddr);

    } else {
        BOOT_LOG("[KERNEL] No framebuffer available, skipping graphics init\n");
    }

    // Arm the PIT as a MONOTONIC TICK COUNTER. The IRQ0 handler increments a
    // tick counter and never reschedules, so cooperative scheduling is
    // preserved (preemptive scheduling is a later milestone). This gives
    // userspace a real time source via SYS_GET_TICKS_MS.
    //
    // POWER: 1000 Hz fires the timer IRQ 1000 times/second. On a laptop
    // (T410) every IRQ wakes the CPU from HLT/C1, costing power. 250 Hz is
    // the sweet spot: 4 ms tick granularity is fine for the desktop compositor
    // (~60 fps = 16 ms) and sleep precision. The scheduler's DEFAULT_TIME_SLICE
    // (10 ticks) becomes 40 ms at 250 Hz -- plenty for a cooperative / light-
    // preempt workload. Enabled by -DT410_POWER_SAVE at build time; default
    // build keeps 1000 Hz for maximum timing precision.
    boot_mark("timer (PIT)");
    extern void pit_init(uint32_t freq_hz);
#ifdef T410_POWER_SAVE
    pit_init(250);
#else
    pit_init(1000);
#endif
#ifdef T410_POWER_SAVE
  #ifdef PREEMPTIVE
    BOOT_LOG("[KERNEL] Timer: 250 Hz (power-save), PREEMPTIVE\n");
  #else
    BOOT_LOG("[KERNEL] Timer: tick counter armed (250 Hz, power-save, cooperative)\n");
  #endif
#else
  #ifdef PREEMPTIVE
    BOOT_LOG("[KERNEL] Timer: 1000 Hz, PREEMPTIVE (IRQ0 -> schedule_from_irq; ring-3 time-slicing)\n");
  #else
    BOOT_LOG("[KERNEL] Timer: tick counter armed (1000 Hz, cooperative)\n");
  #endif
#endif
    boot_mark("timer ok");

#ifdef HDA_ENABLE
    /* HD Audio: PCI-scan for an Intel HDA controller, reset it, set up
     * CORB/RIRB, enumerate the codec, and configure a DAC->pin output path.
     * Must come after pci_init() AND pit_init() -- hda_msleep() uses
     * timer_get_ticks() which requires a running PIT tick counter.
     * OPT-IN via HDA_ENABLE (DEFAULT OFF): T410_SAFE_BOOT used to gate this, but
     * since T410_SAFE_BOOT is always defined, HDA was compiled out EVERYWHERE --
     * including QEMU, which is why sound never worked. The real T410's HDA hangs
     * during controller reset / codec enumeration, so HDA stays off on the safe
     * boot path; QEMU's emulated HDA works fine -- build HDA_ENABLE=1 for audio.
     * Safe when no HDA device is present (hda_init returns cleanly). */
    boot_mark("audio (HDA)");
    BOOT_LOG("[KERNEL] Initializing HD Audio (HDA)...\n");
    /* audio_init() is the documented sole audio entry point: it brings up the
     * HDA controller (hda_init, a clean no-op when no controller is present),
     * then registers the default playback device + initialises /dev/dsp so
     * userspace (aplay / the mixer) has an audio device to open. This is the B0
     * keystone -- previously only hda_init() ran, so the controller came up but
     * no device was ever exposed to userspace. Returns <0 cleanly (no hang) when
     * there is no codec; the AUDIO_SELFTEST tones below still find the controller. */
    extern int audio_init(void);
    audio_init();
    boot_mark("audio ok");

#ifdef AUDIO_SELFTEST
    /* AUDIO-SELFTEST (build flag -DAUDIO_SELFTEST, DEFAULT OFF -- no boot chime
     * in normal builds): play one short 440 Hz / 200 ms test tone so a headless
     * verify can assert real DMA playback. audio_play_tone() prints the marker
     *   "AUDIO: tone done bcis=<N> lpib_adv=<D>"
     * and N>0 OR D>0 proves the DMA engine advanced. Safe no-op if no HDA HW
     * (audio_play_tone returns negative without playing or hanging). */
    {
        extern int audio_play_tone(uint32_t, uint32_t);
        audio_play_tone(440, 200);
        /* AUDIO-MIXER end-to-end proof: two tones SUMMED by amix_mix_period,
         * played through HDA DMA. Marker "AUDIO: mixed done bcis=<N> lpib_adv=<D>"
         * (N>0 OR D>0) proves the software mixer drives real audio hardware. */
        extern int audio_play_mixed(uint32_t, uint32_t, uint32_t);
        audio_play_mixed(440, 660, 200);
    }
#endif
#else
    BOOT_LOG("[KERNEL] HDA audio skipped (build with HDA_ENABLE=1 to enable; T410-unsafe)\n");
#endif

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

    /* AHCI/SATA + diskfs are now gated behind DISK_PERSIST (opt-in), DECOUPLED
     * from T410_SAFE_BOOT so durable persistence can be enabled WITHOUT also
     * re-enabling the NVIDIA GPU probe (the other T410 post-splash-hang suspect).
     * Default OFF -> boot-to-RAM stays the safe default; build with DISK_PERSIST=1
     * (scripts/quick_build.sh) to make /persist-backed files reboot-durable. The
     * IDE config falls back to a session file when this is off (ahci_present()==0). */
#ifdef DISK_PERSIST
    boot_mark("storage (AHCI/SATA disk)");
    BOOT_LOG("[KERNEL] Initializing block layer + AHCI/SATA...\n");
    extern void block_init(void);
    extern int  ahci_init(void);
    extern int  ahci_present(void);
    extern int  ahci_read(uint64_t lba, uint32_t count, void* buf);
    block_init();
    if (ahci_init() == 0 && ahci_present()) {
        uint8_t s0[512];
        if (ahci_read(0, 1, s0) == 0) {
            uint16_t sig = (uint16_t)(s0[510] | ((uint16_t)s0[511] << 8));
            BOOT_LOG("[AHCI] sector0 read OK: MBR sig=0x%04x "
                    "first8=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                    sig, s0[0], s0[1], s0[2], s0[3],
                    s0[4], s0[5], s0[6], s0[7]);
        } else {
            BOOT_LOG("[AHCI] sector0 read FAILED\n");
        }
    }

    // Persistence: durable on-disk superblock with a cross-boot counter. Needs
    // AHCI up (done above). No-op when no SATA disk is attached. The boot
    // counter incrementing across reboots of the same image proves durable
    // read+write+verify (see scripts/smoke_persist.sh).
    boot_mark("storage ok");
    boot_mark("diskfs (read disk)");
    BOOT_LOG("[KERNEL] Initializing persistent diskfs...\n");
    extern void diskfs_init(void);
    diskfs_init();
    boot_mark("diskfs ok");
#else
    // DISK_PERSIST not defined (default): skip block/AHCI/SATA + diskfs entirely.
    // ahci_init() pokes the real SATA controller's MMIO (untestable in QEMU and a
    // T410 post-splash-hang suspect); the boot is RAM-rooted, so no disk is needed
    // to reach the desktop. Build with DISK_PERSIST=1 to enable durable storage.
    boot_mark("storage SKIPPED (no DISK_PERSIST)");
#endif

    // Networking: detect the Intel e1000 NIC, read its MAC, assign the static
    // QEMU user-net IPv4 (10.0.2.15). PCI is up, so this is everything net_init
    // needs. The e1000 BAR sits in the <4GB MMIO hole, which the 16GB identity
    // map covers. With no NIC present net_init() returns non-zero and leaves
    // networking down (SYS_NET_INFO then returns ENOTSUP) -- a safe no-op.
    //
    // PCH NIC (82577LM on T410): the driver now uses the ME-safe reset sequence
    // (SWFLAG acquisition before CTRL_RST) and the full PHY bring-up over MDIO.
    // Link-up polling allows ~3 seconds for real auto-negotiation; a no-cable
    // T410 exits the poll in that time and boots with link=DOWN (not a hang).
    // In QEMU the classic e1000 path is unchanged. GPU + disk remain
    // gated by T410_SAFE_BOOT above.
    boot_mark("network (e1000)");
    BOOT_LOG("[KERNEL] Initializing networking (e1000)...\n");
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

    // WIFI-SEAM: one-shot proof the wifi_ops seam compiled + resolves. Prints
    // "WIFISEAM: PASS ..." (registers nothing; eth0.wifi stays NULL). Runs
    // BEFORE any wifi backend registers, so its wifi_default==none holds.
    extern void wifi_seam_selftest(void);
    wifi_seam_selftest();

#if defined(WIFI_SIM) && defined(IWLWIFI)
#error "wlan0 backend conflict: build WIFI_SIM (simulated) XOR IWLWIFI (real radio), not both -- both register wlan0"
#endif

#ifdef WIFI_SIM
    // WIFI-SIM: register the simulated wlan0 backend (fixed AP list + simulated
    // connect) so the whole scan->connect->DHCP flow runs in QEMU with no radio.
    // The real iwlwifi driver replaces this behind the same wifi_ops seam.
    extern void wifisim_init(void);
    wifisim_init();
#endif

#ifdef IWLWIFI
    // IWL-IDENT: detect the real Intel WiFi card + a single safe MMIO probe
    // (DEFAULT OFF; APM/firmware/RF bring-up is the deferred T410 hardware tail).
    // In QEMU there is no iwlwifi card -> graceful "no card found".
    extern void iwl_init(void);
    iwl_init();
    // IWL-FW: prove the firmware-TLV parser on an embedded synthetic .ucode
    // (the real iwlwifi-<fam>-*.ucode is parsed via iwl_fw_load_from_initrd on
    // the T410). Hostile-input-safe; QEMU-checkable.
    extern int iwl_fw_selftest(void);
    iwl_fw_selftest();
    // IWL DVM pure-logic KATs: prove the RXON builder, the scan-command builder,
    // and the beacon/IE parser against known-answer bytes -- the software-provable
    // half of the radio bring-up (the RF tail itself has no emulator). QEMU-checkable.
    extern int iwl_rxon_selftest(void);
    iwl_rxon_selftest();
    extern int iwl_scan_selftest(void);
    iwl_scan_selftest();
    // IWL firmware AUTO-SELECT KAT: the card's PCI id -> family -> the matching
    // iwlwifi-<fam>-<api>.ucode is picked (newest API first, alias fallback), so
    // the operator can bundle ALL DVM blobs and WiFi auto-selects per card.
    extern int iwl_fwselect_selftest(void);
    iwl_fwselect_selftest();
#endif

#ifdef NET_SELFTEST
    // NET-P1-A0: the deterministic in-kernel net test rig proof. Runs once,
    // pre-userspace, prints the NETRIG marker, and fully resets the socket
    // table on its way out. Compiled only under NET_SELFTEST=1 builds.
    {
        extern void net_testrig_selftest(void);
        net_testrig_selftest();
    }
#endif

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
    BOOT_LOG("[KERNEL] Initializing VFS...\n");
    vfs_init();
    BOOT_LOG("[KERNEL] VFS initialized\n");

    // Initialize filesystem drivers
    boot_mark("fs drivers (ext2/fat32)");
    BOOT_LOG("[KERNEL] Initializing filesystem drivers...\n");
    extern void ext2_init(void);
    extern void fat32_init(void);
    ext2_init();
    fat32_init();
    BOOT_LOG("[KERNEL] Filesystem drivers initialized\n");

    boot_mark("mount root (ramfs)");
    BOOT_LOG("[KERNEL] Mounting root filesystem (ramfs)...\n");
    if (vfs_mount("none", "/", "ramfs") == 0) {
        BOOT_LOG("[KERNEL] Root filesystem mounted\n");
    } else {
        kprintf("[KERNEL] ERROR: Failed to mount root filesystem\n");
    }

    // Create mount points for additional filesystems
    BOOT_LOG("[KERNEL] Creating mount points...\n");
    vfs_mkdir("/mnt", 0755);
    vfs_mkdir("/mnt/data", 0755);
    vfs_mkdir("/mnt/ext2", 0755);
    vfs_mkdir("/mnt/usb", 0755);
    vfs_mkdir("/home", 0755);
    // /Desktop: the compositor enumerates this as clickable wallpaper icons and the
    // IDE compiles programs here (double-click a .elf icon spawns it). Create it at
    // boot so the desktop scan + the IDE's build-output write always have a target.
    vfs_mkdir("/Desktop", 0777);
    BOOT_LOG("[KERNEL] Mount points created\n");

    BOOT_LOG("[KERNEL] Creating /dev directory...\n");
    if (vfs_mkdir("/dev", 0755) == 0) {
        BOOT_LOG("[KERNEL] /dev directory created\n");
    } else {
        BOOT_LOG("[KERNEL] Warning: /dev may already exist\n");
    }

    // Attempt to auto-mount detected filesystems if AHCI is available.
    // DISABLED: parsing a real, foreign/unknown on-disk ext2/fat32 at boot is
    // unvetted on real hardware and can hang the boot. The OS roots on the RAM
    // ramfs and needs no physical disk to reach the desktop ("boot to RAM").
    // Re-enable once ext2/fat32 mount is hardened + tested on real hardware.
    if (0 /* was: ahci_present() */) {
        boot_mark("disk automount (ext2/fat32)");
        BOOT_LOG("[KERNEL] Detecting and mounting filesystems...\n");

        // Try to mount ext2 from first SATA drive
        // Note: AHCI driver registers devices as "sata0", "sata1", etc.
        BOOT_LOG("[KERNEL] Attempting to mount sata0 as ext2...\n");
        if (vfs_mount("sata0", "/mnt/ext2", "ext2") == 0) {
            BOOT_LOG("[FS] Successfully mounted /mnt/ext2 as ext2\n");

            // Test filesystem access
            BOOT_LOG("[FS] Testing ext2 filesystem access...\n");
            vfs_stat_t stat;
            if (vfs_stat("/mnt/ext2", &stat) == 0) {
                BOOT_LOG("[FS] ext2 root directory accessible (inode=%lu)\n", (unsigned long)stat.st_ino);
            }
        } else {
            BOOT_LOG("[FS] No ext2 filesystem found on sata0 (or device not present)\n");
        }

        // Try to mount FAT32 from first SATA drive (alternative attempt)
        // In a partition-aware system, this would be sata0p1, sata0p2, etc.
        BOOT_LOG("[KERNEL] Attempting to mount sata0 as fat32...\n");
        if (vfs_mount("sata0", "/mnt/data", "fat32") == 0) {
            BOOT_LOG("[FS] Successfully mounted /mnt/data as FAT32\n");

            // Test filesystem access
            BOOT_LOG("[FS] Testing FAT32 filesystem access...\n");
            vfs_stat_t stat;
            if (vfs_stat("/mnt/data", &stat) == 0) {
                BOOT_LOG("[FS] FAT32 root directory accessible (inode=%lu)\n", (unsigned long)stat.st_ino);
            }
        } else {
            BOOT_LOG("[FS] No FAT32 filesystem found on sata0 (already mounted as ext2 or wrong format)\n");
        }

        BOOT_LOG("[KERNEL] Filesystem detection complete\n");
        BOOT_LOG("[FS] Note: Current implementation mounts entire drive, not partitions\n");
        BOOT_LOG("[FS] For partition support, implement MBR/GPT partition table parsing\n");
    } else {
        BOOT_LOG("[KERNEL] No block storage detected, skipping filesystem mount\n");
    }

    BOOT_LOG("[KERNEL] Creating PTY device nodes...\n");
    if (vfs_mkdir("/dev/pts", 0755) == 0) {
        BOOT_LOG("[KERNEL] /dev/pts directory created\n");
    } else {
        BOOT_LOG("[KERNEL] Warning: /dev/pts may already exist\n");
    }
    // TODO: Create /dev/ptmx and hook up PTY file operations
    BOOT_LOG("[KERNEL] PTY device nodes created\n");

    // Input + keyboard MUST init after VFS mount + /dev mkdir, so that
    // dev_input_init() can create /dev/input and ps2_init() can link the
    // /dev/input/eventN device nodes into the mounted ramfs tree.
    boot_mark("input subsystem");
    BOOT_LOG("[KERNEL] Initializing input subsystem...\n");
    extern void input_init(void);
    extern void dev_input_init(void);
    input_init();
    dev_input_init();
    BOOT_LOG("[KERNEL] Input subsystem initialized\n");

    boot_mark("keyboard/mouse (PS/2)");
    BOOT_LOG("[KERNEL] Initializing keyboard + mouse...\n");
    extern void ps2_init(void);
    ps2_init();
    // NOTE: ps2mouse_init() (kernel/drivers/input/ps2mouse.c) is DELIBERATELY NOT
    // called. It used to run after ps2_init() and OVERWRITE the IRQ12 handler with
    // a "richer" one — but that created two fatal problems on real hardware:
    //   1) ROUTING BUG: ps2_init() registers the mouse as /dev/input/event1 and
    //      the compositor reads event1; ps2mouse_init() registered a SECOND mouse
    //      device (event2) and pointed IRQ12 at it. Result: real mouse bytes went
    //      to event2 while the compositor polled event1 -> cursor never moved.
    //   2) FRAGILITY: ps2mouse.c's init waits ignore their timeouts and it logs
    //      "bad sync" from IRQ context; on the T410's Synaptics touchpad that
    //      mis-syncs readily, flooding IRQ12 and (pre-serial-IRQ-safe-fix) could
    //      deadlock. The double-init also left the device in an inconsistent rate.
    // ps2_init()'s own mouse handler is hardened (bounded waits, silent bad-packet
    // drop, no IRQ-context logging) and feeds event1 — exactly what the compositor
    // reads. So we keep ONE mouse driver. (Re-enabling ps2mouse needs it to claim
    // event1, not event2, and to drop its IRQ-context kprintf first.)
    // extern void ps2mouse_init(void);
    // ps2mouse_init();
#ifdef USB_UHCI
    /* USB-MOUSE-0 (gated, default OFF): bring up the UHCI host controller and
     * enumerate a wired boot mouse. uhci.c is self-contained -- it enumerates the
     * root hub and injects pointer events through the SAME input_report_* and
     * input_sync path as the PS/2 mouse (above) -- the compositor needs no new path.
     * Safe: returns cleanly with no controller/device, every controller/transfer
     * wait is bounded, and uhci_poll() is driven from timer_handler() (pit.c).
     * NOTE: usb_core.c::usb_init() is a SIMULATION stub; the real hardware entry is
     * uhci_init() -- a usb_hc dispatch wrapper can wrap it when EHCI/xHCI land. */
    {
        extern int uhci_init(void);
        uhci_init();
    }
#endif
    BOOT_LOG("[KERNEL] Keyboard + mouse initialized\n");
    boot_mark("keyboard/mouse ok");

    boot_mark("pty");
    BOOT_LOG("[KERNEL] Initializing PTY subsystem...\n");
    extern void pty_init(void);
    pty_init();
    BOOT_LOG("[KERNEL] PTY subsystem initialized\n");

    BOOT_LOG("[KERNEL] Initializing process table...\n");
    process_init();
    BOOT_LOG("[KERNEL] Process table initialized\n");

    BOOT_LOG("[KERNEL] Initializing scheduler...\n");
    scheduler_init();
    BOOT_LOG("[KERNEL] Scheduler initialized\n");

    BOOT_LOG("[KERNEL] Initializing performance monitoring...\n");
    perf_init();
    BOOT_LOG("[KERNEL] Performance monitoring initialized\n");

    BOOT_LOG("\n");
    BOOT_LOG("[KERNEL] All subsystems initialized!\n");
    BOOT_LOG("[KERNEL] Free memory: %lu MB\n",
            (unsigned long)(pmm_get_free_memory() / (1024 * 1024)));

    uint64_t boot_end = rdtsc();
    uint64_t boot_cycles = boot_end - boot_start;
    BOOT_LOG("[BOOT] Total boot time: %lu.%02lu ms\n",
            (unsigned long)cycles_to_ms(boot_cycles),
            (unsigned long)cycles_to_ms_frac(boot_cycles));

    BOOT_LOG("\n");
    BOOT_LOG("=====================================\n");
    BOOT_LOG("   AutomationOS BOOT COMPLETE!\n");
    BOOT_LOG("   All kernel subsystems: ONLINE\n");
    BOOT_LOG("   created by fourzerofour & claude\n");
    BOOT_LOG("=====================================\n");
    BOOT_LOG("\n");

    if (boot_info->initrd_addr && boot_info->initrd_size) {
        BOOT_LOG("[KERNEL] Loading initrd...\n");
        initrd_init(boot_info->initrd_addr, boot_info->initrd_size);

        BOOT_LOG("[KERNEL] Mounting initrd as root filesystem...\n");
        if (initrd_mount() == 0) {
            BOOT_LOG("[KERNEL] Initrd mounted successfully\n");

            /* Create writable scratch dirs (/tmp, /var/tmp, /run) on the ramfs. */
            extern void vfs_fs_init(void);
            vfs_fs_init();

            /* List initrd contents for debugging (gated: ~30 serial lines) */
#ifndef BOOT_QUIET
            initrd_list_files();
#endif

            /* Try to load and execute /sbin/init */
            BOOT_LOG("[KERNEL] Loading /sbin/init...\n");

            uint64_t init_size = 0;
            void* init_data = initrd_get_file("sbin/init", &init_size);

            if (init_data && init_size > 0) {
                BOOT_LOG("[KERNEL] Found init: %lu bytes\n", (unsigned long)init_size);

                /* Load init as ELF binary */
                int pid = elf_load_and_exec(init_data, init_size, "/sbin/init");

                if (pid > 0) {
                    BOOT_LOG("[KERNEL] Init process started (PID %d)\n", pid);

#ifdef SMP_FOUNDATION
                    /* Deferred from the SMP brick above: start the health-monitor
                     * kernel thread NOW that /sbin/init has claimed PID 1, so the
                     * monitor thread takes a later PID instead of stealing PID 1. */
                    health_monitor_start_thread();
                    BOOT_LOG("[HEALTH] Monitor thread started (5s sampling)\n");
#endif

#ifdef SMP_SCHED
                    /* Brick D: populate CPU1's per-CPU scheduler slot (idle thread +
                     * empty runqueues + online) now that init owns PID 1, so CPU1's
                     * idle takes a normal free PID instead of stealing PID 1. CPU1 is
                     * still a coprocessor (no dispatch yet, Bricks E/F); this is pure
                     * BSP-side data init that those bricks build on. */
                    {
                        extern int madt_get_apic_id(int index);
                        int ap_apic = madt_get_apic_id(1);
                        uint32_t ap_apic_id = (ap_apic < 0) ? 1u : (uint32_t)ap_apic;
                        if (scheduler_init_secondary_cpu(1, ap_apic_id)) {
                            BOOT_LOG("[SMP] Brick D: CPU1 scheduler slot initialized\n");
#ifdef SMP_SCHED_DISPATCH
#ifdef SMP_IPI
                            /* SMP-G1: the no-lost-wake ping proof MUST run HERE --
                             * after CPU1's scheduler slot is live but BEFORE the F2
                             * kthread spawn, the only window where CPU1 is
                             * genuinely hlt-parked on an EMPTY runqueue (the exact
                             * state the cli/check/sti;hlt close protects). 32
                             * IPI pings, each acked by the idle loop's cli'd
                             * check; ticks cannot ack, so every ack = an IPI woke
                             * the hlt. */
                            {
                                extern void ipiwake_ping_selftest(void);
                                ipiwake_ping_selftest();
                            }
                            /* SMP-G2: one real kernel-range shootdown end to
                             * end (local invlpg + acked IPI_TLB_FLUSH_PAGE to
                             * the still-idle CPU1) + the pin-model audit that
                             * proves user ranges need no cross-flush. Same
                             * window as the pings: CPU1 hlt-parked, BSP
                             * serial-safe. */
                            {
                                extern void tlb_shootdown_selftest(void);
                                tlb_shootdown_selftest();
                            }
#endif
                            /* SMP-F3-6: prove THE placement seam's branches
                             * (legality clamps, pin wins, role, home stub)
                             * before any seam-routed placement below runs. */
                            scheduler_choosecpu_selftest();
                            /* SMP-PROFILE-0: the typed profile exists, the
                             * seam reads it, and nothing routes differently
                             * because of it (BATCH = data, not migration). */
                            scheduler_profile_selftest();
#ifdef SMP_BATCH
                            /* SMP-F3-7: the batch branch routes exactly as
                             * specified, bounded by the legality walls. */
                            {
                                extern void scheduler_batchclass_selftest(void);
                                scheduler_batchclass_selftest();
                            }
#endif
#ifdef SMP_RUNMASK
                            /* SMP-RUNMASK-0: the audit audits reality --
                             * baseline clean, a PLANTED cross-CPU footprint
                             * is detected loudly, restore is clean. */
                            {
                                extern void runmask_selftest(void);
                                runmask_selftest();
                            }
#endif
#ifdef SMP_THREAD_INHERIT
                            /* SMP-THREAD-INHERIT-0: prove the inheritance
                             * predicate (a BATCH-CPU1 parent's thread inherits
                             * CPU1+BATCH; a NORMAL parent's thread stays CPU0)
                             * before the threaded probe below relies on it. */
                            {
                                extern void threadinherit_selftest(void);
                                threadinherit_selftest();
                            }
#endif
                            /* Brick F2: pin ONE ring-0 kernel test thread to CPU1.
                             * CPU1's ap_scheduler_loop context-switches into it on
                             * the next tick. Spin briefly on the BSP, then read
                             * ap_kthread_counter: a positive delta PROVES CPU1 did
                             * the first real AP context switch and is running the
                             * thread in parallel with CPU0. (The earlier heartbeat
                             * window ran before this spawn, hence its delta=0.) */
                            ap_spawn_test_kthread();
                            {
                                extern volatile uint64_t ap_kthread_counter;
                                uint64_t c0 = ap_kthread_counter;
                                for (volatile long d = 0; d < 8000000L; d++) {
                                    __asm__ volatile("pause");
                                }
                                uint64_t c1 = ap_kthread_counter;
                                /* kprintf (not BOOT_LOG): cpu1_smoke.sh greps this
                                 * gate line, and BOOT_LOG is suppressed by the
                                 * always-on -DBOOT_QUIET in quick_build CFLAGS. */
                                kprintf("[SMP] Brick F2 VERIFY: AP kthread counter "
                                        "%lu -> %lu (delta=%lu; >0 proves the FIRST "
                                        "AP context switch into a scheduled thread)\n",
                                        c0, c1, c1 - c0);

                                /* F3-4 APCURRENT: the kthread (on CPU1) called the
                                 * real process_get_current() and proved it resolves
                                 * CPU1-local (distinct from CPU0's current). The BSP
                                 * reads the result the kthread set and emits the
                                 * single greppable gate line. Proves the per-cpu
                                 * "current" routing works from a real CPU1 context
                                 * -- the prerequisite for a safe AP syscall. */
                                extern volatile int ap_current_probe_result;
                                if (ap_current_probe_result == 1) {
                                    kprintf("[SMP] APCURRENT: PASS (cpu1 process_get_current() "
                                            "is cpu1-local + distinct from cpu0)\n");
                                } else {
                                    kprintf("[SMP] APCURRENT: FAIL (result=%d -- per-cpu "
                                            "current resolution broken)\n",
                                            ap_current_probe_result);
                                }
                            }

#endif
                        } else {
                            BOOT_LOG("[SMP] Brick D: CPU1 slot init FAILED (CPU1 stays coprocessor)\n");
                        }
                    }
#endif

                    /* Initialize TSS for ring 3 transitions */
                    boot_mark("TSS init");
#ifdef SMP_SCHED
                    /* Brick B: already initialized the per-CPU TSS earlier (before
                     * AP bring-up); do NOT re-run it here (would re-zero/re-ltr). */
                    BOOT_LOG("[KERNEL] TSS already initialized (per-CPU, pre-AP)\n");
#else
                    BOOT_LOG("[KERNEL] Initializing TSS for usermode...\n");
                    tss_init();
                    BOOT_LOG("[KERNEL] TSS initialized\n");
#endif
                    boot_mark("TSS ok");

                    /* Initialize syscall dispatch table */
                    boot_mark("syscall dispatch table");
                    extern void syscall_init(void);
                    syscall_init();
                    BOOT_LOG("[KERNEL] Syscall dispatch table initialized\n");
                    boot_mark("syscall dispatch ok");

                    /* Initialize IPC subsystems (shared memory, message queues, notifications, clipboard) */
                    boot_mark("IPC init");
                    extern void ipc_init(void);
                    ipc_init();
                    BOOT_LOG("[KERNEL] IPC initialized\n");
                    boot_mark("IPC ok");

                    /* Initialize SYSCALL/SYSRET MSRs for userspace */
                    boot_mark("syscall MSR init");
                    extern void syscall_msr_init(void);
                    syscall_msr_init();
                    BOOT_LOG("[KERNEL] SYSCALL/SYSRET MSRs programmed\n");
                    boot_mark("syscall MSR ok");

#if defined(SMP_SCHED) && defined(SMP_SCHED_DISPATCH)
                    /* F3-5: spawn cpu1hello -- the FIRST ring-3 process on
                     * CPU1. MUST be enqueued only NOW: CPU1 dispatches within
                     * a tick of the enqueue, and a CPU1 syscall needs the
                     * GLOBAL dispatch table that syscall_init() (above) just
                     * registered. The first attempt enqueued it back at the
                     * F2 block -- before syscall_init -- and every cpu1hello
                     * syscall no-op'd: writes vanished, sys_exit RETURNED,
                     * and crt0's hlt guard took a CPL3 #GP. Load it normally
                     * (the BSP scheduler still hasn't started, so the CPU0
                     * enqueue is inert), then dequeue, set init as the
                     * reaping parent + the CPU1 pin, and enqueue on cpus[1]. */
                    {
                        uint64_t h_size = 0;
                        void* h_data = initrd_get_file("sbin/cpu1hello", &h_size);
                        if (h_data && h_size > 0) {
                            int hpid = elf_load_and_exec(h_data, h_size,
                                                         "cpu1hello");
                            process_t* h = (hpid > 0)
                                         ? process_get_by_pid(hpid) : NULL;
                            if (h) {
                                scheduler_remove_process(h);
                                process_t* ini = process_get_by_pid(1);
                                if (ini) {
                                    h->parent_pid = 1;
                                    h->parent_seq = ini->create_seq;
                                    process_unref(ini);
                                }
                                h->allowed_cpus = (uint64_t)1 << 1;
                                h->pinned_cpu   = 1;
#ifdef SMP_IPI
                                /* SMP-G1: stamp THIS enqueue. The add below now
                                 * sends an IPI_RESCHEDULE (the G1 enqueue->kick
                                 * path); CPU1's first switch into this pid stamps
                                 * dispatch_tsc and prints the latency -- the
                                 * end-to-end enqueue_to_cpu1 + first-dispatch
                                 * proof through the REAL scheduler path. */
                                {
                                    extern volatile uint64_t g_g1_enq_tsc;
                                    extern volatile int      g_g1_enq_pid;
                                    g_g1_enq_pid = hpid;
                                    g_g1_enq_tsc = rdtsc();   /* perf.h inline */
                                }
#endif
                                /* F3-6: the one real ring-3 placement goes
                                 * through THE seam; PROFILE-0: declared
                                 * PINNED_RT and submitted via the NAMED
                                 * funnel. The CPU1HELLO ladder still passing
                                 * IS the live proof the typed path places
                                 * identically. (The F3-6 marker line is kept
                                 * verbatim -- choosecpu_smoke.sh greps it.) */
                                {
                                    h->sched.sched_class = SCHED_CLASS_PINNED_RT;
                                    uint32_t h_target = scheduler_submit_task(h);
                                    kprintf("[SMP] F3-6: cpu1hello placed via "
                                            "scheduler_choose_cpu -> cpu%u\n",
                                            h_target);
                                }
                                kprintf("[SMP] F3-5: cpu1hello PID %d pinned + "
                                        "enqueued on CPU1\n", hpid);
                                process_unref(h);
                            } else {
                                kprintf("[SMP] F3-5: cpu1hello load FAILED (%d)\n",
                                        hpid);
                            }
                        } else {
                            kprintf("[SMP] F3-5: sbin/cpu1hello not in initrd -- "
                                    "skipped\n");
                        }
                    }
#ifdef SMP_BATCH
                    /* SMP-F3-7 acceptance: the first ORDINARY workload on
                     * CPU1. Deliberately NOT pinned and NOT special-cased:
                     * class=BATCH + a multi-CPU legal mask, and the seam's
                     * batch branch (not this call site) decides CPU1. The
                     * G1 IPI kick wakes the core; the enqueue->dispatch
                     * stamp proves it (sub-ms = IPI, not the 10 ms tick).
                     * ORDER MATTERS: this spawns BEFORE the SMP_BKL storms
                     * below -- the wake-latency proof needs a near-idle
                     * CPU1; the first run spawned it after the 60 s CPU1
                     * storm and measured 578 ms of STORM-SHARING, then
                     * starved past the QEMU window (the ordering find). */
                    {
                        uint64_t d_size = 0;
                        void* d_data = initrd_get_file("sbin/batchdemo", &d_size);
                        if (d_data && d_size > 0) {
                            int dpid = elf_load_and_exec(d_data, d_size,
                                                         "batchdemo");
                            process_t* dm = (dpid > 0)
                                          ? process_get_by_pid(dpid) : NULL;
                            if (dm) {
                                scheduler_remove_process(dm);
                                process_t* ini = process_get_by_pid(1);
                                if (ini) {
                                    dm->parent_pid = 1;
                                    dm->parent_seq = ini->create_seq;
                                    process_unref(ini);
                                }
                                dm->allowed_cpus = ((uint64_t)1 << 0) |
                                                   ((uint64_t)1 << 1);
                                dm->pinned_cpu   = CPU_NONE;     /* NOT pinned */
                                dm->sched.sched_class = SCHED_CLASS_BATCH;
                                {
                                    extern volatile uint64_t g_f37_enq_tsc;
                                    extern volatile int      g_f37_enq_pid;
                                    g_f37_enq_pid = dpid;
                                    g_f37_enq_tsc = rdtsc();
                                }
                                uint32_t d_target = scheduler_submit_task(dm);
                                kprintf("[SMP] F3-7: batchdemo PID %d "
                                        "class=BATCH unpinned -> the seam "
                                        "chose cpu%u\n", dpid, d_target);
                                process_unref(dm);
                            }
                        } else {
                            kprintf("[SMP] F3-7: sbin/batchdemo not in initrd "
                                    "-- skipped (rebuild initrd)\n");
                        }
                    }
#endif
#ifdef SMP_THREAD_INHERIT
                    /* SMP-THREAD-INHERIT-0 acceptance: a THREADED BATCH workload
                     * on CPU1 whose worker threads INHERIT the parent's CPU1
                     * placement (one address space, one execution CPU). Spawned
                     * EXACTLY like batchdemo (class=BATCH + a multi-CPU legal
                     * mask -> the seam's batch branch picks CPU1) -- and pointedly
                     * NOT added to the sys_spawn allowlist (no_allowlist_expansion).
                     * scheduler_submit_task records the chosen CPU as the mm's
                     * home; when threadprobe runs there and creates its 2 worker
                     * threads, thread_create pins them to that home CPU. The
                     * [THREADINHERIT] observation + ran_on_cpus ground truth prove
                     * parent+workers all run CPU1 and the mm never spans two CPUs. */
                    {
                        uint64_t tp_size = 0;
                        void* tp_data = initrd_get_file("sbin/threadprobe", &tp_size);
                        if (tp_data && tp_size > 0) {
                            int tppid = elf_load_and_exec(tp_data, tp_size,
                                                          "threadprobe");
                            process_t* tp = (tppid > 0)
                                          ? process_get_by_pid(tppid) : NULL;
                            if (tp) {
                                scheduler_remove_process(tp);
                                process_t* ini = process_get_by_pid(1);
                                if (ini) {
                                    tp->parent_pid = 1;
                                    tp->parent_seq = ini->create_seq;
                                    process_unref(ini);
                                }
                                tp->allowed_cpus = ((uint64_t)1 << 0) |
                                                   ((uint64_t)1 << 1);
                                tp->pinned_cpu   = CPU_NONE;     /* the seam decides */
                                tp->sched.sched_class = SCHED_CLASS_BATCH;
                                uint32_t tp_target = scheduler_submit_task(tp);
                                kprintf("[SMP] THREAD-INHERIT: threadprobe PID %d "
                                        "class=BATCH unpinned -> the seam chose "
                                        "cpu%u (workers inherit it)\n",
                                        tppid, tp_target);
                                process_unref(tp);
                            }
                        } else {
                            kprintf("[SMP] THREAD-INHERIT: sbin/threadprobe not in "
                                    "initrd -- skipped (rebuild initrd)\n");
                        }
                    }
#endif
#ifdef SMP_BKL
                    /* SMP-H1 BKL-LITE acceptance: TWO 60 s syscall storms,
                     * one pinned to CPU1 (the cpu1hello placement pattern),
                     * one left NORMAL (the funnel homes it to CPU0). Both
                     * hammer the MARKED groups concurrently; the shm pattern
                     * verify inside each is the corruption detector and the
                     * [BKL] engaged counters prove both CPUs executed marked
                     * paths under the wall. */
                    {
                        uint64_t b_size = 0;
                        void* b_data = initrd_get_file("sbin/bklstorm", &b_size);
                        if (b_data && b_size > 0) {
                            /* instance 1: pinned CPU1, PINNED_RT */
                            int bpid1 = elf_load_and_exec(b_data, b_size,
                                                          "bklstorm");
                            process_t* b1 = (bpid1 > 0)
                                          ? process_get_by_pid(bpid1) : NULL;
                            if (b1) {
                                scheduler_remove_process(b1);
                                process_t* ini = process_get_by_pid(1);
                                if (ini) {
                                    b1->parent_pid = 1;
                                    b1->parent_seq = ini->create_seq;
                                    process_unref(ini);
                                }
                                b1->allowed_cpus = (uint64_t)1 << 1;
                                b1->pinned_cpu   = 1;
                                b1->sched.sched_class = SCHED_CLASS_PINNED_RT;
                                scheduler_submit_task(b1);
                                kprintf("[SMP] H1: bklstorm PID %d -> CPU1 "
                                        "(60s marked-syscall storm)\n", bpid1);
                                process_unref(b1);
                            }
                            /* instance 2: untouched defaults = NORMAL,
                             * CPU0-only mask -> the funnel homes it */
                            int bpid2 = elf_load_and_exec(b_data, b_size,
                                                          "bklstorm");
                            process_t* b2 = (bpid2 > 0)
                                          ? process_get_by_pid(bpid2) : NULL;
                            if (b2) {
                                scheduler_remove_process(b2);
                                process_t* ini = process_get_by_pid(1);
                                if (ini) {
                                    b2->parent_pid = 1;
                                    b2->parent_seq = ini->create_seq;
                                    process_unref(ini);
                                }
                                scheduler_submit_task(b2);
                                kprintf("[SMP] H1: bklstorm PID %d -> CPU0 "
                                        "(60s marked-syscall storm)\n", bpid2);
                                process_unref(b2);
                            }
                        } else {
                            kprintf("[SMP] H1: sbin/bklstorm not in initrd -- "
                                    "storm skipped (rebuild initrd)\n");
                        }
                    }
#endif
#endif

                    /* Start scheduler (will enable interrupts after TSS.RSP0 is set) */
                    boot_mark("starting services (scheduler)");
                    BOOT_LOG("[KERNEL] Starting scheduler...\n");
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
        BOOT_LOG("[KERNEL] No initrd detected\n");
        BOOT_LOG("[KERNEL] Desktop environment requires UEFI boot or initrd\n");
    }

    BOOT_LOG("[KERNEL] Kernel initialization complete\n");
    BOOT_LOG("[KERNEL] Entering idle loop\n");
    sti();
    while (1) {
        hlt();
    }
}
