#ifndef X86_64_H
#define X86_64_H

#include "types.h"

// Port I/O
static inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// CPU control
static inline void cli(void) {
    asm volatile("cli");
}

static inline void sti(void) {
    asm volatile("sti");
}

static inline void hlt(void) {
    asm volatile("hlt");
}

static inline void invlpg(void* addr) {
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

// INVPCID types
#define INVPCID_TYPE_INDIVIDUAL        0
#define INVPCID_TYPE_SINGLE_CONTEXT    1
#define INVPCID_TYPE_ALL_CONTEXTS      2
#define INVPCID_TYPE_ALL_INCLUDING_GLOBAL 3

// INVPCID instruction wrapper
static inline void invpcid(uint64_t type, uint64_t pcid, uint64_t addr) {
    struct {
        uint64_t pcid;
        uint64_t addr;
    } desc = { pcid, addr };
    asm volatile("invpcid %0, %1" :: "m"(desc), "r"(type) : "memory");
}

// Flush single page with PCID
static inline void invpcid_flush_single(uint64_t pcid, uint64_t addr) {
    invpcid(INVPCID_TYPE_INDIVIDUAL, pcid, addr);
}

// Flush all mappings for a PCID
static inline void invpcid_flush_all(uint64_t pcid) {
    invpcid(INVPCID_TYPE_SINGLE_CONTEXT, pcid, 0);
}

// Flush all mappings including global pages
static inline void invpcid_flush_all_including_global(void) {
    invpcid(INVPCID_TYPE_ALL_INCLUDING_GLOBAL, 0, 0);
}

// MSR definitions for SYSCALL/SYSRET
#define MSR_STAR  0xC0000081  // Syscall segment selectors
#define MSR_LSTAR 0xC0000082  // Syscall entry point
#define MSR_FMASK 0xC0000084  // RFLAGS mask for syscall

// MSR for Extended Feature Enable Register (EFER)
#define MSR_EFER  0xC0000080  // Extended Feature Enable Register
#define EFER_NXE  (1ULL << 11) // No-Execute Enable bit (inert until wrmsr(EFER) is called)

// Read/write MSRs
static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    asm volatile("wrmsr" :: "a"(low), "d"(high), "c"(msr));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

// Read/write control registers
static inline uint64_t read_cr0(void) {
    uint64_t val;
    asm volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline void write_cr0(uint64_t val) {
    asm volatile("mov %0, %%cr0" : : "r"(val));
}

static inline uint64_t read_cr3(void) {
    uint64_t val;
    asm volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(uint64_t val) {
    asm volatile("mov %0, %%cr3" : : "r"(val));
}

// CR3 bits
#define CR3_NO_FLUSH (1ULL << 63)  // Bit 63: preserve TLB entries on CR3 load

static inline uint64_t read_cr4(void) {
    uint64_t val;
    asm volatile("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline void write_cr4(uint64_t val) {
    asm volatile("mov %0, %%cr4" : : "r"(val) : "memory");
}

// CR4 bits
#define CR4_PCIDE (1ULL << 17)
#define CR4_SMEP  (1ULL << 20)  // Supervisor Mode Execution Prevention
#define CR4_SMAP  (1ULL << 21)  // Supervisor Mode Access Prevention

/*
 * STAC / CLAC — Supervisor-mode user-page access brackets.
 *
 * When SMAP is enabled (CR4.SMAP=1), any supervisor-mode access to a page
 * marked PAGE_USER raises #PF *unless* RFLAGS.AC is set.  STAC sets AC
 * (opens the window), CLAC clears it (closes).  Every copy_from/to_user
 * and every raw user-memory access (futex atomic load) must be wrapped in
 * a stac() ... clac() bracket so the kernel can reach the user page.
 *
 * GUARD: STAC/CLAC are Haswell+ instructions (opcode 0F 01 CB/CA).  On
 * Westmere/Arrandale (T410) they are UNDEFINED and cause #UD, freezing
 * the kernel.  We gate on a global flag set by paging_init() only when
 * SMAP is actually enabled.  Without SMAP the instructions are never
 * needed (the CPU ignores RFLAGS.AC for page-fault checks).
 */
extern bool cpu_smap_active;

static inline void stac(void) {
    if (cpu_smap_active)
        __asm__ volatile("stac" ::: "cc", "memory");
}
static inline void clac(void) {
    if (cpu_smap_active)
        __asm__ volatile("clac" ::: "cc", "memory");
}

// CPUID feature detection for INVPCID
// GUARD: Westmere/Arrandale (T410) has max leaf 0xB but does not support
// INVPCID (Haswell+).  Older CPUs with max leaf < 7 would return the
// highest-supported-leaf result (garbage).  Always check max leaf first.
static inline bool cpu_has_invpcid(void) {
    uint32_t max_leaf;
    asm volatile("cpuid" : "=a"(max_leaf) : "a"(0) : "ebx", "ecx", "edx");
    if (max_leaf < 7) return false;
    uint32_t a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    return (b >> 10) & 1;
}

// Critical section helpers for interrupt disabling
// NOTE: save_flags_cli() and restore_flags() are defined in spinlock.h
// to avoid duplicate definitions. Include spinlock.h if you need them.

// Simple IRQ handler (legacy - for basic interrupt registration)
typedef void (*simple_irq_handler_t)(void);
void irq_register_handler(uint8_t irq, simple_irq_handler_t handler);

#endif
