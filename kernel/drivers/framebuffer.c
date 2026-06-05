/**
 * Framebuffer Driver - VESA/VBE Linear Framebuffer
 *
 * Provides pixel plotting and text rendering for 32-bit RGB framebuffers.
 * Supports basic VGA-style text output with an 8x8 bitmap font.
 */

#include "../include/drivers.h"
#include "../include/types.h"

#ifdef FB_WC
/* =========================================================================
 * GATED Write-Combining (WC) framebuffer acceleration  --  #ifdef FB_WC ONLY
 * =========================================================================
 *
 * NOTHING in this block compiles unless the kernel is built with -DFB_WC
 * (FB_WC=1 bash scripts/quick_build.sh). The DEFAULT kernel is byte-identical.
 *
 * WHY: On the ThinkPad T410 the firmware maps the linear framebuffer UNCACHED
 * (UC). Every pixel store to a full-screen game therefore round-trips to the
 * GPU as a single non-buffered write -> ~10 fps. Marking the FB region
 * Write-Combining (WC) lets the CPU coalesce many small stores into burst
 * writes across the PCIe link, which on real hardware is a multi-x speedup
 * for the whole compositor/desktop.
 *
 * HOW: We program a single VARIABLE-RANGE MTRR (Memory Type Range Register)
 * to cover the FB physical range with memory type WC (0x01). We find a FREE
 * variable MTRR (PHYSMASK valid bit clear) so we never clobber a firmware
 * MTRR that is already in use.
 *
 * HONEST CAVEAT: MTRR overlap rules say UC WINS. If the firmware has already
 * placed a variable (or fixed) MTRR marking this region UC, our WC MTRR will
 * NOT take effect (the effective type stays UC). That is exactly why this is
 * opt-in/testable rather than default-on: it must be validated on the real
 * T410 panel, where only the user can see the fps change. In QEMU the FB is
 * cached anyway, so there is nothing to measure there -- we only prove the
 * setup runs and the kernel still boots cleanly.
 * ========================================================================= */

#include "../include/kernel.h"   /* kprintf */
#include "../include/x86_64.h"   /* rdmsr / wrmsr / read_cr0 / write_cr0 / cli / sti */

/* --- IA32 MTRR MSR architecture (Intel SDM Vol 3A, 11.11) --------------- */
#define IA32_MTRRCAP            0x000000FE  /* RO: VCNT (bits 7:0), FIX (10), WC (8), SMRR (11) */
#define IA32_MTRR_DEF_TYPE      0x000002FF  /* default type + E (bit 11) + FE (bit 10)         */
#define IA32_MTRR_PHYSBASE(n)   (0x00000200 + (n) * 2)  /* base | type (bits 7:0)              */
#define IA32_MTRR_PHYSMASK(n)   (0x00000201 + (n) * 2)  /* mask | V (bit 11)                   */

#define MTRR_TYPE_WC            0x01ULL     /* Write-Combining memory type    */
#define MTRR_DEFTYPE_E          (1ULL << 11)/* MTRRs enable bit in DEF_TYPE   */
#define MTRR_PHYSMASK_VALID     (1ULL << 11)/* "this variable MTRR is in use" */
#define MTRR_PHYSBASE_ADDR_MASK 0xFFFFFFFFFFFFF000ULL  /* base address bits   */

/* CR0.CD (bit 30) = Cache Disable, CR0.NW (bit 29) = Not-Write-through. The
 * SDM MTRR-modification sequence requires CD=1,NW=0 (caches off) around the
 * MSR writes, with a WBINVD before and after. */
#define CR0_CD                  (1ULL << 30)
#define CR0_NW                  (1ULL << 29)

/* Round v UP to the next power of two (v assumed > 0 and <= 2^62). Returns v
 * unchanged when it is already a power of two. */
