/*
 * lspci.c -- PCI device lister (freestanding, ring 3).
 * =====================================================
 *
 * Lists all PCI devices enumerated by the kernel via SYS_PCI_LIST (92).
 * Displays bus/device/function, vendor:device IDs, class name, and a
 * known-device name when available.
 *
 * NO libc, NO stdio, NO malloc, NO standard headers.
 * Inline syscalls + fixed buffers + own helpers only.
 *
 * Usage:
 *   lspci              -- list all PCI devices
 *   lspci (no args)    -- same (self-test mode prints PASS/FAIL)
 *
 * Build (flags DIRECT on cmdline -- NEVER via shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/lspci/lspci.c -o /tmp/lspci.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/crt0.o /tmp/lspci.o -o /tmp/lspci.elf
 *   objdump -d /tmp/lspci.o | grep fs:0x28   # MUST be empty
 */

/* ---- syscall numbers (must match kernel/include/syscall.h) ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_PCI_LIST     92

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

/* ---- pci_info_t: must match kernel/include/pci.h pci_info_t ---- */
typedef struct {
    u16  vendor_id;
    u16  device_id;
    u8   bus;
    u8   device;
    u8   function;
    u8   class_code;
    u8   subclass;
    u8   prog_if;
    u8   revision_id;
    u8   interrupt_line;
    u8   interrupt_pin;
    u8   _pad[3];
} pci_info_t;

/* ---- 6-argument inline syscall ---- */
static inline long sc(long n, long a1, long a2, long a3,
                      long a4, long a5, long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny freestanding helpers ---- */

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0);
}

static void print_ch(char c)
{
    sc(SYS_WRITE, 1, (long)&c, 1, 0, 0, 0);
}

