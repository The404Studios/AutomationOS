#include "../../include/x86_64.h"
#include "../../include/kernel.h"
#include "../../include/gdt_constants.h"
#include "../../include/tss.h"

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} PACKED gdt_entry_t;

// TSS descriptor is 16 bytes in x86_64 (2 GDT entries)
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
    uint32_t base_upper;
    uint32_t reserved;
} PACKED tss_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} PACKED gdt_ptr_t;

#ifdef SMP_SCHED
/* =====================================================================
 * SMP scheduler Brick B: per-CPU TSS + per-CPU IST.
 * ---------------------------------------------------------------------
 * One global TSS means CPU1's ring-3 entry would load CPU0's RSP0 -> wrong-stack
 * corruption -> #DF on a shared IST -> triple fault. So each logical CPU gets its
 * OWN TSS and its OWN #DF/NMI IST1 stack. Both CPUs share this ONE GDT (the AP
 * lgdt's the BSP's image); they just `ltr` DIFFERENT selectors so each owns a
 * distinct, non-busy TSS descriptor:
 *     gdt[5-6] = CPU0 TSS, selector 0x28   (BSP loads in tss_init)
 *     gdt[7-8] = CPU1 TSS, selector 0x38   (AP  loads in gdt_ap_load_tss)
 *
 * SIZED TO 2, NOT MAX_CPUS (256): ist1_stacks[256][8192] would be a 2 MB .bss
 * array that overruns into the GRUB-placed initrd (the documented .bss/initrd
 * overlap hazard). The SMP foundation has exactly two logical CPUs (BSP=0, AP=1)
 * and cpu_id() only ever returns 0 or 1, so 2 slots is correct and tiny (16 KB). */
#define SMP_SCHED_NCPU 2
extern uint32_t cpu_id(void);
static gdt_entry_t gdt[9];   /* 5 standard + CPU0 TSS (5-6) + CPU1 TSS (7-8) */
static gdt_ptr_t gdt_ptr;
static tss_t tss_array[SMP_SCHED_NCPU];
static uint8_t ist1_stacks[SMP_SCHED_NCPU][8192] __attribute__((aligned(16)));
#else
// GDT: 7 entries worth of space (5 standard + 2 for 16-byte TSS descriptor)
// TSS descriptor at entry 5 spans entries 5 and 6
static gdt_entry_t gdt[7];
static gdt_ptr_t gdt_ptr;
static tss_t tss;

// Dedicated 16 KB stack for #DF / NMI delivery (TSS IST1). Separate from the
// per-process kernel stack so a stack overflow that triggers #DF lands on a
// fresh, valid stack rather than re-faulting and triple-faulting.
static uint8_t ist1_stack[8192] __attribute__((aligned(16)));  /* 8 KB: enough for the #DF/NMI panic path */
#endif

extern void gdt_flush(uint64_t gdt_ptr);
extern void tss_flush(uint16_t tss_selector);

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_mid = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;

    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access = access;
}

#ifdef SMP_SCHED
/* Write a 16-byte 64-bit TSS descriptor for tss_array[cpu] into gdt[idx..idx+1]. */
static void tss_set_gate_cpu(int cpu, int idx) {
    uint64_t base = (uint64_t)&tss_array[cpu];
    uint32_t limit = sizeof(tss_t) - 1;

    gdt[idx].limit_low   = limit & 0xFFFF;
    gdt[idx].base_low    = base & 0xFFFF;
    gdt[idx].base_mid    = (base >> 16) & 0xFF;
    gdt[idx].access      = 0x89;  // Present, DPL=0, TSS Available (64-bit)
    gdt[idx].granularity = 0x00;
    gdt[idx].base_high   = (base >> 24) & 0xFF;

    uint32_t* hi = (uint32_t*)&gdt[idx + 1];
    hi[0] = (base >> 32) & 0xFFFFFFFF;  // base_upper
    hi[1] = 0;                          // reserved
}
#else
static void tss_set_gate(void) {
    uint64_t base = (uint64_t)&tss;
    uint32_t limit = sizeof(tss_t) - 1;

    // TSS descriptor is 16 bytes - write directly into gdt[5] and gdt[6]
    // Entry 5 (low 8 bytes):
    gdt[5].limit_low = limit & 0xFFFF;
    gdt[5].base_low = base & 0xFFFF;
    gdt[5].base_mid = (base >> 16) & 0xFF;
    gdt[5].access = 0x89;  // Present, DPL=0, TSS Available (64-bit)
    gdt[5].granularity = 0x00;
    gdt[5].base_high = (base >> 24) & 0xFF;

    // Entry 6 (high 8 bytes): upper 32 bits of base + reserved
    uint32_t* gdt6 = (uint32_t*)&gdt[6];
    gdt6[0] = (base >> 32) & 0xFFFFFFFF;  // base_upper
    gdt6[1] = 0;                            // reserved
}
#endif