static uint64_t fb_wc_po2_up(uint64_t v) {
    if (v == 0) return 1;
    uint64_t p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

/* Query the CPU's physical-address width via CPUID 0x80000008 (EAX bits 7:0).
 * Returns a PHYSMASK with the low `maxphysaddr` bits set above bit 12, used to
 * mask the MTRR PHYSMASK to the architectural width. Falls back to 36 bits
 * (mask 0x0000000FFFFFF000) if the leaf is unsupported -- the conservative
 * legacy default that is valid on virtually all x86_64 parts. */
static uint64_t fb_wc_physmask_bits(void) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    uint8_t  phys_bits = 36;  /* safe default */

    /* Is extended leaf 0x80000008 available? Check max extended leaf first. */
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000000U));
    if (eax >= 0x80000008U) {
        __asm__ volatile("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(0x80000008U));
        uint8_t reported = (uint8_t)(eax & 0xFF);
        if (reported >= 32 && reported <= 52) {  /* sanity bound */
            phys_bits = reported;
        }
    }

    /* Mask = bits [phys_bits-1 : 12] set. */
    uint64_t full = (phys_bits >= 64) ? ~0ULL : ((1ULL << phys_bits) - 1ULL);
    return full & MTRR_PHYSBASE_ADDR_MASK;
}

/**
 * fb_enable_write_combining -- mark [base, base+size) Write-Combining via a
 * free variable-range MTRR.  GATED: only built/linked under -DFB_WC.
 *
 * @param base  physical base of the framebuffer (e.g. 0xFD000000 on the T410)
 * @param size  framebuffer byte size (pitch * height, ~4 MB at 1280x800x4)
 *
 * Steps (Intel SDM Vol 3A "MTRR Considerations"):
 *   1. Read IA32_MTRRCAP to get VCNT (number of variable MTRRs).
 *   2. Compute a power-of-two range that COVERS the FB and is BASE-ALIGNED to
 *      that power of two (an MTRR requirement). If base is not aligned to the
 *      chosen size, LOG and BAIL -- we never force a misaligned MTRR.
 *   3. Find a FREE variable MTRR (PHYSMASK.V == 0); never clobber one in use.
 *   4. Enter the no-caching window (CR0.CD=1, NW=0, WBINVD, disable MTRRs),
 *      write PHYSBASE=base|WC and PHYSMASK=~(size-1)&physmask|V, then re-enable
 *      MTRRs, WBINVD, restore CR0. Interrupts are disabled across the change.
 *   5. Log the slot/base/size/type so the serial log proves it took.
 */
