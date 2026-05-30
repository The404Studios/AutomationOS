/*
 * NVIDIA GPU driver -- detection + firmware-framebuffer foundation
 * ================================================================
 *
 * TARGET: Lenovo ThinkPad T410 discrete GPU -- NVIDIA NVS 3100M,
 *         PCI 0x10DE:0x0A6C, GT218 core (Tesla / NV50 generation).
 *
 * WHAT THIS DRIVER DOES (honest, conservative scope)
 * --------------------------------------------------
 *   1. Scans the (already-enumerated) PCI bus for an NVIDIA display
 *      controller: vendor 0x10DE, PCI class 0x03 (display). It recognises the
 *      NVS 3100M (device 0x0A6C) specifically and the broader GT218/NV50
 *      Tesla family generically, logging vendor/device/revision.
 *
 *   2. Maps BAR0 -- the MMIO register aperture (~16 MiB on Tesla parts) -- and
 *      records BAR1, the VRAM/framebuffer aperture. It then performs ONE
 *      read-only access: PMC_BOOT_0 at MMIO offset 0x000000, whose value
 *      encodes the chip id / architecture (GT218 reports an NVAx id). Reading
 *      it back proves the register aperture is live and decodes the chip.
 *
 *   3. ADOPTS the firmware-configured linear framebuffer. The bootloader hands
 *      the kernel a VESA/VBIOS linear framebuffer (the very surface the boot
 *      splash already renders to). This driver captures that geometry and
 *      exposes a clean accessor. IT DOES NOT PROGRAM THE DISPLAY. Native
 *      mode-setting on Tesla (CRTC + PLL + DCB) requires on-hardware iteration
 *      that, done blind, bricks the panel to black -- so we deliberately leave
 *      the screen mode exactly as firmware set it.
 *
 *   4. Scaffolds (present but NOT called, clearly TODO) the future native path:
 *      VBIOS ROM read (PROM / shadow), DCB parse, CRTC/PLL programming. All
 *      disabled.
 *
 * HARD SAFETY RULES (enforced here)
 * ---------------------------------
 *   - NO writes to GPU MMIO registers during detection. Probing is read-only.
 *   - NO display-mode changes of any kind.
 *   - NO unbounded loops; the PCI scan and the (unused) ROM-shadow scaffold are
 *     all bounded by fixed limits.
 *   - On a machine with NO NVIDIA GPU (QEMU std-VGA), nvidia_init() is a pure
 *     no-op: it never touches MMIO or the framebuffer, so the working std-VGA
 *     desktop is undisturbed. QEMU has no 0x10DE device, so this path is what
 *     runs under QEMU and it changes nothing.
 *
 * MEMORY / ADDRESSING MODEL
 * -------------------------
 * Like the AHCI and e1000 drivers in this tree, low (< 4 GiB) physical MMIO
 * addresses from PCI BARs fall inside the kernel's 1:1 identity map (paging.c
 * identity-maps RAM and the <4 GiB MMIO hole), so a BAR base can be used
 * directly as a pointer. On the T410 the GPU BAR0 sits in the 32-bit MMIO hole
 * below 4 GiB. We ADDITIONALLY (and defensively) request an explicit identity
 * mapping for the register page(s) we touch, mirroring what kernel.c does for a
 * high framebuffer -- harmless if the page is already mapped, and correct if it
 * is not. We map only the first page we actually read, never the whole 16 MiB.
 *
 * Scope: kernel/drivers/gpu/nvidia.c + nvidia.h only.
 */

#include "../../include/pci.h"
#include "../../include/mem.h"       /* vmm_map_page, PAGE_PRESENT/PAGE_WRITE */
#include "../../include/types.h"
#include "../../include/kernel.h"    /* kprintf, PAGE_SIZE                    */
#include "../../include/drivers.h"   /* fb_info_t, framebuffer_get_info       */
#include "nvidia.h"

