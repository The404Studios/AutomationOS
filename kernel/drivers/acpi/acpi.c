/*
 * kernel/drivers/acpi/acpi.c
 *
 * Self-contained ACPI power driver for a from-scratch x86_64 kernel.
 *
 * Public API (all symbols are prefixed acpi_ to avoid clashes with the
 * separate kernel/acpi/acpi.c driver):
 *
 *   int  acpi_init(void)       -- parse RSDP/RSDT/XSDT/FADT/DSDT, enable ACPI
 *   void acpi_power_off(void)  -- S5 soft-off; does not return on success
 *   void acpi_reboot(void)     -- ACPI reset reg + 8042 fallback; does not return
 *   int  acpi_present(void)    -- 1 if RSDP + FADT were located, else 0
 *
 * Memory access:
 *   Boot identity-maps low physical RAM (0..512 MB at minimum), so all BIOS
 *   firmware tables are reachable via their physical address. We use a local
 *   phys_to_virt macro instead of any external helper to stay self-contained.
 *
 * QEMU notes:
 *   QEMU provides ACPI tables (SeaBIOS/EDK II). No special QEMU flag is needed.
 *   If ACPI parsing is complete the PM1a write will power off the VM. As an
 *   unconditional fallback the well-known QEMU/Bochs magic I/O ports are tried
 *   first (before the ACPI path) so power-off works even without a full DSDT
 *   parse.
 *
 * Compile check (from repo root):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -nostdinc -fno-pic -fno-pie      \
 *       -fno-stack-protector -mno-red-zone -mcmodel=kernel                   \
 *       -DSYSCALL_QUIET -DSCHEDULER_QUIET -DCONTEXT_SWITCH_QUIET             \
 *       -Wno-unused-variable -Wno-unused-function                            \
 *       -Wno-builtin-declaration-mismatch -Wno-implicit-function-declaration \
 *       -Wno-int-conversion -Wno-incompatible-pointer-types                  \
 *       -Ikernel/include -Ikernel/include/compat                             \
 *       -c kernel/drivers/acpi/acpi.c -o /tmp/x.o
 */

#include "../../include/types.h"
#include "../../include/kernel.h"
#include "../../include/io.h"
#include "../../include/string.h"

/* ---------------------------------------------------------------------------
 * Physical-address access
 *   Low physical RAM is identity-mapped by the boot stub, so phys == virt for
 *   addresses below 512 MB. Using a local macro keeps this file independent of
 *   any external phys_to_virt() that may or may not exist.
 * --------------------------------------------------------------------------- */
#define PHYS(addr)  ((void*)(uintptr_t)(addr))

/* ---------------------------------------------------------------------------
 * ACPI table signatures
 * --------------------------------------------------------------------------- */
#define SIG_RSDP   "RSD PTR "   /* 8 bytes, no NUL */
#define SIG_RSDT   "RSDT"
#define SIG_XSDT   "XSDT"
#define SIG_FADT   "FACP"
#define SIG_DSDT   "DSDT"

/* ---------------------------------------------------------------------------
 * I/O delay helper (avoids pulling in timer subsystem)
 * --------------------------------------------------------------------------- */
static void io_delay(uint32_t loops) {
    /* Reading port 0x80 (POST code port) takes ~1 µs on real hardware. */
    for (uint32_t i = 0; i < loops; i++) {
        (void)inb(0x80);
    }
}

/* Park the CPU forever -- used after issuing a power/reset command. */
static void acpi_hang(void) {
    for (;;) {
        cli();
        hlt();
    }
}

/* ---------------------------------------------------------------------------
 * Packed ACPI structure definitions (only what we actually need)
 * --------------------------------------------------------------------------- */

typedef struct {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;   /* 32-bit phys address */
    /* ACPI 2.0+ extension */
    uint32_t length;
    uint64_t xsdt_address;   /* 64-bit phys address */
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) rsdp_t;

typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) tbl_hdr_t;

typedef struct {
    tbl_hdr_t hdr;
    uint32_t  entries[];   /* 32-bit physical addresses */
} __attribute__((packed)) rsdt_t;

typedef struct {
    tbl_hdr_t hdr;
    uint64_t  entries[];   /* 64-bit physical addresses */
} __attribute__((packed)) xsdt_t;

/* Generic Address Structure (GAS) */
typedef struct {
    uint8_t  addr_space;   /* 0=mem, 1=I/O, 2=PCI cfg */
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed)) gas_t;

/* FADT (Fixed ACPI Description Table) -- ACPI 2.0+ layout */
typedef struct {
    tbl_hdr_t hdr;
    uint32_t  firmware_ctrl;
    uint32_t  dsdt;          /* 32-bit phys addr of DSDT */
    uint8_t   _rsvd0;
    uint8_t   pm_profile;
    uint16_t  sci_int;
    uint32_t  smi_cmd;
    uint8_t   acpi_enable;
    uint8_t   acpi_disable;
    uint8_t   s4bios_req;
    uint8_t   pstate_ctrl;
    uint32_t  pm1a_evt_blk;
    uint32_t  pm1b_evt_blk;
    uint32_t  pm1a_cnt_blk;  /* PM1a control register I/O port */
    uint32_t  pm1b_cnt_blk;  /* PM1b control register I/O port (may be 0) */
    uint32_t  pm2_cnt_blk;
    uint32_t  pm_tmr_blk;
    uint32_t  gpe0_blk;
    uint32_t  gpe1_blk;
    uint8_t   pm1_evt_len;
    uint8_t   pm1_cnt_len;
    uint8_t   pm2_cnt_len;
    uint8_t   pm_tmr_len;
    uint8_t   gpe0_blk_len;
    uint8_t   gpe1_blk_len;
    uint8_t   gpe1_base;
    uint8_t   cstate_ctrl;
    uint16_t  worst_c2_lat;
    uint16_t  worst_c3_lat;
    uint16_t  flush_size;
    uint16_t  flush_stride;
    uint8_t   duty_offset;
    uint8_t   duty_width;
    uint8_t   day_alrm;
    uint8_t   mon_alrm;
    uint8_t   century;
    uint16_t  boot_arch;
    uint8_t   _rsvd1;
    uint32_t  flags;
    gas_t     reset_reg;
    uint8_t   reset_value;
    uint16_t  arm_boot_arch;
    uint8_t   fadt_minor;
    uint64_t  x_firmware_ctrl;
    uint64_t  x_dsdt;        /* 64-bit phys addr of DSDT (preferred) */
    gas_t     x_pm1a_evt_blk;
    gas_t     x_pm1b_evt_blk;
    gas_t     x_pm1a_cnt_blk;
    gas_t     x_pm1b_cnt_blk;
    gas_t     x_pm2_cnt_blk;
    gas_t     x_pm_tmr_blk;
    gas_t     x_gpe0_blk;
    gas_t     x_gpe1_blk;
} __attribute__((packed)) fadt_t;

/* FADT flags bit */
#define FADT_FLAG_RESET_REG_SUP  (1u << 10)

/* PM1 control register bits */
#define PM1_SCI_EN         (1u << 0)
#define PM1_SLP_TYP_SHIFT  10
#define PM1_SLP_EN         (1u << 13)

/* ---------------------------------------------------------------------------
 * Module-private state
 * --------------------------------------------------------------------------- */
static struct {
    bool     found_rsdp;
    bool     found_fadt;
    bool     acpi_enabled;

    rsdp_t*  rsdp;
    fadt_t*  fadt;

    /* PM1 control I/O ports (0 = not present) */
    uint16_t pm1a_port;
    uint16_t pm1b_port;