void fb_enable_write_combining(uint64_t base, uint64_t size) {
    if (!base || !size) {
        kprintf("[FB-WC] skip: invalid base/size (base=0x%lx size=0x%lx)\n",
                (unsigned long)base, (unsigned long)size);
        return;
    }

    /* --- 1. variable MTRR count --------------------------------------- */
    uint64_t cap   = rdmsr(IA32_MTRRCAP);
    uint32_t vcnt  = (uint32_t)(cap & 0xFF);
    if (vcnt == 0) {
        kprintf("[FB-WC] skip: CPU reports 0 variable MTRRs (cap=0x%lx)\n",
                (unsigned long)cap);
        return;
    }

    /* --- 2. power-of-two range that covers the FB, base-aligned ------- */
    uint64_t po2 = fb_wc_po2_up(size);
    /* MTRR base must be aligned to the range size. If the FB base is not
     * aligned to po2, the smallest legal covering MTRR would have to be the
     * largest power-of-two that DOES divide base AND still covers size. If no
     * such single MTRR exists (base poorly aligned), we refuse rather than
     * mark the wrong physical range WC. */
    if (base & (po2 - 1)) {
        /* Base not aligned to the covering po2. Try growing po2 until base is
         * aligned to it (this also keeps coverage, since po2 only increases),
         * but cap the growth so we never mark a huge unrelated region WC. A
         * 4 MB FB at 0xFD000000 is already 64 MB-aligned, so the common case
         * needs no growth at all. */
        uint64_t grown = po2;
        int bailed = 1;
        for (int i = 0; i < 8; i++) {     /* allow up to 256x growth */
            if ((base & (grown - 1)) == 0) { bailed = 0; break; }
            grown <<= 1;
        }
        if (bailed) {
            kprintf("[FB-WC] BAIL: FB base 0x%lx not alignable to a covering "
                    "power-of-two (size=0x%lx, po2=0x%lx). Not forcing a "
                    "misaligned MTRR.\n",
                    (unsigned long)base, (unsigned long)size,
                    (unsigned long)po2);
            return;
        }
        po2 = grown;
    }

    /* --- 3. find a FREE variable MTRR (PHYSMASK.V == 0) --------------- */
    int slot = -1;
    for (uint32_t i = 0; i < vcnt; i++) {
        uint64_t mask = rdmsr(IA32_MTRR_PHYSMASK(i));
        if (!(mask & MTRR_PHYSMASK_VALID)) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        kprintf("[FB-WC] BAIL: no free variable MTRR (all %u in use by "
                "firmware)\n", vcnt);
        return;
    }

    /* --- build the PHYSBASE/PHYSMASK values --------------------------- */
    uint64_t physmask_bits = fb_wc_physmask_bits();
    uint64_t physbase = (base & MTRR_PHYSBASE_ADDR_MASK) | MTRR_TYPE_WC;
    uint64_t physmask = ((~(po2 - 1ULL)) & physmask_bits) | MTRR_PHYSMASK_VALID;

    /* --- 4. apply under the SDM MTRR-modification protocol ------------ */
    /* Disable interrupts across the whole change. */
    cli();

    /* a) Enter no-fill cache mode: set CR0.CD, clear CR0.NW. */
    uint64_t cr0_saved = read_cr0();
    uint64_t cr0_nocache = (cr0_saved | CR0_CD) & ~CR0_NW;
    write_cr0(cr0_nocache);

    /* b) Flush caches and TLBs (WBINVD then reload CR3). */
    __asm__ volatile("wbinvd" ::: "memory");
    write_cr3(read_cr3());

    /* c) Disable MTRRs (clear DEF_TYPE.E) so the table is inert while edited. */
    uint64_t deftype_saved = rdmsr(IA32_MTRR_DEF_TYPE);
    wrmsr(IA32_MTRR_DEF_TYPE, deftype_saved & ~MTRR_DEFTYPE_E);

    /* d) Program the chosen variable MTRR pair. */
    wrmsr(IA32_MTRR_PHYSBASE(slot), physbase);
    wrmsr(IA32_MTRR_PHYSMASK(slot), physmask);

    /* e) Re-enable MTRRs (restore DEF_TYPE, with E set as before). */
    wrmsr(IA32_MTRR_DEF_TYPE, deftype_saved | MTRR_DEFTYPE_E);

    /* f) Flush again, then restore CR0 (exit no-fill mode). */
    __asm__ volatile("wbinvd" ::: "memory");
    write_cr3(read_cr3());
    write_cr0(cr0_saved);

    sti();

    /* --- 5. log proof that it took ----------------------------------- */
    kprintf("[FB-WC] Write-Combining enabled: MTRR slot %d  "
            "base=0x%lx size=0x%lx (po2=0x%lx) type=WC(0x01)\n",
            slot, (unsigned long)base, (unsigned long)size,
            (unsigned long)po2);
    kprintf("[FB-WC]   PHYSBASE[%d]=0x%lx PHYSMASK[%d]=0x%lx (physmask_bits=0x%lx)\n",
            slot, (unsigned long)physbase, slot, (unsigned long)physmask,
            (unsigned long)physmask_bits);
    kprintf("[FB-WC]   NOTE: if firmware already marks this region UC, UC wins "
            "on overlap and WC will not take (opt-in/testable by design).\n");
}
#endif /* FB_WC */

// Framebuffer state
static struct {
    uint32_t* buffer;
    uint64_t phys_base;  // physical base address as passed by multiboot
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      // bytes per scanline
    uint32_t bpp;        // bits per pixel
    bool initialized;
} fb_state = {0};

