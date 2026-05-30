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

// GDT: 7 entries worth of space (5 standard + 2 for 16-byte TSS descriptor)
// TSS descriptor at entry 5 spans entries 5 and 6
static gdt_entry_t gdt[7];
static gdt_ptr_t gdt_ptr;
static tss_t tss;

// Dedicated 16 KB stack for #DF / NMI delivery (TSS IST1). Separate from the
// per-process kernel stack so a stack overflow that triggers #DF lands on a
// fresh, valid stack rather than re-faulting and triple-faulting.
static uint8_t ist1_stack[8192] __attribute__((aligned(16)));  /* 8 KB: enough for the #DF/NMI panic path */

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

void tss_set_kernel_stack(uint64_t stack) {
    tss.rsp0 = stack;
}

tss_t* tss_get(void) {
    return &tss;
}