    /* _S5_ sleep types decoded from DSDT */
    uint8_t  slp_typa;  /* SLP_TYPa for S5 (soft-off) */
    uint8_t  slp_typb;  /* SLP_TYPb for S5             */
    bool     have_s5;   /* true if _S5_ was decoded     */
} g_acpi;

/* ---------------------------------------------------------------------------
 * Checksum verification
 * --------------------------------------------------------------------------- */
static bool checksum_ok(const void* p, uint32_t len) {
    const uint8_t* b = (const uint8_t*)p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += b[i];
    return sum == 0;
}

/* ---------------------------------------------------------------------------
 * RSDP search
 *
 * Spec says to scan:
 *   1. First 1 KB of the EBDA (pointer at 0x040E, shifted left 4).
 *   2. BIOS ROM area 0x000E0000 – 0x000FFFFF.
 * Scan on 16-byte boundaries; validate the 20-byte ACPI 1.0 checksum first.
 * --------------------------------------------------------------------------- */
static rsdp_t* find_rsdp(void) {
    /* 1. EBDA */
    uint16_t ebda_seg = *(volatile uint16_t*)PHYS(0x0000040E);
    uint64_t ebda     = (uint64_t)ebda_seg << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000) {
        for (uint64_t a = ebda; a < ebda + 1024; a += 16) {
            rsdp_t* r = (rsdp_t*)PHYS(a);
            if (memcmp(r->signature, SIG_RSDP, 8) == 0 && checksum_ok(r, 20)) {
                kprintf("[ACPI] RSDP found in EBDA at 0x%lx\n", a);
                return r;
            }
        }
    }
    /* 2. BIOS ROM */
    for (uint64_t a = 0x000E0000; a < 0x00100000; a += 16) {
        rsdp_t* r = (rsdp_t*)PHYS(a);
        if (memcmp(r->signature, SIG_RSDP, 8) == 0 && checksum_ok(r, 20)) {
            kprintf("[ACPI] RSDP found in BIOS ROM at 0x%lx\n", a);
            return r;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Table lookup: scan RSDT (32-bit) or XSDT (64-bit) for a 4-byte signature
 * --------------------------------------------------------------------------- */
static tbl_hdr_t* find_table(const char* sig) {
    if (!g_acpi.rsdp) return NULL;

    if (g_acpi.rsdp->revision >= 2 && g_acpi.rsdp->xsdt_address) {
        xsdt_t* x = (xsdt_t*)PHYS(g_acpi.rsdp->xsdt_address);
        if (!checksum_ok(x, x->hdr.length)) return NULL;
        uint32_t n = (x->hdr.length - sizeof(tbl_hdr_t)) / 8;
        for (uint32_t i = 0; i < n; i++) {
            tbl_hdr_t* h = (tbl_hdr_t*)PHYS(x->entries[i]);
            if (memcmp(h->signature, sig, 4) == 0 && checksum_ok(h, h->length))
                return h;
        }
    } else if (g_acpi.rsdp->rsdt_address) {
        rsdt_t* r = (rsdt_t*)PHYS((uint64_t)g_acpi.rsdp->rsdt_address);
        if (!checksum_ok(r, r->hdr.length)) return NULL;
        uint32_t n = (r->hdr.length - sizeof(tbl_hdr_t)) / 4;
        for (uint32_t i = 0; i < n; i++) {
            tbl_hdr_t* h = (tbl_hdr_t*)PHYS((uint64_t)r->entries[i]);
            if (memcmp(h->signature, sig, 4) == 0 && checksum_ok(h, h->length))
                return h;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Minimal AML integer decoder
 *
 * AML encodes small integers in several ways:
 *   0x00        ZeroOp  → 0
 *   0x01        OneOp   → 1
 *   0x0A <b>    BytePrefix → b
 *   0x0B <lo> <hi>  WordPrefix → lo (we only need the low byte for SLP_TYP)
 *   0x0C <b0>..<b3> DWordPrefix → b0
 *   <bare byte>     direct value (some compilers emit bare value for SLP_TYP)
 *
 * Returns number of bytes consumed (1..5), or -1 on error.
 * --------------------------------------------------------------------------- */
static int aml_read_int(const uint8_t* p, const uint8_t* end, uint8_t* out) {
    if (p >= end) return -1;
    switch (*p) {
    case 0x00: *out = 0; return 1;
    case 0x01: *out = 1; return 1;
    case 0x0A:
        if (p + 1 >= end) return -1;
        *out = p[1]; return 2;
    case 0x0B:
        if (p + 2 >= end) return -1;
        *out = p[1]; return 3;
    case 0x0C:
        if (p + 4 >= end) return -1;
        *out = p[1]; return 5;
    default:
        *out = *p; return 1;
    }
}

/* ---------------------------------------------------------------------------
 * Scan DSDT AML for the _S5_ name object and decode SLP_TYPa/b
 *
 * The AML layout for:
 *     Name (_S5_, Package () { SLP_TYPa, SLP_TYPb })
 * is:
 *     NameOp(0x08) '_' 'S' '5' '_'  PackageOp(0x12) PkgLen NumElements
 *     <AML-int: SLP_TYPa> <AML-int: SLP_TYPb>
 *
 * We look for either the 5-byte sequence  08 5F 53 35 5F  followed by 0x12,
 * or directly for the 4-byte name  5F 53 35 5F  followed by 0x12 (some
 * compilers omit the surrounding NameOp context).
 * --------------------------------------------------------------------------- */
static bool dsdt_find_s5(tbl_hdr_t* dsdt, uint8_t* s5a, uint8_t* s5b) {
    if (!dsdt) return false;

    const uint8_t* aml = (const uint8_t*)dsdt + sizeof(tbl_hdr_t);
    const uint8_t* end = (const uint8_t*)dsdt + dsdt->length;

    for (const uint8_t* p = aml; p + 6 < end; p++) {
        /*
         * Match: [0x08] '_' 'S' '5' '_' 0x12
         * The leading NameOp 0x08 is optional -- some tables emit it, some
         * don't. We anchor on the 4-byte name sequence.
         */
        if (!(p[0] == '_' && p[1] == 'S' && p[2] == '5' && p[3] == '_'))
            continue;

        const uint8_t* q = p + 4;
        if (q >= end || *q != 0x12) continue;  /* must be PackageOp */
        q++;  /* skip PackageOp */

        /* Skip PkgLength: high 2 bits = number of additional length bytes. */
        if (q >= end) continue;
        uint8_t extra = (*q >> 6) & 3;
        q += 1 + extra;

        /* NumElements byte */
        if (q >= end) continue;
        uint8_t nelems = *q;
        q++;
        if (nelems < 2) continue;

        uint8_t a = 0, b = 0;
        int n = aml_read_int(q, end, &a);
        if (n < 0) continue;
        q += n;
        (void)aml_read_int(q, end, &b);  /* SLP_TYPb; failure is non-fatal */

        *s5a = a & 0x7;
        *s5b = b & 0x7;
        kprintf("[ACPI] _S5_ decoded: SLP_TYPa=0x%x SLP_TYPb=0x%x\n",
                *s5a, *s5b);
        return true;
    }

    kprintf("[ACPI] WARNING: _S5_ not found in DSDT\n");
    return false;
}

/* ---------------------------------------------------------------------------
 * ACPI enable: signal the firmware to hand SCI ownership to the OS
 * --------------------------------------------------------------------------- */
static void acpi_enable_mode(void) {
    if (!g_acpi.fadt) return;
    if (!g_acpi.pm1a_port) return;

    /* Already in ACPI mode? */
    if (inw(g_acpi.pm1a_port) & PM1_SCI_EN) {
        kprintf("[ACPI] SCI_EN already set; ACPI mode active\n");
        g_acpi.acpi_enabled = true;
        return;
    }

    /* Ask firmware to switch via SMI_CMD / ACPI_ENABLE */
    if (g_acpi.fadt->smi_cmd && g_acpi.fadt->acpi_enable) {
        outb((uint16_t)g_acpi.fadt->smi_cmd, g_acpi.fadt->acpi_enable);

        /* Poll SCI_EN with a coarse timeout (~300 × 10 ms I/O delay) */
        for (int i = 0; i < 300; i++) {
            if (inw(g_acpi.pm1a_port) & PM1_SCI_EN) {
                kprintf("[ACPI] ACPI mode enabled\n");
                g_acpi.acpi_enabled = true;
                return;
            }
            io_delay(10000);
        }
        kprintf("[ACPI] WARNING: timeout waiting for ACPI mode\n");
    } else {
        /* Hardware-reduced ACPI or firmware already in ACPI mode */
        kprintf("[ACPI] No SMI_CMD; assuming ACPI mode\n");
        g_acpi.acpi_enabled = true;
    }
}

/* ===========================================================================
 * PUBLIC API
 * =========================================================================== */

/*
 * acpi_init() -- parse ACPI tables, locate FADT + DSDT/_S5_, enable ACPI mode.
 *
 * Call from kernel_main() after pci_init() (or earlier -- ACPI is independent
 * of PCI initialisation). Safe to call before the heap is available.
 *
 * Returns  0 on success (RSDP + FADT located and parsed).
 * Returns -1 on failure (ACPI not available; poweroff/reboot still work via
 *            fallback ports).
 */
int acpi_init(void) {
    kprintf("[ACPI] Initialising ACPI power driver...\n");

    /* Zero state */
    memset(&g_acpi, 0, sizeof(g_acpi));

    /* ---- Step 1: Locate RSDP -------------------------------------------- */
    g_acpi.rsdp = find_rsdp();
    if (!g_acpi.rsdp) {
        kprintf("[ACPI] ERROR: RSDP not found -- ACPI unavailable\n");
        return -1;
    }
    g_acpi.found_rsdp = true;
    kprintf("[ACPI] RSDP revision %u; OEM: %.6s\n",
            g_acpi.rsdp->revision, g_acpi.rsdp->oem_id);

    /* ---- Step 2: Locate FADT (signature "FACP") -------------------------- */
    fadt_t* fadt = (fadt_t*)find_table(SIG_FADT);
    if (!fadt) {
        kprintf("[ACPI] ERROR: FADT (FACP) not found\n");
        return -1;
    }
    g_acpi.fadt       = fadt;
    g_acpi.found_fadt = true;

    /* ---- Step 3: Extract PM1a/PM1b control ports ------------------------- */
    /*
     * FADT revision >= 2: prefer the 64-bit Extended GAS fields if non-zero
     * and in I/O address space (addr_space == 1). Fall back to the 32-bit
     * legacy fields.
     */
    if (fadt->hdr.revision >= 2
            && fadt->x_pm1a_cnt_blk.addr_space == 1
            && fadt->x_pm1a_cnt_blk.address) {
        g_acpi.pm1a_port = (uint16_t)fadt->x_pm1a_cnt_blk.address;
    } else if (fadt->pm1a_cnt_blk) {
        g_acpi.pm1a_port = (uint16_t)fadt->pm1a_cnt_blk;
    }

    if (fadt->hdr.revision >= 2
            && fadt->x_pm1b_cnt_blk.addr_space == 1
            && fadt->x_pm1b_cnt_blk.address) {
        g_acpi.pm1b_port = (uint16_t)fadt->x_pm1b_cnt_blk.address;
    } else if (fadt->pm1b_cnt_blk) {
        g_acpi.pm1b_port = (uint16_t)fadt->pm1b_cnt_blk;
    }

    kprintf("[ACPI] PM1a_CNT_BLK=0x%x PM1b_CNT_BLK=0x%x\n",
            g_acpi.pm1a_port, g_acpi.pm1b_port);

    if (!g_acpi.pm1a_port) {
        kprintf("[ACPI] WARNING: PM1a control port is 0 -- S5 write may fail\n");
    }

    /* ---- Step 4: Locate DSDT and decode _S5_ ----------------------------- */
    tbl_hdr_t* dsdt = NULL;

    /* Prefer X_DSDT (64-bit) over DSDT (32-bit) when revision >= 2 */
    if (fadt->hdr.revision >= 2 && fadt->x_dsdt) {
        dsdt = (tbl_hdr_t*)PHYS(fadt->x_dsdt);
    } else if (fadt->dsdt) {
        dsdt = (tbl_hdr_t*)PHYS((uint64_t)fadt->dsdt);
    }

    if (dsdt && memcmp(dsdt->signature, SIG_DSDT, 4) == 0
             && checksum_ok(dsdt, dsdt->length)) {
        kprintf("[ACPI] DSDT at 0x%lx, length %u bytes\n",
                (uint64_t)(uintptr_t)dsdt, dsdt->length);
        g_acpi.have_s5 = dsdt_find_s5(dsdt, &g_acpi.slp_typa, &g_acpi.slp_typb);
    } else {
        kprintf("[ACPI] WARNING: DSDT not valid/found; using fallback SLP_TYP\n");
    }

    if (!g_acpi.have_s5) {
        /*
         * Fallback SLP_TYP values. QEMU SeaBIOS encodes S5 as SLP_TYPa=0,
         * SLP_TYPb=0. Many real BIOSes also use 0x05. We default to 0 (QEMU
         * path) to maximise the chance that the PM1a write actually powers off.
         */
        g_acpi.slp_typa = 0x00;
        g_acpi.slp_typb = 0x00;
        kprintf("[ACPI] Using fallback SLP_TYPa/b = 0x00\n");
    }

    /* ---- Step 5: Enable ACPI mode ---------------------------------------- */
    acpi_enable_mode();

    kprintf("[ACPI] Init complete: PM1a=0x%x PM1b=0x%x S5a=0x%x S5b=0x%x\n",
            g_acpi.pm1a_port, g_acpi.pm1b_port,
            g_acpi.slp_typa, g_acpi.slp_typb);

    return 0;
}

/*
 * acpi_present() -- 1 if RSDP and FADT were located, else 0.
 */
int acpi_present(void) {
    return (g_acpi.found_rsdp && g_acpi.found_fadt) ? 1 : 0;
}

/*
 * acpi_power_off() -- Soft-off (ACPI S5 / system shutdown).
 *
 * Three-layer strategy so QEMU and real hardware both work:
 *
 *   Layer 0 (fallback, always tried first because it is instantaneous on QEMU):
 *     Write the well-known QEMU/Bochs magic poweroff port values.
 *     On QEMU these immediately exit the emulator; on real hardware they are
 *     no-ops (the ports are unused), so execution continues.
 *
 *   Layer 1 (ACPI S5 via PM1a/PM1b control registers):
 *     Disable interrupts, write (SLP_TYPa<<10)|SLP_EN to PM1a_CNT.
 *     If PM1b is present, write (SLP_TYPb<<10)|SLP_EN there too.
 *
 *   Layer 2 (panic hang):
 *     If we somehow fall through, park the CPU forever.
 *
 * This function does not return on success.
 */
void acpi_power_off(void) {
    kprintf("[ACPI] Powering off...\n");

    /* ---- Layer 0: QEMU / Bochs / VirtualBox fallback I/O ports ----------- */
    /*
     * QEMU with SeaBIOS / pc machine: port 0x604, value 0x2000
     * QEMU with ICH9 / q35 machine:  same (remapped by PIIX4 ACPI)
     * Bochs:                          port 0xB004, value 0x2000
     * VirtualBox (some versions):     port 0x4004, value 0x3400
     *
     * Writing these on QEMU causes an immediate VM exit/poweroff.
     * On real hardware the ports are either unused or map to PCI devices
     * that ignore unrecognised writes, so we fall through harmlessly.
     */
    outw(0x604,  0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);

    /* ---- Layer 1: ACPI S5 write ------------------------------------------ */
    if (g_acpi.pm1a_port) {
        cli();  /* prevent any interrupt from delaying the write */

        uint16_t pm1a = (uint16_t)(((uint16_t)g_acpi.slp_typa << PM1_SLP_TYP_SHIFT)
                                   | PM1_SLP_EN);
        outw(g_acpi.pm1a_port, pm1a);

        if (g_acpi.pm1b_port) {
            uint16_t pm1b = (uint16_t)(((uint16_t)g_acpi.slp_typb << PM1_SLP_TYP_SHIFT)
                                       | PM1_SLP_EN);
            outw(g_acpi.pm1b_port, pm1b);
        }

        /* Give the hardware a moment to act */
        io_delay(100000);
    } else {
        kprintf("[ACPI] No PM1a port; ACPI S5 write skipped\n");
    }

    /* ---- Layer 2: Give up, park the CPU ---------------------------------- */
    kprintf("[ACPI] Power-off failed; halting CPU\n");
    acpi_hang();
}

/*
 * acpi_reboot() -- system reset.
 *
 * Three-layer strategy:
 *
 *   Layer 1: ACPI reset register (FADT offset 116, if FADT_FLAG_RESET_REG_SUP
 *            is set). Supports I/O space and memory-mapped writes.
 *
 *   Layer 2: 8042 keyboard-controller pulse-reset (port 0x64, command 0xFE).
 *            Works on virtually every PC with a legacy or emulated KBC.
 *            Drains the input buffer first so the command is accepted.
 *
 *   Layer 3: Triple-fault by loading an all-zero IDT and issuing a software
 *            interrupt. The resulting #GP/#DF causes an immediate CPU reset on
 *            real hardware and QEMU.
 *
 * This function does not return on success.
 */
void acpi_reboot(void) {
    kprintf("[ACPI] Rebooting...\n");

    cli();

    /* ---- Layer 1: ACPI reset register ------------------------------------ */
    if (g_acpi.fadt && (g_acpi.fadt->flags & FADT_FLAG_RESET_REG_SUP)) {
        gas_t*  rreg = &g_acpi.fadt->reset_reg;
        uint8_t rval =  g_acpi.fadt->reset_value;

        if (rreg->address) {
            if (rreg->addr_space == 1) {
                /* I/O space */
                outb((uint16_t)rreg->address, rval);
            } else if (rreg->addr_space == 0) {
                /* Memory-mapped (identity-mapped low physical memory) */
                *(volatile uint8_t*)PHYS(rreg->address) = rval;
            }
            /* PCI config space (addr_space == 2) is uncommon; skip */
            io_delay(100000);
        }
    }

    /* ---- Layer 2: 8042 keyboard controller reset ------------------------- */
    kprintf("[ACPI] Trying 8042 reset (port 0x64 <- 0xFE)...\n");

    /* Drain the KBC input buffer (max ~1000 polls) */
    for (int i = 0; i < 1000; i++) {
        if (!(inb(0x64) & 0x02)) break;  /* input buffer empty */
        io_delay(100);
    }
    outb(0x64, 0xFE);   /* pulse CPU reset line */
    io_delay(100000);

    /* ---- Layer 3: Triple-fault via null IDT ------------------------------ */
    kprintf("[ACPI] Attempting triple-fault reset...\n");
    {
        /* Load an IDT with a zero limit so any interrupt causes #DF -> reset */
        struct { uint16_t limit; uint64_t base; } __attribute__((packed)) zero_idt = {0, 0};
        asm volatile("lidt %0" : : "m"(zero_idt));
        asm volatile("int $3");   /* cause a #BP -> #DF -> reset */
    }

    /* Should never reach here */
    acpi_hang();
}