// Simple 8x8 bitmap font (ASCII 32-127)
// Each character is 8 bytes, one bit per pixel, row-major
static const uint8_t font_8x8[96][8] = {
    // 0x20 ' ' (space)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // 0x21 '!'
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},
    // 0x22 '"'
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // 0x23 '#'
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},
    // 0x24 '$'
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00},
    // 0x25 '%'
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},
    // 0x26 '&'
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00},
    // 0x27 '''
    {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},
    // 0x28 '('
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00},
    // 0x29 ')'
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},
    // 0x2A '*'
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},
    // 0x2B '+'
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},
    // 0x2C ','
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06},
    // 0x2D '-'
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},
    // 0x2E '.'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00},
    // 0x2F '/'
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},
    // 0x30 '0'
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00},
    // 0x31 '1'
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},
    // 0x32 '2'
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00},
    // 0x33 '3'
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},
    // 0x34 '4'
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00},
    // 0x35 '5'
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},
    // 0x36 '6'
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00},
    // 0x37 '7'
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},
    // 0x38 '8'
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00},
    // 0x39 '9'
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},
    // 0x3A ':'
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00},
    // 0x3B ';'
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},
    // 0x3C '<'
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00},
    // 0x3D '='
    {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},
    // 0x3E '>'
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00},
    // 0x3F '?'
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},
    // 0x40 '@'
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00},
    // 0x41 'A'
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},
    // 0x42 'B'
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},
    // 0x43 'C'
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},
    // 0x44 'D'
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},
    // 0x45 'E'
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},
    // 0x46 'F'
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},
    // 0x47 'G'
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},
    // 0x48 'H'
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},
    // 0x49 'I'
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    // 0x4A 'J'
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},
    // 0x4B 'K'
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},
    // 0x4C 'L'
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},
    // 0x4D 'M'
    {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},
    // 0x4E 'N'
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},
    // 0x4F 'O'
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},
    // 0x50 'P'
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},
    // 0x51 'Q'
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},
    // 0x52 'R'
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},
    // 0x53 'S'
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},
    // 0x54 'T'
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    // 0x55 'U'
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},
    // 0x56 'V'
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},
    // 0x57 'W'
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},
    // 0x58 'X'
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},
    // 0x59 'Y'
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},
    // 0x5A 'Z'
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00},
    // 0x5B '['
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},
    // 0x5C '\'
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00},
    // 0x5D ']'
    {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},
    // 0x5E '^'
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00},
    // 0x5F '_'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},
    // 0x60 '`'
    {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
    // 0x61 'a'
    {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},
    // 0x62 'b'
    {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00},
    // 0x63 'c'
    {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},
    // 0x64 'd'
    {0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6E, 0x00},
    // 0x65 'e'
    {0x00, 0x00, 0x1E, 0x33, 0x3f, 0x03, 0x1E, 0x00},
    // 0x66 'f'
    {0x1C, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0F, 0x00},
    // 0x67 'g'
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    // 0x68 'h'
    {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00},
    // 0x69 'i'
    {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    // 0x6A 'j'
    {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E},
    // 0x6B 'k'
    {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},
    // 0x6C 'l'
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    // 0x6D 'm'
    {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},
    // 0x6E 'n'
    {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00},
    // 0x6F 'o'
    {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},
    // 0x70 'p'
    {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F},
    // 0x71 'q'
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},
    // 0x72 'r'
    {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00},
    // 0x73 's'
    {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},
    // 0x74 't'
    {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00},
    // 0x75 'u'
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},
    // 0x76 'v'
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},
    // 0x77 'w'
    {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},
    // 0x78 'x'
    {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00},
    // 0x79 'y'
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    // 0x7A 'z'
    {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00},
    // 0x7B '{'
    {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},
    // 0x7C '|'
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00},
    // 0x7D '}'
    {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},
    // 0x7E '~'
    {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // 0x7F (DEL - block character)
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
};

/**
 * Plot a single pixel at (x, y) with given color
 */