/* ----------------------------------------------------------------------- */
/* NVIDIA register offsets (byte offsets from the BAR0 register aperture).  */
/* Only PMC_BOOT_0 is read; the rest document the aperture for the future   */
/* native path and are intentionally unused for now.                        */
/* ----------------------------------------------------------------------- */
#define NV_PMC_BOOT_0          0x000000  /* chip id / architecture (read-only) */
#define NV_PMC_BOOT_0_ALT      0x000004  /* secondary boot/strap (unused)      */
#define NV_PMC_INTR_0          0x000100  /* master interrupt status (unused)   */
#define NV_PMC_ENABLE          0x000200  /* engine enable bits (unused)        */
#define NV_PROM_BASE           0x300000  /* VBIOS ROM shadow window (unused)   */
#define NV_PROM_SIZE           0x010000  /* 64 KiB ROM window (unused)         */
#define NV_PDISPLAY_BASE       0x610000  /* NV50 display engine (unused)       */

/*
 * BAR0 on Tesla parts is a 16 MiB register aperture. We only ever DEREFERENCE
 * the first page (PMC_BOOT_0 lives at offset 0), so this bound is the size we
 * RECORD for reporting; we do NOT eagerly map all of it.
 */
#define NV_BAR0_REG_APERTURE   (16u * 1024u * 1024u)

/* ----------------------------------------------------------------------- */
/* Driver state -- a single global GPU snapshot.                           */
/* ----------------------------------------------------------------------- */
static nvidia_gpu_t g_nv = { 0 };

/* ----------------------------------------------------------------------- */
/* Read-only MMIO accessor.                                                */
/*                                                                         */
/* `volatile` so the compiler cannot fold/reorder the device read. We only */
/* ever READ -- there is deliberately no mmio_write helper, to make it     */
/* structurally impossible to poke a register during detection.            */
/* ----------------------------------------------------------------------- */
static inline uint32_t nv_mmio_read32(volatile uint8_t* base, uint32_t reg) {
    return *(volatile uint32_t*)(base + reg);
}

/* ----------------------------------------------------------------------- */
/* PMC_BOOT_0 decode.                                                      */
/*                                                                         */
/* The classic NV layout packs the chip id in bits[31:20]; the high nibble */
/* selects the architecture generation. GT218 (NVS 3100M) reads back in    */
/* the NV50/Tesla range (an NVAx id). We decode defensively and only for   */
/* LOGGING -- nothing downstream depends on a specific value.              */
/* ----------------------------------------------------------------------- */
static uint32_t nv_decode_chip_id(uint32_t boot0) {
    /* Modern/most NV parts: id in bits[31:20] -> e.g. 0x0A8 for GT218. */
    uint32_t id = (boot0 >> 20) & 0xFFF;
    return id;
}

static nv_arch_t nv_decode_arch(uint32_t chip_id) {
    /* Architecture is the top byte of the 12-bit chip id (e.g. 0x0A8 -> 0xA0
     * generation == NV50/Tesla). Bucket conservatively. */
    uint32_t gen = chip_id & 0xF0;
    switch (gen) {
        case 0x00: /* very early NV04-class fall through to NV04 bucket */
        case 0x04: return NV_ARCH_NV04;
        case 0x10: return NV_ARCH_NV10;
        case 0x20: return NV_ARCH_NV20;
        case 0x30: return NV_ARCH_NV30;
        case 0x40:
        case 0x60: return NV_ARCH_NV40;
        case 0x50:
        case 0x80:
        case 0x90:
        case 0xA0: /* GT21x (GT218 == 0x0A8) lives here -- Tesla/NV50 family  */
        case 0xB0: return NV_ARCH_NV50;
        case 0xC0:
        case 0xD0: return NV_ARCH_NVC0;
        case 0xE0:
        case 0xF0: return NV_ARCH_NVE0;
        default:   return NV_ARCH_UNKNOWN;
    }
}

static const char* nv_arch_name(nv_arch_t a) {
    switch (a) {
        case NV_ARCH_NV04:  return "NV04 (TNT)";
        case NV_ARCH_NV10:  return "NV10";
        case NV_ARCH_NV20:  return "NV20";
        case NV_ARCH_NV30:  return "NV30";
        case NV_ARCH_NV40:  return "NV40";
        case NV_ARCH_NV50:  return "NV50/Tesla (GT2xx)";
        case NV_ARCH_NVC0:  return "NVC0/Fermi";
        case NV_ARCH_NVE0:  return "NVE0/Kepler";
        case NV_ARCH_NEWER: return "newer-than-tabulated";
        default:            return "unknown";
    }
}

