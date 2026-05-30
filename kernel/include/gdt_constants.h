#ifndef GDT_CONSTANTS_H
#define GDT_CONSTANTS_H

/*
 * GDT (Global Descriptor Table) Constants
 * =======================================
 *
 * Defines access flags and granularity bits for x86_64 GDT entries.
 */

/* GDT Access Byte Flags */
#define GDT_ACCESS_PRESENT      0x80    /* Segment present bit */
#define GDT_ACCESS_PRIV_RING0   0x00    /* Privilege ring 0 (kernel) */
#define GDT_ACCESS_PRIV_RING3   0x60    /* Privilege ring 3 (user) */
#define GDT_ACCESS_S_BIT        0x10    /* S=1: code/data segment (not system) */
#define GDT_ACCESS_EXECUTABLE   0x08    /* Code segment (executable) */
#define GDT_ACCESS_RW           0x02    /* Readable (code) / Writable (data) */
#define GDT_ACCESS_ACCESSED     0x01    /* Accessed bit */

/* Combined Access Flags */
#define GDT_ACCESS_KERNEL_CODE  (GDT_ACCESS_PRESENT | GDT_ACCESS_PRIV_RING0 | \
                                 GDT_ACCESS_S_BIT | GDT_ACCESS_EXECUTABLE | GDT_ACCESS_RW)  /* 0x9A */

#define GDT_ACCESS_KERNEL_DATA  (GDT_ACCESS_PRESENT | GDT_ACCESS_PRIV_RING0 | \
                                 GDT_ACCESS_S_BIT | GDT_ACCESS_RW)  /* 0x92 */

#define GDT_ACCESS_USER_CODE    (GDT_ACCESS_PRESENT | GDT_ACCESS_PRIV_RING3 | \
                                 GDT_ACCESS_S_BIT | GDT_ACCESS_EXECUTABLE | GDT_ACCESS_RW)  /* 0xFA */

#define GDT_ACCESS_USER_DATA    (GDT_ACCESS_PRESENT | GDT_ACCESS_PRIV_RING3 | \
                                 GDT_ACCESS_S_BIT | GDT_ACCESS_RW)  /* 0xF2 */

/* GDT Granularity Flags */
#define GDT_GRAN_4K             0x80    /* 4KB page granularity */
#define GDT_GRAN_BYTE           0x00    /* Byte granularity */
#define GDT_GRAN_64BIT          0x20    /* 64-bit code segment (L-bit) */
#define GDT_GRAN_32BIT          0x40    /* 32-bit protected mode (D/B-bit) */

/* Combined Granularity for 64-bit segments */
#define GDT_GRAN_64BIT_CODE     (GDT_GRAN_4K | GDT_GRAN_64BIT)  /* 0xA0 - for code segments */
#define GDT_GRAN_64BIT_DATA     (GDT_GRAN_4K | GDT_GRAN_32BIT)  /* 0xC0 - for data segments */

/* GDT Segment Limits */
#define GDT_LIMIT_FULL          0xFFFFFFFF  /* Full 4GB addressing */

/*
 * GDT Segment Selectors (x86_64 SYSRET-compatible layout)
 * ========================================================
 *
 * SYSRET requires: CS = STAR[63:48]+16, SS = STAR[63:48]+8
 * So user data must come BEFORE user code in the GDT.
 *
 * GDT Layout:
 * Entry 0 (0x00): Null segment
 * Entry 1 (0x08): Kernel code segment (DPL=0)
 * Entry 2 (0x10): Kernel data segment (DPL=0)
 * Entry 3 (0x18): User data segment (DPL=3) ← SYSRET SS = STAR+8
 * Entry 4 (0x20): User code segment (DPL=3) ← SYSRET CS = STAR+16
 * Entry 5 (0x28): Task State Segment (TSS, 16 bytes)
 */

#define KERNEL_CS_BASE      0x08
#define KERNEL_DS_BASE      0x10
#define USER_DS_BASE        0x18    /* User data BEFORE code (SYSRET requirement) */
#define USER_CS_BASE        0x20    /* User code AFTER data */
#define TSS_SELECTOR        0x28

#define KERNEL_CS           0x08
#define KERNEL_DS           0x10
#define USER_CS             (USER_CS_BASE | 3)  /* 0x23 */
#define USER_DS             (USER_DS_BASE | 3)  /* 0x1B */

/* RPL values */
#define RPL_RING0           0       /* Kernel privilege */
#define RPL_RING3           3       /* User privilege */

#endif