void framebuffer_plot_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_state.initialized || x >= fb_state.width || y >= fb_state.height) {
        return;
    }

    // Calculate pixel offset: pitch is in bytes, we need pixel offset
    uint32_t* pixel = (uint32_t*)((uint8_t*)fb_state.buffer + y * fb_state.pitch + x * 4);
    *pixel = color;
}

/* Internal alias for convenience */
static inline void plot_pixel(uint32_t x, uint32_t y, uint32_t color) {
    framebuffer_plot_pixel(x, y, color);
}

/**
 * Initialize the framebuffer driver
 *
 * @param fb_addr Physical address of the framebuffer
 * @param width Width in pixels
 * @param height Height in pixels
 * @param pitch Bytes per scanline (usually width * 4 for 32-bit color)
 */
void framebuffer_init(uint64_t fb_addr, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!fb_addr || width == 0 || height == 0 || pitch == 0) {
        return;
    }

    fb_state.phys_base  = fb_addr;
    fb_state.buffer     = (uint32_t*)(uintptr_t)fb_addr;
    fb_state.width      = width;
    fb_state.height     = height;
    fb_state.pitch      = pitch;
    // Derive bpp from pitch/width: bytes_per_pixel * 8.
    // Falls back to 32 if the division is not clean (should not happen for
    // standard VESA 32-bpp linear framebuffers).
    fb_state.bpp        = (width > 0) ? (pitch / width) * 8 : 32;
    fb_state.initialized = true;
}

/*
 * fill_row_u64 -- write `count` pixels of `color` starting at `dst`.
 *
 * Uses 64-bit stores (2 pixels per write) for the bulk of the row, then
 * falls back to a single 32-bit store for an odd trailing pixel.
 * `dst` must be a valid uint32_t* into the framebuffer; no alignment
 * requirement beyond what the CPU handles for unaligned 64-bit stores
 * (x86_64 handles these in hardware at minimal cost).
 *
 * Throughput: ~1 store per 8 bytes vs. 1 store per 4 bytes — 2x fewer
 * store instructions for the bulk path; combined with eliminated loop
 * overhead (bounds check, function call) the practical gain is ~4–8x.
 */
static inline void fill_row_u64(uint32_t* dst, uint32_t color, uint32_t count) {
    /* Pack two pixels into one 64-bit word. */
    uint64_t pat = ((uint64_t)color << 32) | (uint64_t)color;
    uint64_t* dst64 = (uint64_t*)dst;
    uint32_t pairs = count >> 1;      /* number of 8-byte writes */
    uint32_t tail  = count & 1u;      /* 0 or 1 leftover pixel  */

    for (uint32_t i = 0; i < pairs; i++) {
        dst64[i] = pat;
    }
    if (tail) {
        /* One leftover pixel at the end of the row. */
        dst[count - 1] = color;
    }
}

/**
 * Clear the framebuffer to a solid color
 *
 * @param color 32-bit RGB color (0xRRGGBB)
 *
 * Optimised: uses 64-bit stores (2 pixels/word) per scanline instead of
 * per-pixel plot_pixel calls.  Respects pitch so non-contiguous framebuffers
 * (pitch != width*4) are handled correctly.
 */
void framebuffer_clear(uint32_t color) {
    if (!fb_state.initialized) {
        return;
    }

    const uint32_t w = fb_state.width;
    const uint32_t h = fb_state.height;
    const uint32_t pitch = fb_state.pitch;   /* bytes per scanline */
    uint8_t* row_ptr = (uint8_t*)fb_state.buffer;

    for (uint32_t y = 0; y < h; y++) {
        fill_row_u64((uint32_t*)row_ptr, color, w);
        row_ptr += pitch;
    }
}

/**
 * Render a single character at (x, y) with given color
 *
 * @param c ASCII character to render (32-127)
 * @param x X position in pixels
 * @param y Y position in pixels
 * @param color 32-bit RGB foreground color
 */