_Static_assert(sizeof(gdt_entry_t) == 8, "GDT entry must be 8 bytes");
_Static_assert(sizeof(tss_entry_t) == 16, "TSS descriptor must be 16 bytes");

void gdt_init(void) {
    kprintf("[GDT] Initializing Global Descriptor Table...\n");
    kprintf("[GDT] sizeof(gdt)=%lu sizeof(gdt_entry_t)=%lu sizeof(tss_entry_t)=%lu\n",
            (unsigned long)sizeof(gdt), (unsigned long)sizeof(gdt_entry_t),
            (unsigned long)sizeof(tss_entry_t));

    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint64_t)&gdt[0];

    kprintf("[GDT] gdt_ptr: base=%p limit=%u\n", (void*)gdt_ptr.base, gdt_ptr.limit);

    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, GDT_LIMIT_FULL, GDT_ACCESS_KERNEL_CODE, GDT_GRAN_64BIT_CODE);
    gdt_set_gate(2, 0, GDT_LIMIT_FULL, GDT_ACCESS_KERNEL_DATA, GDT_GRAN_64BIT_DATA);
    gdt_set_gate(3, 0, GDT_LIMIT_FULL, GDT_ACCESS_USER_DATA, GDT_GRAN_64BIT_DATA);  // User data FIRST (SYSRET)
    gdt_set_gate(4, 0, GDT_LIMIT_FULL, GDT_ACCESS_USER_CODE, GDT_GRAN_64BIT_CODE);  // User code SECOND

    kprintf("[GDT] Standard entries set\n");

    // Load GDT without reloading segments (they already have correct selectors from boot)
    // Just extend the GDTR limit to include TSS entries
    kprintf("[GDT] lgdt (no segment reload)...\n");
    __asm__ volatile("lgdt %0" : : "m"(gdt_ptr));
    kprintf("[GDT] lgdt done\n");

    kprintf("[GDT] Global Descriptor Table initialized\n");
}

#ifdef SMP_SCHED
void tss_init(void) {
    kprintf("[TSS] Initializing per-CPU TSS array (%d CPUs)...\n", SMP_SCHED_NCPU);

    /* CPU0 TSS -> gdt[5-6] (sel 0x28), CPU1 TSS -> gdt[7-8] (sel 0x38). */
    static const int tss_gdt_idx[SMP_SCHED_NCPU] = { 5, 7 };

    for (int c = 0; c < SMP_SCHED_NCPU; c++) {
        uint8_t* ptr = (uint8_t*)&tss_array[c];
        for (uint32_t i = 0; i < sizeof(tss_t); i++) ptr[i] = 0;

        tss_array[c].iomap_base = sizeof(tss_t);   // no I/O permission bitmap

        // Each CPU's #DF/NMI lands on ITS OWN IST1 stack (never a shared stack).
        tss_array[c].ist1 = ((uint64_t)ist1_stacks[c] + sizeof(ist1_stacks[c])) & ~0xFULL;

        tss_set_gate_cpu(c, tss_gdt_idx[c]);
        kprintf("[TSS] CPU%d TSS at %p, IST1 top=0x%016lx, GDT[%d-%d] sel=0x%02x\n",
                c, &tss_array[c], (unsigned long)tss_array[c].ist1,
                tss_gdt_idx[c], tss_gdt_idx[c] + 1, tss_gdt_idx[c] * 8);
    }

    // Route #DF/NMI IDT gates to IST1 (shared IDT; each CPU's IST1 is its own TSS).
    extern void idt_enable_ist_stacks(void);
    idt_enable_ist_stacks();

    // The BSP (CPU0) loads its own TSS now. The AP loads 0x38 in gdt_ap_load_tss().
    kprintf("[TSS] BSP ltr 0x28...\n");
    tss_flush(0x28);
    kprintf("[TSS] BSP TSS loaded (selector 0x28); CPU1 TSS (0x38) ready for the AP\n");
}