/* ----------------------------------------------------------------------- */
/* PCI discovery.                                                          */
/*                                                                         */
/* Prefer an exact NVS 3100M match; otherwise accept ANY NVIDIA device     */
/* (vendor 0x10DE) whose PCI class is 0x03 (display controller). We do the */
/* generic scan ourselves over pci_find_class()/pci_find_device() so we    */
/* can match "vendor 0x10DE AND class 0x03" without assuming a subclass.   */
/* ----------------------------------------------------------------------- */
static pci_device_t* nv_find_gpu(void) {
    /* 1) Exact, verified target: the T410's NVS 3100M. */
    pci_device_t* dev = pci_find_device(NVIDIA_VENDOR_ID, NVIDIA_DEV_NVS_3100M);
    if (dev) {
        return dev;
    }

    /*
     * 2) Generic: any NVIDIA display controller. pci_find_class() matches on
     *    (class, subclass, prog_if) and there is no vendor-filtered helper, so
     *    we probe the two common display subclasses and confirm the vendor.
     *    Both lookups are O(table) over the bounded PCI device table -- no
     *    unbounded iteration.
     */
    static const uint8_t display_subclasses[] = {
        0x00,  /* VGA-compatible controller            */
        0x80,  /* "other" display controller           */
    };
    for (unsigned i = 0; i < sizeof(display_subclasses); i++) {
        /* prog_if is 0x00 for plain VGA and 0x00 for "other display" too. */
        pci_device_t* cand = pci_find_class(NVIDIA_PCI_CLASS_DISPLAY,
                                            display_subclasses[i], 0x00);
        if (cand && cand->vendor_id == NVIDIA_VENDOR_ID) {
            return cand;
        }
    }
    return NULL;
}

/* ----------------------------------------------------------------------- */
/* Firmware framebuffer adoption.                                          */
/*                                                                         */
/* The framebuffer driver already owns the firmware-configured linear FB   */
/* (set up from boot_info in kernel.c). We simply snapshot its geometry --  */
/* this is the "adopt the VESA/VBIOS mode, do not reprogram" contract.     */
/* ----------------------------------------------------------------------- */
static void nv_capture_firmware_fb(nvidia_gpu_t* gpu) {
    fb_info_t info;
    if (framebuffer_get_info(&info) == 0) {
        gpu->fb       = info;
        gpu->fb_valid = true;
    } else {
        gpu->fb_valid = false;
    }
}

/* ======================================================================= */
/* SCAFFOLD ONLY -- the future native mode-set path. NONE of this is called */
/* from nvidia_init(). It is documented + stubbed so the next iteration has */
/* a clear, safe starting point. Programming any of this blind WILL black   */
/* out a real panel, which is exactly why it stays disabled until it can be */
/* developed against the T410 with a serial console and recovery path.      */
/* ======================================================================= */

/*
 * TODO(native): read the VBIOS image.
 *   On Tesla the VBIOS is reachable two ways: (a) the PROM shadow window at
 *   BAR0+0x300000 (must first ungate it via PMC/PBUS straps), or (b) the
 *   legacy expansion-ROM BAR / 0xC0000 shadow copy. The image begins with the
 *   0x55 0xAA signature. This stub does NOT touch hardware; it only sketches
 *   the bounded copy loop a real implementation would use.
 */
static int nv_scaffold_read_vbios(volatile uint8_t* mmio_base,
                                  uint8_t* out, uint32_t out_len) {
    (void)mmio_base; (void)out; (void)out_len;
    /* DISABLED. A real version would, AFTER ungating PROM:
     *   uint32_t n = MIN(out_len, NV_PROM_SIZE);
     *   for (uint32_t i = 0; i < n; i++)             // bounded by NV_PROM_SIZE
     *       out[i] = nv_mmio_read32(mmio_base, NV_PROM_BASE + i) & 0xFF;
     *   if (out[0] != 0x55 || out[1] != 0xAA) return -1;   // ROM signature
     * Left unimplemented on purpose. */
    return -1;  /* not implemented */
}

/*
 * TODO(native): parse the Device Control Block (DCB) from the VBIOS to learn
 * which CRTC/encoder/connector drives the LVDS panel and at what native
 * timings. No hardware access; pure VBIOS-image parsing. Disabled.
 */