void framebuffer_putchar(char c, uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_state.initialized) {
        return;
    }

    // Only support printable ASCII
    if (c < 32 || c > 127) {
        c = '?';
    }

    const uint8_t* glyph = font_8x8[c - 32];

    // Render 8x8 character bitmap
    for (uint32_t row = 0; row < 8; row++) {
        uint8_t row_data = glyph[row];
        for (uint32_t col = 0; col < 8; col++) {
            /* Font is authored LSB-first (bit 0 = leftmost pixel), matching the
             * classic IBM VGA 8x8 layout. Testing bit `col` (not `7-col`) keeps
             * glyphs upright instead of mirrored. */
            if (row_data & (1 << col)) {
                plot_pixel(x + col, y + row, color);
            }
        }
    }
}

/**
 * Draw a NUL-terminated string using the 8x8 font, magnified by `scale`
 * (each set pixel becomes a scale x scale block). Used for the boot splash.
 */
void framebuffer_puts_scaled(const char* s, uint32_t x, uint32_t y,
                             uint32_t color, uint32_t scale) {
    if (!fb_state.initialized || scale == 0 || !s) {
        return;
    }
    uint32_t cx = x;
    for (uint32_t i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        if (c < 32 || c > 127) {
            c = '?';
        }
        const uint8_t* glyph = font_8x8[c - 32];
        for (uint32_t row = 0; row < 8; row++) {
            uint8_t row_data = glyph[row];
            for (uint32_t col = 0; col < 8; col++) {
                if (row_data & (1 << col)) {
                    for (uint32_t sy = 0; sy < scale; sy++) {
                        for (uint32_t sx = 0; sx < scale; sx++) {
                            plot_pixel(cx + col * scale + sx, y + row * scale + sy, color);
                        }
                    }
                }
            }
        }
        cx += 8 * scale;
    }
}

/**
 * Draw a filled rectangle
 *
 * @param x X position
 * @param y Y position
 * @param width Width in pixels
 * @param height Height in pixels
 * @param color 32-bit RGB color
 *
 * Optimised: one fill_row_u64 call per scanline instead of width*height
 * individual plot_pixel calls.  Pitch is respected.
 */
void framebuffer_draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) {
    if (!fb_state.initialized) {
        return;
    }

    /* Clip to screen. */
    if (x >= fb_state.width || y >= fb_state.height) return;
    if (x + width  > fb_state.width)  width  = fb_state.width  - x;
    if (y + height > fb_state.height) height = fb_state.height - y;

    const uint32_t pitch = fb_state.pitch;
    /* Pointer to first pixel of the rectangle's top-left corner. */
    uint8_t* row_ptr = (uint8_t*)fb_state.buffer + (uint64_t)y * pitch + (uint64_t)x * 4;

    for (uint32_t dy = 0; dy < height; dy++) {
        fill_row_u64((uint32_t*)row_ptr, color, width);
        row_ptr += pitch;
    }
}

/**
 * Draw a horizontal line
 *
 * @param x Starting X position
 * @param y Y position
 * @param length Length in pixels
 * @param color 32-bit RGB color
 *
 * Optimised: fill_row_u64 replaces length individual plot_pixel calls.
 */
void framebuffer_draw_hline(uint32_t x, uint32_t y, uint32_t length, uint32_t color) {
    if (!fb_state.initialized) {
        return;
    }

    /* Clip to screen. */
    if (x >= fb_state.width || y >= fb_state.height) return;
    if (x + length > fb_state.width) length = fb_state.width - x;

    uint32_t* dst = (uint32_t*)((uint8_t*)fb_state.buffer
                                + (uint64_t)y * fb_state.pitch
                                + (uint64_t)x * 4);
    fill_row_u64(dst, color, length);
}

/**
 * Draw a vertical line
 *
 * @param x X position
 * @param y Starting Y position
 * @param length Length in pixels
 * @param color 32-bit RGB color
 *
 * One pixel per scanline — uses direct pointer arithmetic to avoid the
 * bounds-check overhead of plot_pixel on every row.
 */
