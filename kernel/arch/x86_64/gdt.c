#include "../../include/x86_64.h"
#include "../../include/kernel.h"

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} PACKED gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} PACKED gdt_ptr_t;

static gdt_entry_t gdt[5];
static gdt_ptr_t gdt_ptr;

extern void gdt_flush(uint64_t gdt_ptr);

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_mid = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;

    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access = access;
}

void gdt_init(void) {
    kprintf("[GDT] Initializing Global Descriptor Table...\n");

    gdt_ptr.limit = (sizeof(gdt_entry_t) * 5) - 1;
    gdt_ptr.base = (uint64_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);                // Null segment
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xA0); // Kernel code (64-bit)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xA0); // Kernel data
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xA0); // User code (64-bit)
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xA0); // User data

    gdt_flush((uint64_t)&gdt_ptr);

    kprintf("[GDT] GDT loaded\n");
}