static void print_dec(unsigned long v)
{
    char buf[20];
    int  i = 0;
    if (v == 0) { print_ch('0'); return; }
    do {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (i > 0) print_ch(buf[--i]);
}

static void print_hex4(u16 v)
{
    const char *h = "0123456789abcdef";
    char b[4] = {
        h[(v >> 12) & 0xF],
        h[(v >> 8)  & 0xF],
        h[(v >> 4)  & 0xF],
        h[ v        & 0xF]
    };
    sc(SYS_WRITE, 1, (long)b, 4, 0, 0, 0);
}

static void print_hex2(u8 v)
{
    const char *h = "0123456789abcdef";
    char b[2] = { h[(v >> 4) & 0xF], h[v & 0xF] };
    sc(SYS_WRITE, 1, (long)b, 2, 0, 0, 0);
}

/* print_padded: print a string left-justified in a field of `width` chars. */
static void print_padded(const char *s, int width)
{
    int len = 0;
    while (s[len]) len++;
    print(s);
    for (int i = len; i < width; i++)
        print_ch(' ');
}

/* ---- known device database (mirrors kernel/drivers/pci.c) ---- */

typedef struct {
    u16         vendor_id;
    u16         device_id;
    const char *name;
} pci_id_entry_t;

static const pci_id_entry_t known_devices[] = {
    /* Intel Core i5 / Ironlake (T410 northbridge) */
    { 0x8086, 0x0044, "Intel Core i5 Host Bridge (Ironlake)" },
    { 0x8086, 0x0046, "Intel HD Graphics (Ironlake)" },
    { 0x8086, 0x0040, "Intel Core i5 DRAM Controller" },
    { 0x8086, 0x0042, "Intel HD Graphics (Ironlake Desktop)" },

    /* Intel 5 Series / ICH9M (T410 southbridge / PCH) */
    { 0x8086, 0x3B09, "Intel ICH9M-E LPC Interface Controller" },
    { 0x8086, 0x3B0B, "Intel ICH9M LPC Interface Controller" },
    { 0x8086, 0x3B22, "Intel 5 Series SMBus Controller" },
    { 0x8086, 0x3B30, "Intel 5 Series USB UHCI #1" },
    { 0x8086, 0x3B31, "Intel 5 Series USB UHCI #2" },
    { 0x8086, 0x3B32, "Intel 5 Series USB UHCI #3" },
    { 0x8086, 0x3B34, "Intel 5 Series USB2 EHCI #1" },
    { 0x8086, 0x3B36, "Intel 5 Series USB2 EHCI #2" },
    { 0x8086, 0x3B42, "Intel 5 Series SATA AHCI" },
    { 0x8086, 0x3B56, "Intel 5 Series HD Audio" },
    { 0x8086, 0x3B64, "Intel 5 Series Thermal Subsystem" },
    { 0x8086, 0x3B3C, "Intel 5 Series MEI Controller #1" },
    { 0x8086, 0x3B4C, "Intel 5 Series PCIe Root Port 1" },
    { 0x8086, 0x3B4E, "Intel 5 Series PCIe Root Port 2" },
    { 0x8086, 0x3B50, "Intel 5 Series PCIe Root Port 3" },

    /* Intel Ethernet (T410 + QEMU) */
    { 0x8086, 0x10EA, "Intel 82577LM Gigabit Ethernet" },
    { 0x8086, 0x100E, "Intel 82540EM Gigabit Ethernet (QEMU)" },
    { 0x8086, 0x100F, "Intel 82545EM Gigabit Ethernet" },
    { 0x8086, 0x10D3, "Intel 82574L Gigabit Ethernet" },

    /* NVIDIA NVS 3100M (T410 discrete GPU) */
    { 0x10DE, 0x0A6C, "NVIDIA NVS 3100M (GT218)" },

    /* Ricoh card reader (T410) */
    { 0x1180, 0x0592, "Ricoh R5C592 SD/MMC Card Reader" },
    { 0x1180, 0x0843, "Ricoh R5C843 FireWire" },

    /* QEMU virtual devices */
    { 0x8086, 0x1237, "Intel 440FX Host Bridge (QEMU)" },
    { 0x8086, 0x7000, "Intel PIIX3 ISA Bridge (QEMU)" },
    { 0x8086, 0x7010, "Intel PIIX3 IDE (QEMU)" },
    { 0x8086, 0x7020, "Intel PIIX3 USB UHCI (QEMU)" },
    { 0x8086, 0x7113, "Intel PIIX4 ACPI/Power Mgmt (QEMU)" },
    { 0x8086, 0x2922, "Intel ICH9 AHCI (QEMU)" },
    { 0x8086, 0x2668, "Intel ICH6 HD Audio (QEMU)" },
    { 0x8086, 0x29C0, "Intel Q35 Host Bridge (QEMU)" },
    { 0x8086, 0x2918, "Intel ICH9 LPC Interface (QEMU)" },
    { 0x1234, 0x1111, "QEMU stdvga (Bochs VBE)" },
    { 0x1AF4, 0x1000, "VirtIO Network" },
    { 0x1AF4, 0x1001, "VirtIO Block" },
    { 0x1AF4, 0x1002, "VirtIO Balloon" },
    { 0x1AF4, 0x1041, "VirtIO Network (modern)" },
    { 0x1AF4, 0x1042, "VirtIO Block (modern)" },
    { 0x1AF4, 0x1050, "VirtIO GPU" },
    { 0x10EC, 0x8139, "Realtek RTL8139 Fast Ethernet" },

    /* Intel Wireless */
    { 0x8086, 0x0085, "Intel Centrino Advanced-N 6205" },
    { 0x8086, 0x4232, "Intel WiFi Link 5100 AGN" },

    /* Sentinel */
    { 0, 0, (const char *)0 }
};

static const char *lookup_device_name(u16 vendor, u16 device)
{
    for (int i = 0; known_devices[i].name != (const char *)0; i++) {
        if (known_devices[i].vendor_id == vendor &&
            known_devices[i].device_id == device) {
            return known_devices[i].name;
        }
    }
    return (const char *)0;
}

/* ---- PCI class-code to human-readable name ---- */

static const char *class_name(u8 cls, u8 sub)
{
    switch (cls) {
    case 0x00: return "Unclassified";
    case 0x01:
        switch (sub) {
        case 0x00: return "SCSI Bus Controller";
        case 0x01: return "IDE Controller";
        case 0x05: return "ATA Controller";
        case 0x06: return "SATA Controller";
        case 0x08: return "NVMe Controller";
        default:   return "Mass Storage Controller";
        }
    case 0x02:
        switch (sub) {
        case 0x00: return "Ethernet Controller";
        case 0x80: return "Other Network Controller";
        default:   return "Network Controller";
        }
    case 0x03:
        switch (sub) {
        case 0x00: return "VGA Controller";
        case 0x02: return "3D Controller";
        default:   return "Display Controller";
        }
    case 0x04:
        switch (sub) {
        case 0x03: return "HD Audio Controller";
        default:   return "Multimedia Controller";
        }
    case 0x05: return "Memory Controller";
    case 0x06:
        switch (sub) {
        case 0x00: return "Host Bridge";
        case 0x01: return "ISA Bridge";
        case 0x04: return "PCI-to-PCI Bridge";
        default:   return "Bridge Device";
        }
    case 0x07: return "Communication Controller";
    case 0x08: return "System Peripheral";
    case 0x09: return "Input Device";
    case 0x0C:
        switch (sub) {
        case 0x00: return "FireWire (IEEE 1394)";
        case 0x03: return "USB Controller";
        case 0x05: return "SMBus";
        default:   return "Serial Bus Controller";
        }
    case 0x0D: return "Wireless Controller";
    case 0xFF: return "Vendor-specific";
    default:   return "Unknown";
    }
}

/* ---- maximum PCI devices to query ---- */
#define MAX_PCI_DEVICES 64

/* ---- entry point ---- */

int main(int argc, char **argv)
{
    (void)argv;

    pci_info_t devs[MAX_PCI_DEVICES];

    /* Zero the buffer. */
    u8 *p = (u8 *)devs;
    for (unsigned long i = 0; i < sizeof(devs); i++) p[i] = 0;

    long n = sc(SYS_PCI_LIST, (long)devs, MAX_PCI_DEVICES, 0, 0, 0, 0);

    /* Self-test mode (no arguments): just check the syscall works. */
    if (argc <= 1) {
        if (n < 0) {
            print("LSPCI SELFTEST: FAIL (syscall returned ");
            print_dec((unsigned long)(-n));
            print(")\n");
            return 1;
        }
        if (n == 0) {
            print("LSPCI SELFTEST: PASS (0 devices)\n");
            return 0;
        }
        /* Validate first entry has a non-zero vendor. */
        if (devs[0].vendor_id == 0 || devs[0].vendor_id == 0xFFFF) {
            print("LSPCI SELFTEST: FAIL (bad vendor in slot 0)\n");
            return 1;
        }
        print("LSPCI SELFTEST: PASS\n");
        return 0;
    }

    /* Live mode: display PCI devices. */
    if (n < 0) {
        print("lspci: SYS_PCI_LIST failed (rc=");
        print_dec((unsigned long)(-n));
        print(")\n");
        return 1;
    }
    if (n == 0) {
        print("lspci: no PCI devices found\n");
        return 0;
    }

    /* Header. */
    print("BDF         Vendor Device  Class  Description\n");
    print("----------  ------ ------  -----  -----------\n");

    for (long i = 0; i < n; i++) {
        const pci_info_t *d = &devs[i];

        /* BDF: xx:xx.x */
        print_hex2(d->bus);
        print_ch(':');
        print_hex2(d->device);
        print_ch('.');
        print_dec(d->function);
        /* pad to 12 chars */
        {
            /* "xx:xx.f" is 7 chars min, 8 if func>=10 (can't happen, max 7) */
            int bdf_len = 7;
            for (int pad = bdf_len; pad < 12; pad++) print_ch(' ');
        }

        /* Vendor:Device IDs */
        print_hex4(d->vendor_id);
        print("   ");
        print_hex4(d->device_id);
        print("  ");

        /* Class code xx:xx */
        print_hex2(d->class_code);
        print_ch(':');
        print_hex2(d->subclass);
        print("  ");

        /* Human-readable name: prefer known device, fall back to class. */
        const char *name = lookup_device_name(d->vendor_id, d->device_id);
        if (name) {
            print(name);
        } else {
            print(class_name(d->class_code, d->subclass));
        }

        print_ch('\n');
    }

    /* Summary. */
    print_dec((unsigned long)n);
    print(" device(s)\n");

    return 0;
}