void framebuffer_draw_vline(uint32_t x, uint32_t y, uint32_t length, uint32_t color) {
    if (!fb_state.initialized) {
        return;
    }

    /* Clip to screen. */
    if (x >= fb_state.width || y >= fb_state.height) return;
    if (y + length > fb_state.height) length = fb_state.height - y;

    const uint32_t pitch = fb_state.pitch;
    uint8_t* ptr = (uint8_t*)fb_state.buffer + (uint64_t)y * pitch + (uint64_t)x * 4;

    for (uint32_t dy = 0; dy < length; dy++) {
        *(uint32_t*)ptr = color;
        ptr += pitch;
    }
}

/**
 * Fill out framebuffer geometry for userspace export.
 *
 * @param out  Pointer to caller-supplied fb_info_t to fill.
 * @return     0 on success, -1 if the framebuffer has not been initialized.
 */
int framebuffer_get_info(fb_info_t* out) {
    if (!fb_state.initialized || !out) {
        return -1;
    }
    out->phys_base = fb_state.phys_base;
    out->width     = fb_state.width;
    out->height    = fb_state.height;
    out->pitch     = fb_state.pitch;
    out->bpp       = fb_state.bpp;
    return 0;
}

/* =========================================================================
 * Boot / recovery loading animation -- a "fluid circle": a comet of orbiting
 * filled discs. Pure 32-bit integer math (no float, no 64-bit division in the
 * hot path -> no libgcc soft-float; verified by the no-float link gate). The
 * sin LUT + isqrt are copies of the compositor's (compositor_m8.c) so the
 * kernel boot spinner and the userspace recovery overlay animate identically.
 * The kernel cannot read back the framebuffer (no alpha blend), so the comet
 * trail DARKENS toward the known splash background instead of true alpha.
 * ========================================================================= */

/* sin(deg)*256, Q8 fixed point. Mirrors compositor_m8.c sin_q/cos_q. */
static const int32_t FB_SINQ_TBL[19] = {  /* sin(0..90 by 5 deg) * 256 */
      0,  22,  44,  66,  88, 109, 128, 147, 165, 181,
    196, 209, 221, 231, 240, 247, 252, 255, 256
};
static int32_t fb_sin_q(int32_t deg) {
    deg %= 360; if (deg < 0) deg += 360;
    int32_t sign = 1;
    if (deg >= 180) { deg -= 180; sign = -1; }
    if (deg > 90) deg = 180 - deg;
    int32_t i = deg / 5;
    int32_t frac = deg - i * 5;
    int32_t a = FB_SINQ_TBL[i];
    int32_t b = FB_SINQ_TBL[i + 1];
    return sign * (a + (b - a) * frac / 5);
}
static int32_t fb_cos_q(int32_t deg) { return fb_sin_q(deg + 90); }

/* Integer sqrt (Newton). 32-bit divides only -> hardware idiv, no libgcc. */
static uint32_t fb_isqrt32(uint32_t n) {
    if (n == 0) return 0;
    uint32_t x = n, y = (x + 1u) / 2u;
    while (y < x) { x = y; y = (x + n / x) / 2u; }
    return x;
}

/* TSC read (rdtsc in perf.h is a static inline with no linkable symbol; carry a
 * private copy, same as page_cache_test.c, so the spinner has a clock pre-PIT). */
static inline uint64_t fb_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Linear blend of two 0x00RRGGBB colors: t in [0,256], 0 -> a, 256 -> b. */
static uint32_t fb_blend_rgb(uint32_t a, uint32_t b, int32_t t) {
    if (t <= 0)   return a;
    if (t >= 256) return b;
    uint32_t inv = (uint32_t)(256 - t);
    uint32_t r = (((a >> 16) & 0xFF) * inv + ((b >> 16) & 0xFF) * (uint32_t)t) >> 8;
    uint32_t g = (((a >>  8) & 0xFF) * inv + ((b >>  8) & 0xFF) * (uint32_t)t) >> 8;
    uint32_t bl= ((( a       & 0xFF) * inv + ( b        & 0xFF) * (uint32_t)t)) >> 8;
    return (r << 16) | (g << 8) | bl;
}