static int nv_scaffold_parse_dcb(const uint8_t* vbios, uint32_t len) {
    (void)vbios; (void)len;
    return -1;  /* not implemented */
}

/*
 * TODO(native): program the CRTC + PLL for a real mode-set. THIS is the step
 * that bricks the display if done without on-hardware iteration. Hard-disabled
 * behind an always-false guard so it can never run by accident.
 */
static int nv_scaffold_modeset(volatile uint8_t* mmio_base,
                               uint32_t width, uint32_t height, uint32_t bpp) {
    (void)mmio_base; (void)width; (void)height; (void)bpp;
    const bool NATIVE_MODESET_ENABLED = false;  /* DO NOT flip without HW test */
    if (!NATIVE_MODESET_ENABLED) {
        return -1;  /* disabled: never writes CRTC/PLL registers */
    }
    /* unreachable -- real CRTC/PLL/cursor programming would live here */
    return -1;
}

/* ======================================================================= */
/* Public API                                                              */
/* ======================================================================= */

void nvidia_init(void) {
    /* Idempotent: if we already detected the GPU, don't re-probe. */
    if (g_nv.present) {
        return;
    }

    pci_device_t* dev = nv_find_gpu();
    if (!dev) {
        /*
         * No NVIDIA display controller. This is the QEMU std-VGA case. Pure
         * no-op: do NOT touch MMIO or the framebuffer. Leave g_nv all-zero
         * (present == false). One quiet log line, nothing else.
         */
        kprintf("[NVIDIA] no NVIDIA display controller found "
                "(QEMU std-VGA / non-NVIDIA GPU); driver idle\n");
        return;
    }

    /* Record PCI identity. */
    g_nv.vendor_id = dev->vendor_id;
    g_nv.device_id = dev->device_id;
    g_nv.revision  = dev->revision_id;
    g_nv.pci_bus   = dev->bus;
    g_nv.pci_dev   = dev->device;
    g_nv.pci_func  = dev->function;

    kprintf("[NVIDIA] found GPU %04x:%04x rev 0x%02x at %02x:%02x.%x "
            "(class=%02x:%02x)\n",
            dev->vendor_id, dev->device_id, dev->revision_id,
            dev->bus, dev->device, dev->function,
            dev->class_code, dev->subclass);

    if (dev->device_id == NVIDIA_DEV_NVS_3100M) {
        kprintf("[NVIDIA] device matches NVS 3100M (GT218, Tesla/NV50) -- "
                "ThinkPad T410 discrete GPU\n");
    } else {
        kprintf("[NVIDIA] generic NVIDIA display controller "
                "(not the verified NVS 3100M id 0x0A6C)\n");
    }

    /*
     * Decode the BARs.
     *   BAR0 = MMIO register aperture (Tesla: ~16 MiB).
     *   BAR1 = VRAM / framebuffer aperture.
     * pci_get_bar() masks the low type bits and coalesces 64-bit BAR pairs.
     */
    uint64_t bar0 = pci_get_bar(dev, 0);
    uint64_t bar1 = pci_get_bar(dev, 1);

    g_nv.mmio_phys = bar0;
    g_nv.mmio_size = NV_BAR0_REG_APERTURE;   /* aperture size we report        */
    g_nv.vram_phys = bar1;
    g_nv.vram_size = 0;                       /* not probed (see note below)    */

    /*
     * Enable memory-space decoding so the register aperture responds. This is
     * a PCI *config-space* COMMAND write (not a GPU MMIO write) and is required
     * for the aperture to be readable at all. We deliberately do NOT enable bus
     * mastering: we issue no DMA. We do NOT change the GPU's display state.
     */
    pci_enable_memory_space(dev);

    if (bar0 == 0) {
        /*
         * No register aperture. Still safe: report what we have (incl. the
         * firmware framebuffer) and mark present, but skip the PMC_BOOT_0 read.
         */
        kprintf("[NVIDIA] BAR0 (MMIO register aperture) is empty -- "
                "skipping chip-id read\n");
    } else {
        /*
         * Defensive mapping: the T410 GPU BAR0 lives in the <4 GiB MMIO hole
         * that paging.c identity-maps, so `bar0` is usable as a pointer as-is
         * (same assumption AHCI/e1000 make). We ADDITIONALLY request an
         * explicit identity mapping of just the first register page we read --
         * harmless if already mapped, correct if not. We never map the whole
         * 16 MiB; only the page containing PMC_BOOT_0 (offset 0).
         */
        uint64_t page = bar0 & ~((uint64_t)PAGE_SIZE - 1);
        vmm_map_page((void*)(uintptr_t)page, (void*)(uintptr_t)page,
                     PAGE_PRESENT | PAGE_WRITE);

        volatile uint8_t* mmio = (volatile uint8_t*)(uintptr_t)bar0;

        /* The ONLY hardware access in this driver: a single read-only probe. */
        uint32_t boot0 = nv_mmio_read32(mmio, NV_PMC_BOOT_0);
        g_nv.pmc_boot_0 = boot0;
        g_nv.chip_id    = nv_decode_chip_id(boot0);
        g_nv.arch       = nv_decode_arch(g_nv.chip_id);

        kprintf("[NVIDIA] BAR0 (MMIO) @ 0x%lx size~%u MiB; "
                "BAR1 (VRAM) @ 0x%lx\n",
                (unsigned long)bar0,
                (unsigned)(NV_BAR0_REG_APERTURE / (1024 * 1024)),
                (unsigned long)bar1);
        kprintf("[NVIDIA] PMC_BOOT_0 = 0x%08x -> chip_id 0x%03x, arch %s\n",
                boot0, g_nv.chip_id, nv_arch_name(g_nv.arch));

        /*
         * Sanity note (logged, not enforced): a GT218 reads back an NVAx-class
         * id (architecture nibble 0xA0). If we see something wildly different
         * the aperture read may be bogus -- but we still do not act on it.
         */
        if (g_nv.arch != NV_ARCH_NV50) {
            kprintf("[NVIDIA] note: decoded arch is not NV50/Tesla -- "
                    "expected for GT218; treating as informational only\n");
        }
    }

    /*
     * Adopt the firmware-configured linear framebuffer. We do NOT reprogram
     * the display -- we capture the VESA/VBIOS mode the firmware already set
     * (the surface the boot splash renders to) and expose it via the accessor.
     */
    nv_capture_firmware_fb(&g_nv);
    if (g_nv.fb_valid) {
        kprintf("[NVIDIA] firmware framebuffer: %ux%u, %u bpp, pitch %u, "
                "phys 0x%lx (mode left UNCHANGED -- no native mode-set)\n",
                g_nv.fb.width, g_nv.fb.height, g_nv.fb.bpp, g_nv.fb.pitch,
                (unsigned long)g_nv.fb.phys_base);
    } else {
        kprintf("[NVIDIA] no firmware framebuffer reported by the kernel "
                "(framebuffer driver not initialised?)\n");
    }

    /*
     * Native mode-setting status (HONEST): scaffolded, DISABLED. The VBIOS
     * read, DCB parse, and CRTC/PLL programming helpers exist in this file but
     * are never invoked. Reference them once here so the compiler keeps them
     * and to document intent -- still with ZERO hardware effect (the guards
     * inside each make them no-ops even if reached).
     */
    if (/* never */ false) {
        uint8_t dummy = 0;
        (void)nv_scaffold_read_vbios((volatile uint8_t*)(uintptr_t)bar0, &dummy, 0);
        (void)nv_scaffold_parse_dcb(&dummy, 0);
        (void)nv_scaffold_modeset((volatile uint8_t*)(uintptr_t)bar0, 0, 0, 0);
    }
    kprintf("[NVIDIA] native mode-setting: SCAFFOLDED + DISABLED "
            "(using firmware framebuffer; no CRTC/PLL programming)\n");

    g_nv.present = true;
    kprintf("[NVIDIA] GPU present and registered (detection-only driver)\n");
}

bool nvidia_is_present(void) {
    return g_nv.present;
}

const nvidia_gpu_t* nvidia_get_gpu(void) {
    return &g_nv;
}

int nvidia_get_framebuffer(fb_info_t* out) {
    if (!out || !g_nv.present || !g_nv.fb_valid) {
        return -1;
    }
    *out = g_nv.fb;
    return 0;
}