/* AP (CPU1) loads its OWN TSS (selector 0x38). Called as the FIRST C statement in
 * ap_main, BEFORE any sti -- ltr is CPU-local and needs no interrupts. The BSP has
 * already installed gdt[7-8] in tss_init() (which runs long before try_start_cpu1),
 * and CPU1's TSS is a distinct, non-busy descriptor, so this ltr is legal. */
void gdt_ap_load_tss(void) {
    tss_flush(0x38);
}
#else
void tss_init(void) {
    kprintf("[TSS] Initializing Task State Segment...\n");

    // Zero out TSS
    uint8_t* ptr = (uint8_t*)&tss;
    for (uint32_t i = 0; i < sizeof(tss_t); i++) {
        ptr[i] = 0;
    }

    // Set I/O map base to end of TSS (no I/O permission bitmap)
    tss.iomap_base = sizeof(tss_t);

    // Point IST1 at a dedicated, known-good stack and route #DF/NMI to it, so a
    // kernel-stack overflow becomes a clean panic instead of a triple-fault.
    tss.ist1 = ((uint64_t)ist1_stack + sizeof(ist1_stack)) & ~0xFULL;  // 16-aligned top
    extern void idt_enable_ist_stacks(void);
    idt_enable_ist_stacks();

    kprintf("[TSS] TSS at %p, size=%lu, IST1 top=0x%016lx\n",
            &tss, (unsigned long)sizeof(tss_t), (unsigned long)tss.ist1);

    // Setup TSS descriptor in GDT
    tss_set_gate();
    kprintf("[TSS] TSS descriptor installed in GDT[5-6]\n");

    // Verify TSS descriptor bytes
    uint8_t* d = (uint8_t*)&gdt[5];
    kprintf("[TSS] Descriptor bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    kprintf("[TSS]                   %02x %02x %02x %02x %02x %02x %02x %02x\n",
            d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);

    kprintf("[TSS] ltr 0x28...\n");
    tss_flush(0x28);
    kprintf("[TSS] ltr done\n");

    kprintf("[TSS] TSS loaded (selector 0x28)\n");
}
#endif

#ifdef SMP_SCHED
/* Per-CPU syscall kernel-RSP table from syscall.asm ([0]=CPU0, [1]=CPU1). */
extern uint64_t kernel_rsp_save_arr[2];

void tss_set_kernel_stack(uint64_t stack) {
    /* Each CPU writes ONLY its own TSS slot -> no lock needed (single-writer). */
    uint32_t c = cpu_id();
    tss_array[c].rsp0 = stack;
    /* Brick C: keep this CPU's SYSCALL kernel RSP in lockstep with TSS.RSP0 --
     * syscall_entry/syscall_entry_cpu1 load RSP from kernel_rsp_save_arr[cpu]. */
    if (c < 2) kernel_rsp_save_arr[c] = stack;
}

tss_t* tss_get(void) {
    return &tss_array[cpu_id()];
}
#else
void tss_set_kernel_stack(uint64_t stack) {
    tss.rsp0 = stack;
}

tss_t* tss_get(void) {
    return &tss;
}
#endif