/* Filled disc centered at (cx,cy), radius r, via per-row hlines (clipped). */
void framebuffer_fill_circle(int cx, int cy, int r, uint32_t color) {
    if (!fb_state.initialized || r <= 0) return;
    int32_t r2 = r * r;
    for (int32_t dy = -r; dy <= r; dy++) {
        int32_t y = cy + dy;
        if (y < 0) continue;
        if (y >= (int32_t)fb_state.height) break;
        int32_t half = (int32_t)fb_isqrt32((uint32_t)(r2 - dy * dy));
        int32_t x0 = cx - half;
        int32_t len = 2 * half + 1;
        if (x0 < 0) { len += x0; x0 = 0; }
        if (len <= 0) continue;
        framebuffer_draw_hline((uint32_t)x0, (uint32_t)y, (uint32_t)len, color);
    }
}

/* The "fluid circle": NDOTS filled discs orbiting (cx,cy) at radius R, driven by
 * one phase angle. The head (i=0) is full base_color + full size; trailing dots
 * fade toward the splash bg and taper, reading as a comet/spinner. */
#define FB_SPIN_BG    0x00101826u   /* must match the boot splash background */
#define FB_SPIN_DOTS  8
void framebuffer_draw_fluid_circle(int cx, int cy, int R, int dot_r,
                                   int phase_deg, uint32_t base_color) {
    for (int i = 0; i < FB_SPIN_DOTS; i++) {
        int32_t a  = phase_deg - i * (360 / FB_SPIN_DOTS);
        int32_t x  = cx + R * fb_cos_q(a) / 256;
        int32_t y  = cy + R * fb_sin_q(a) / 256;
        uint32_t col = fb_blend_rgb(base_color, FB_SPIN_BG, i * 256 / FB_SPIN_DOTS);
        int rr = dot_r - (i * dot_r) / (2 * FB_SPIN_DOTS);
        if (rr < 1) rr = 1;
        framebuffer_fill_circle(x, y, rr, col);
    }
}

/* Bounded boot loading spinner: an rdtsc-timed ~60fps loop drawing the fluid
 * circle BELOW the splash title for `duration_ms`. Boot is single-threaded with
 * IRQs off here (pre-scheduler, pre-PIT), so rdtsc is the only clock -- same
 * ~3GHz convention as the kernel's SMP heartbeat window. Erases only a small box
 * around the spinner each frame (NOT a full clear -- full clears are slow on
 * UC-mapped framebuffers) and never touches the splash text above it. */
void framebuffer_boot_spinner(uint32_t duration_ms) {
    if (!fb_state.initialized) return;
    const uint64_t TSC_PER_US = 3000ULL;            /* ~3 GHz, matches kernel.c */
    const uint64_t FRAME_TSC  = 16ULL * 1000ULL * TSC_PER_US;   /* ~16 ms */
    int cx  = (int)fb_state.width / 2;
    int cy  = (int)fb_state.height / 2 + 90;        /* below the splash title */
    int R   = 26, dot_r = 6;
    int box = R + dot_r + 4;
    uint64_t start = fb_rdtsc();
    uint64_t total = (uint64_t)duration_ms * 1000ULL * TSC_PER_US;
    uint64_t next_frame = 0;
    while ((fb_rdtsc() - start) < total) {
        uint64_t now = fb_rdtsc() - start;
        if (now < next_frame) continue;
        next_frame = now + FRAME_TSC;
        int phase = (int)((now / TSC_PER_US / 1500ULL) % 360ULL);  /* ~1.8 rot/s */
        framebuffer_draw_rect((uint32_t)(cx - box), (uint32_t)(cy - box),
                              (uint32_t)(2 * box), (uint32_t)(2 * box), FB_SPIN_BG);
        framebuffer_draw_fluid_circle(cx, cy, R, dot_r, phase, 0x009FC8FFu);
    }
}
