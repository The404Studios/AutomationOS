#ifndef NVIDIA_H
#define NVIDIA_H

/*
 * NVIDIA GPU driver (detection + firmware-framebuffer foundation)
 * ===============================================================
 *
 * TARGET HARDWARE: Lenovo ThinkPad T410 discrete GPU --
 *   NVIDIA NVS 3100M, PCI 0x10DE:0x0A6C, GT218 core (Tesla / "NV50" family,
 *   the nv50/NVAx generation in the nouveau lineage).
 *
 * HONEST SCOPE: this driver DETECTS the GPU and adopts the framebuffer the
 * firmware (VBIOS/VESA) already programmed -- it performs NO native mode-set.
 * It NEVER writes a GPU MMIO register during probing; CRTC/PLL programming is
 * scaffolded but disabled (see nvidia.c). On a machine with no NVIDIA GPU
 * (e.g. QEMU std-VGA) nvidia_init() is a safe no-op that touches nothing.
 *
 * See nvidia.c for the full design + safety notes.
 */

#include "../../include/types.h"
#include "../../include/drivers.h"   /* fb_info_t */

/* PCI identity ----------------------------------------------------------- */
#define NVIDIA_VENDOR_ID        0x10DE  /* NVIDIA Corporation                 */
#define NVIDIA_DEV_NVS_3100M    0x0A6C  /* ThinkPad T410 NVS 3100M (GT218)    */

/* PCI class for a display controller (class 0x03). Subclass 0x00 == VGA-
 * compatible, 0x80 == "other display controller"; we match the class only and
 * accept any display subclass so we still recognise the GPU if firmware left it
 * in a non-VGA state. */
#define NVIDIA_PCI_CLASS_DISPLAY 0x03

/*
 * GPU architecture families we recognise (decoded from PMC_BOOT_0). These are
 * the high "architecture" nibbles of the chip id; GT218 (the NVS 3100M core)
 * reports an NVAx id in the NV50/Tesla generation.
 */
typedef enum {
    NV_ARCH_UNKNOWN = 0,
    NV_ARCH_NV04,        /* 0x04 -- RIVA TNT era                            */
    NV_ARCH_NV10,        /* 0x10                                            */
    NV_ARCH_NV20,        /* 0x20                                            */
    NV_ARCH_NV30,        /* 0x30                                            */
    NV_ARCH_NV40,        /* 0x40 / 0x60                                     */
    NV_ARCH_NV50,        /* 0x50..0x90 -- Tesla (GT2xx incl. GT218)         */
    NV_ARCH_NVC0,        /* 0xC0 -- Fermi                                   */
    NV_ARCH_NVE0,        /* 0xE0 -- Kepler                                  */
    NV_ARCH_NEWER,       /* anything past what we tabulate                  */
} nv_arch_t;

/*
 * Public snapshot of the detected GPU. Filled by nvidia_init(); all-zero /
 * present=false when no NVIDIA GPU exists. Safe to read at any time.
 */
typedef struct {
    bool       present;          /* an NVIDIA display controller was found    */
    uint16_t   vendor_id;        /* always NVIDIA_VENDOR_ID when present       */
    uint16_t   device_id;        /* e.g. 0x0A6C for the NVS 3100M              */
    uint8_t    revision;         /* PCI revision id                            */
    uint8_t    pci_bus;
    uint8_t    pci_dev;
    uint8_t    pci_func;

    uint64_t   mmio_phys;        /* BAR0 physical base (register aperture)     */
    uint64_t   mmio_size;        /* BAR0 size we mapped (bounded)              */
    uint64_t   vram_phys;        /* BAR1 physical base (VRAM/FB aperture)      */
    uint64_t   vram_size;        /* BAR1 reported size (informational)         */

    uint32_t   pmc_boot_0;       /* raw PMC_BOOT_0 register value              */
    uint32_t   chip_id;          /* decoded chip id (e.g. 0x0A8 for GT218)     */
    nv_arch_t  arch;             /* decoded architecture family                */

    bool       fb_valid;        /* firmware framebuffer geometry captured     */
    fb_info_t  fb;               /* the firmware-configured linear framebuffer */
} nvidia_gpu_t;

/*
 * nvidia_init() -- scan PCI for an NVIDIA display controller.
 *
 *   - If found: enable memory space, map BAR0 (register aperture) and record
 *     BAR1 (VRAM aperture), read PMC_BOOT_0 to identify the chip, capture the
 *     firmware framebuffer geometry, log everything, and mark the GPU present.
 *     It does NOT modify any display mode -- the screen stays exactly as the
 *     firmware left it.
 *   - If NOT found (e.g. QEMU std-VGA): a pure no-op. No MMIO, no framebuffer
 *     access, nothing that could disturb the working display.
 *
 * Intended to be called by the kernel dispatcher AFTER pci_init().
 */
void nvidia_init(void);

/* Accessors (all safe before/after init; return defaults when not present). */
bool             nvidia_is_present(void);
const nvidia_gpu_t* nvidia_get_gpu(void);

/*
 * Copy the firmware framebuffer geometry of the detected GPU into *out.
 * Returns 0 on success, -1 if no NVIDIA GPU / no valid framebuffer was found.
 * This is the clean accessor for the firmware-configured linear framebuffer;
 * callers can use it without poking the GPU.
 */
int nvidia_get_framebuffer(fb_info_t* out);

#endif /* NVIDIA_H */
