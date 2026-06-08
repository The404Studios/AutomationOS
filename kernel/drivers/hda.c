#include "../include/hda.h"
#include "../include/pci.h"
#include "../include/mem.h"
#include "../include/x86_64.h"
#include "../include/types.h"
#include "../include/drivers.h"   /* serial_write, serial_putchar, timer_get_ticks, timer_get_frequency */
#include "../include/syscall.h"   /* sys_yield */

// Global HDA controller instance
static hda_controller_t* g_hda_ctrl = NULL;

// Helper function prototypes
// NOTE: hda_msleep is intentionally NON-static (external linkage): hda_stream.c
// and audio/audio_tone.c reference it via `extern void hda_msleep(uint32_t)`.
// It is also declared in hda.h.
static uint32_t hda_build_verb(uint8_t codec_addr, uint8_t nid, uint32_t verb, uint32_t param);
static int hda_wait_for_response(hda_controller_t* ctrl, uint32_t* response);

/**
 * Simple millisecond delay (busy wait)
 */
void hda_msleep(uint32_t ms) {
    // Assuming 1000 Hz timer, convert to ticks
    uint64_t start = timer_get_ticks();
    uint64_t end = start + (ms * timer_get_frequency() / 1000);
    /* Frozen-tick backstop: SYS_BEEP runs in syscall context with IF=0, so the
     * PIT (IRQ0) cannot advance timer_get_ticks() while THIS task holds the CPU.
     * The yield lets other IF=1 tasks run (IRQ0 ticks during them), but if the
     * system is otherwise idle the tick may never advance -- so cap the spin so
     * audio can NEVER hang the caller. Same rule as net resolve_mac():
     * "any wall-clock wait inside a syscall MUST also have an iteration cap." */
    uint32_t iters = 0;
    const uint32_t iter_cap = 4000000u;
    while (timer_get_ticks() < end) {
        sys_yield(0, 0, 0, 0, 0, 0);  // Scheduler-aware sleep
        if (++iters >= iter_cap) break;
    }
}

/**
 * Build a verb command for CORB.
 *
 * Intel HDA spec §7.3.3 defines two verb encodings:
 *
 *   12-bit verbs (e.g. GET_PARAMETER 0xF00, SET_PIN_CTRL 0x707):
 *     bits[31:28] = codec address
 *     bits[27:20] = NID
 *     bits[19:8]  = 12-bit verb
 *     bits[7:0]   = 8-bit parameter
 *
 *   4-bit verbs (e.g. SET_AMP_GAIN_MUTE 0x3, SET_CONVERTER_FORMAT 0x2):
 *     bits[31:28] = codec address
 *     bits[27:20] = NID
 *     bits[19:16] = 4-bit verb
 *     bits[15:0]  = 16-bit payload
 *
 * We distinguish them by whether the verb fits in 4 bits (verb <= 0xF and
 * the caller signals this by passing verb as 0x3 or 0x2 directly), OR by
 * whether the verb >= 0x100 (12-bit range).  Callers that need a 4-bit verb
 * should pass verb in [0x0..0xF]; callers that need a 12-bit verb pass it
 * in [0x100..0xFFF].  The param field is always 8-bit for 12-bit verbs and
 * the FULL 16-bit value for 4-bit verbs (passed via the param argument).
 *
 * For 4-bit verbs use hda_build_verb4() below instead.
 */
static uint32_t hda_build_verb(uint8_t codec_addr, uint8_t nid, uint32_t verb, uint32_t param) {
    return ((uint32_t)codec_addr << 28) |
           ((uint32_t)nid << 20) |
           ((verb & 0xFFF) << 8) |
           (param & 0xFF);
}

/**
 * Build a 4-bit verb command (SET_AMP_GAIN_MUTE, SET_CONVERTER_FORMAT, etc.)
 * The payload is a full 16-bit value.
 */
static uint32_t hda_build_verb4(uint8_t codec_addr, uint8_t nid, uint8_t verb4, uint16_t payload16) {
    return ((uint32_t)codec_addr << 28) |
           ((uint32_t)nid << 20) |
           ((uint32_t)(verb4 & 0xF) << 16) |
           (uint32_t)payload16;
}

/*
 * hda_send_verb4 forward declaration (non-static: also used by hda_stream.c).
 * Full implementation is after hda_send_command().
 */
uint32_t hda_send_verb4(hda_controller_t* ctrl, uint8_t codec_addr,
                         uint8_t nid, uint8_t verb4, uint16_t payload16);

/**
 * Initialize HDA subsystem
 */
void hda_init(void) {
    // Find HDA controller via PCI
    pci_device_t* pci_dev = pci_find_class(PCI_CLASS_MULTIMEDIA,
                                           PCI_SUBCLASS_AUDIO,
                                           PCI_PROG_IF_HDA);

    if (!pci_dev) {
        serial_write("HDA: No HD Audio controller found\n", 37);
        return;
    }

    serial_write("HDA: Found HD Audio controller\n", 32);

    // Allocate controller structure
    g_hda_ctrl = (hda_controller_t*)kmalloc(sizeof(hda_controller_t));
    if (!g_hda_ctrl) {
        serial_write("HDA: Failed to allocate controller structure\n", 46);
        return;
    }

    // Clear structure
    for (uint32_t i = 0; i < sizeof(hda_controller_t); i++) {
        ((uint8_t*)g_hda_ctrl)[i] = 0;
    }

    g_hda_ctrl->pci_dev = pci_dev;
    g_hda_ctrl->irq_line = pci_dev->interrupt_line;

    // Enable PCI bus mastering and memory space
    pci_enable_bus_master(pci_dev);
    pci_enable_memory_space(pci_dev);

    // Map BAR0 (MMIO)
    uint64_t bar0 = pci_get_bar(pci_dev, 0);
    g_hda_ctrl->mmio_base = (void*)bar0;

    serial_write("HDA: MMIO base at ", 18);
    // Simple hex output
    serial_putchar('0');
    serial_putchar('x');
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (bar0 >> i) & 0xF;
        serial_putchar(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
    }
    serial_putchar('\n');

    // Reset controller
    if (hda_reset_controller(g_hda_ctrl) != 0) {
        serial_write("HDA: Failed to reset controller\n", 33);
        kfree(g_hda_ctrl);
        g_hda_ctrl = NULL;
        return;
    }

    // Read controller capabilities
    g_hda_ctrl->gcap = hda_read16(g_hda_ctrl, HDA_REG_GCAP);
    g_hda_ctrl->vmaj = hda_read8(g_hda_ctrl, HDA_REG_VMAJ);
    g_hda_ctrl->vmin = hda_read8(g_hda_ctrl, HDA_REG_VMIN);

    g_hda_ctrl->num_oss = (g_hda_ctrl->gcap >> 12) & 0xF;  // Output streams
    g_hda_ctrl->num_iss = (g_hda_ctrl->gcap >> 8) & 0xF;   // Input streams
    g_hda_ctrl->num_bss = (g_hda_ctrl->gcap >> 3) & 0x1F;  // Bidirectional streams

    serial_write("HDA: Version ", 13);
    serial_putchar('0' + g_hda_ctrl->vmaj);
    serial_putchar('.');
    serial_putchar('0' + g_hda_ctrl->vmin);
    serial_putchar('\n');

    serial_write("HDA: Streams - Output: ", 23);
    serial_putchar('0' + g_hda_ctrl->num_oss);
    serial_write(", Input: ", 9);
    serial_putchar('0' + g_hda_ctrl->num_iss);
    serial_putchar('\n');

    // Initialize CORB/RIRB
    if (hda_init_corb_rirb(g_hda_ctrl) != 0) {
        serial_write("HDA: Failed to initialize CORB/RIRB\n", 37);
        kfree(g_hda_ctrl);
        g_hda_ctrl = NULL;
        return;
    }

    /*
     * Enable global interrupts + all stream interrupt bits so that the RIRB
     * DMA engine and stream DMA can fire.  Bit 31 = GIE (global interrupt
     * enable), bits [29:0] = per-stream enables.  We don't rely on IRQs for
     * our busy-wait playback path, but QEMU's HDA model may require GIE=1
     * before the stream DMA engine will actually push data.
     */
    hda_write32(g_hda_ctrl, HDA_REG_INTCTL, 0x800000FF);

    // Enumerate codecs
    if (hda_enumerate_codecs(g_hda_ctrl) != 0) {
        serial_write("HDA: Failed to enumerate codecs\n", 33);
        kfree(g_hda_ctrl);
        g_hda_ctrl = NULL;
        return;
    }

    serial_write("HDA: Initialization complete\n", 30);
}

/**
 * Reset the HDA controller.
 *
 * Sequence per Intel HDA spec §3.3.7:
 *   1. Disable wake-up events and clear STATESTS so stale codec-present bits
 *      don't confuse enumeration.
 *   2. Set GCTL.CRST = 0 to assert controller reset.
 *   3. Poll until hardware acknowledges (CRST reads back 0).
 *   4. Wait at least 100 µs (we do 10 ms to be generous and let codec
 *      oscillators settle).
 *   5. Set GCTL.CRST = 1 to de-assert reset.
 *   6. Poll until hardware is ready (CRST reads back 1).
 *   7. Wait ≥ 521 µs for codec state-change interrupts (we use 100 ms so
 *      QEMU's emulated codec has time to assert STATESTS).
 */
int hda_reset_controller(hda_controller_t* ctrl) {
    serial_write("HDA: Resetting controller\n", 27);

    /* Disable wake-up interrupts and clear any stale STATESTS bits so we
     * do not pick up phantom codec-present signals from a previous boot. */
    hda_write16(ctrl, HDA_REG_WAKEEN, 0);
    hda_write16(ctrl, HDA_REG_STATESTS, 0x7FFF);  /* W1C: clear all 15 bits */

    /* Stop CORB/RIRB before resetting (spec says stop them first). */
    hda_write8(ctrl, HDA_REG_CORBCTL, 0);
    hda_write8(ctrl, HDA_REG_RIRBCTL, 0);
    hda_msleep(1);

    // Clear CRST bit to enter reset
    uint32_t gctl = hda_read32(ctrl, HDA_REG_GCTL);
    hda_write32(ctrl, HDA_REG_GCTL, gctl & ~HDA_GCTL_CRST);

    // Wait for controller to acknowledge reset (CRST reads 0)
    int timeout = 1000;
    while (timeout > 0) {
        if (!(hda_read32(ctrl, HDA_REG_GCTL) & HDA_GCTL_CRST)) {
            break;
        }
        hda_msleep(1);
        timeout--;
    }

    if (timeout == 0) {
        serial_write("HDA: Reset timeout (entering reset)\n", 37);
        return -1;
    }

    /* Hold reset for ≥100 µs; 10 ms is conservative. */
    hda_msleep(10);

    // Set CRST bit to exit reset
    gctl = hda_read32(ctrl, HDA_REG_GCTL);
    hda_write32(ctrl, HDA_REG_GCTL, gctl | HDA_GCTL_CRST);

    // Wait for controller to be ready (CRST reads 1)
    timeout = 1000;
    while (timeout > 0) {
        if (hda_read32(ctrl, HDA_REG_GCTL) & HDA_GCTL_CRST) {
            break;
        }
        hda_msleep(1);
        timeout--;
    }

    if (timeout == 0) {
        serial_write("HDA: Reset timeout (exiting reset)\n", 36);
        return -1;
    }

    /*
     * Wait ≥ 521 µs for codecs to present on the link and assert
     * STATESTS bits.  QEMU's HDA emulation needs ~1 ms; use 100 ms
     * to be safe on real hardware too.
     */
    hda_msleep(100);

    /* Re-enable wake-up for all 15 codec slots so STATESTS gets populated. */
    hda_write16(ctrl, HDA_REG_WAKEEN, 0x7FFF);

    serial_write("HDA: Controller reset complete\n", 32);
    return 0;
}

/**
 * Initialize CORB and RIRB (Command/Response ring buffers)
 */
int hda_init_corb_rirb(hda_controller_t* ctrl) {
    serial_write("HDA: Initializing CORB/RIRB\n", 29);

    // Stop CORB
    hda_write8(ctrl, HDA_REG_CORBCTL, 0);

    // Wait for CORB to stop
    int timeout = 100;
    while (timeout > 0) {
        if (!(hda_read8(ctrl, HDA_REG_CORBCTL) & HDA_CORBCTL_RUN)) {
            break;
        }
        hda_msleep(1);
        timeout--;
    }

    // Allocate CORB buffer (256 entries * 4 bytes = 1KB)
    void* corb_page = pmm_alloc_page();
    if (!corb_page) {
        serial_write("HDA: Failed to allocate CORB buffer\n", 37);
        return -1;
    }

    ctrl->corb_phys = corb_page;
    ctrl->corb_virt = (hda_corb_entry_t*)corb_page;
    ctrl->corb_wp = 0;

    // Clear CORB buffer
    for (uint32_t i = 0; i < HDA_CORB_ENTRIES; i++) {
        ctrl->corb_virt[i].data = 0;
    }

    // Set CORB base address
    uint64_t corb_phys_addr = (uint64_t)corb_page;
    hda_write32(ctrl, HDA_REG_CORBLBASE, (uint32_t)corb_phys_addr);
    hda_write32(ctrl, HDA_REG_CORBUBASE, (uint32_t)(corb_phys_addr >> 32));

    // Set CORB size to 256 entries (size code 0x02)
    hda_write8(ctrl, HDA_REG_CORBSIZE, 0x02);

    // Reset CORB read/write pointers.
    // Spec §3.3.21: write CORBRPRST=1, poll until CORBRPRST reads back 1,
    // then write CORBRPRST=0 and poll until it reads back 0.
    hda_write16(ctrl, HDA_REG_CORBWP, 0);
    hda_write16(ctrl, HDA_REG_CORBRP, 0x8000);  // Set CORBRPRST bit
    {
        int rp_timeout = 100;
        while (rp_timeout > 0) {
            if (hda_read16(ctrl, HDA_REG_CORBRP) & 0x8000) break;
            hda_msleep(1);
            rp_timeout--;
        }
    }
    hda_write16(ctrl, HDA_REG_CORBRP, 0);       // Clear CORBRPRST bit
    {
        int rp_timeout = 100;
        while (rp_timeout > 0) {
            if (!(hda_read16(ctrl, HDA_REG_CORBRP) & 0x8000)) break;
            hda_msleep(1);
            rp_timeout--;
        }
    }

    // Start CORB
    hda_write8(ctrl, HDA_REG_CORBCTL, HDA_CORBCTL_RUN);

    // Stop RIRB
    hda_write8(ctrl, HDA_REG_RIRBCTL, 0);

    timeout = 100;
    while (timeout > 0) {
        if (!(hda_read8(ctrl, HDA_REG_RIRBCTL) & HDA_RIRBCTL_DMA_EN)) {
            break;
        }
        hda_msleep(1);
        timeout--;
    }

    // Allocate RIRB buffer (256 entries * 8 bytes = 2KB)
    void* rirb_page = pmm_alloc_page();
    if (!rirb_page) {
        serial_write("HDA: Failed to allocate RIRB buffer\n", 37);
        pmm_free_page(corb_page);
        return -1;
    }

    ctrl->rirb_phys = rirb_page;
    ctrl->rirb_virt = (hda_rirb_entry_t*)rirb_page;
    ctrl->rirb_rp = 0;

    // Clear RIRB buffer
    for (uint32_t i = 0; i < HDA_RIRB_ENTRIES; i++) {
        ctrl->rirb_virt[i].response = 0;
        ctrl->rirb_virt[i].response_ex = 0;
    }

    // Set RIRB base address
    uint64_t rirb_phys_addr = (uint64_t)rirb_page;
    hda_write32(ctrl, HDA_REG_RIRBLBASE, (uint32_t)rirb_phys_addr);
    hda_write32(ctrl, HDA_REG_RIRBUBASE, (uint32_t)(rirb_phys_addr >> 32));

    // Set RIRB size to 256 entries (size code 0x02)
    hda_write8(ctrl, HDA_REG_RIRBSIZE, 0x02);

    // Reset RIRB write pointer
    hda_write16(ctrl, HDA_REG_RIRBWP, 0x8000);  // Set reset bit

    // Start RIRB
    hda_write8(ctrl, HDA_REG_RIRBCTL, HDA_RIRBCTL_DMA_EN);

    serial_write("HDA: CORB/RIRB initialized\n", 28);
    return 0;
}

/**
 * Wait for a response from RIRB
 */
static int hda_wait_for_response(hda_controller_t* ctrl, uint32_t* response) {
    int timeout = 1000;  // 1 second timeout

    while (timeout > 0) {
        uint16_t wp = hda_read16(ctrl, HDA_REG_RIRBWP);

        // Check if there's a new response
        if (wp != ctrl->rirb_rp) {
            // Read response
            ctrl->rirb_rp = (ctrl->rirb_rp + 1) % HDA_RIRB_ENTRIES;
            *response = ctrl->rirb_virt[ctrl->rirb_rp].response;
            return 0;
        }

        hda_msleep(1);
        timeout--;
    }

    serial_write("HDA: Response timeout\n", 23);
    return -1;
}

/**
 * Send a verb command to a codec and wait for response.
 * Use this for 12-bit verbs (verb in [0x100..0xFFF], param is 8-bit).
 */
uint32_t hda_send_command(hda_controller_t* ctrl, uint8_t codec_addr,
                          uint8_t nid, uint32_t verb, uint32_t param) {
    // Build verb command
    uint32_t cmd = hda_build_verb(codec_addr, nid, verb, param);

    // Write to CORB
    ctrl->corb_wp = (ctrl->corb_wp + 1) % HDA_CORB_ENTRIES;
    ctrl->corb_virt[ctrl->corb_wp].data = cmd;

    /*
     * Memory barrier before updating CORBWP.
     *
     * The CORB DMA engine (inside the HDA controller) begins fetching the
     * new command as soon as CORBWP is written.  We must guarantee that the
     * verb write to corb_virt[] (normal WB memory) is visible to the
     * controller's DMA before it sees the new write-pointer value in MMIO.
     * On x86 TSO the CPU won't reorder store→store within the same address
     * type, but WB→UC stores can be reordered on some implementations without
     * an explicit store fence (SFENCE / MFENCE).  The asm volatile("" :::
     * "memory") compiler barrier is always sufficient to prevent the
     * compiler from sinking the store past the MMIO write; SFENCE closes
     * the hardware reorder window.
     */
    asm volatile("sfence" ::: "memory");

    // Update CORB write pointer register
    hda_write16(ctrl, HDA_REG_CORBWP, ctrl->corb_wp);

    // Wait for response
    uint32_t response = 0;
    if (hda_wait_for_response(ctrl, &response) != 0) {
        return 0xFFFFFFFF;  // Error response
    }

    return response;
}

/**
 * Send a 4-bit verb with a 16-bit payload (SET_AMP_GAIN_MUTE, SET_CONVERTER_FORMAT).
 *
 * 4-bit verb format (Intel HDA spec §7.3.3.7):
 *   bits[31:28] = codec address
 *   bits[27:20] = NID
 *   bits[19:16] = 4-bit verb
 *   bits[15:0]  = 16-bit payload
 */
uint32_t hda_send_verb4(hda_controller_t* ctrl, uint8_t codec_addr,
                        uint8_t nid, uint8_t verb4, uint16_t payload16) {
    uint32_t cmd = hda_build_verb4(codec_addr, nid, verb4, payload16);

    ctrl->corb_wp = (ctrl->corb_wp + 1) % HDA_CORB_ENTRIES;
    ctrl->corb_virt[ctrl->corb_wp].data = cmd;
    asm volatile("sfence" ::: "memory");  /* flush verb write before CORBWP MMIO update */
    hda_write16(ctrl, HDA_REG_CORBWP, ctrl->corb_wp);

    uint32_t response = 0;
    if (hda_wait_for_response(ctrl, &response) != 0) {
        return 0xFFFFFFFF;
    }
    return response;
}

/**
 * Enumerate and initialize codecs
 */
int hda_enumerate_codecs(hda_controller_t* ctrl) {
    serial_write("HDA: Enumerating codecs\n", 25);

    // Read STATESTS register to see which codecs are present
    uint16_t statests = hda_read16(ctrl, HDA_REG_STATESTS);

    serial_write("HDA: STATESTS = 0x", 18);
    for (int i = 12; i >= 0; i -= 4) {
        uint8_t nibble = (statests >> i) & 0xF;
        serial_putchar(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
    }
    serial_putchar('\n');

    if (statests == 0) {
        serial_write("HDA: No codecs found\n", 22);
        return -1;
    }

    // Enumerate each codec
    ctrl->num_codecs = 0;
    for (uint8_t addr = 0; addr < HDA_MAX_CODECS; addr++) {
        if (!(statests & (1 << addr))) {
            continue;  // Codec not present
        }

        serial_write("HDA: Found codec at address ", 29);
        serial_putchar('0' + addr);
        serial_putchar('\n');

        // Allocate codec structure
        hda_codec_t* codec = (hda_codec_t*)kmalloc(sizeof(hda_codec_t));
        if (!codec) {
            serial_write("HDA: Failed to allocate codec structure\n", 41);
            continue;
        }

        // Clear structure
        for (uint32_t i = 0; i < sizeof(hda_codec_t); i++) {
            ((uint8_t*)codec)[i] = 0;
        }

        codec->addr = addr;

        // Get vendor ID (root node, NID=0)
        codec->vendor_id = hda_send_command(ctrl, addr, 0, HDA_VERB_GET_PARAMETER, HDA_PARAM_VENDOR_ID);
        codec->revision_id = hda_send_command(ctrl, addr, 0, HDA_VERB_GET_PARAMETER, HDA_PARAM_REVISION_ID);

        serial_write("HDA: Vendor ID = 0x", 19);
        for (int i = 28; i >= 0; i -= 4) {
            uint8_t nibble = (codec->vendor_id >> i) & 0xF;
            serial_putchar(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
        }
        serial_putchar('\n');

        // Find Audio Function Group (AFG)
        uint32_t sub_node_count = hda_send_command(ctrl, addr, 0, HDA_VERB_GET_PARAMETER, HDA_PARAM_SUB_NODE_COUNT);
        uint8_t start_nid = (sub_node_count >> 16) & 0xFF;
        uint8_t num_nodes = sub_node_count & 0xFF;

        serial_write("HDA: Sub-nodes: start=", 22);
        serial_putchar('0' + start_nid);
        serial_write(", count=", 8);
        serial_putchar('0' + num_nodes);
        serial_putchar('\n');

        // Search for AFG
        codec->afg_nid = 0;
        for (uint8_t nid = start_nid; nid < start_nid + num_nodes; nid++) {
            uint32_t func_type = hda_send_command(ctrl, addr, nid, HDA_VERB_GET_PARAMETER, HDA_PARAM_FUNC_GROUP_TYPE);
            if ((func_type & 0xFF) == 0x01) {  // Audio Function Group
                codec->afg_nid = nid;
                serial_write("HDA: Found AFG at NID ", 23);
                serial_putchar('0' + nid);
                serial_putchar('\n');
                break;
            }
        }

        if (codec->afg_nid == 0) {
            serial_write("HDA: No AFG found for codec\n", 29);
            kfree(codec);
            continue;
        }

        // Read AFG widgets
        if (hda_codec_read_widgets(codec, ctrl) != 0) {
            serial_write("HDA: Failed to read codec widgets\n", 35);
            kfree(codec);
            continue;
        }

        // Setup output path
        if (hda_codec_setup_output(codec, ctrl) != 0) {
            serial_write("HDA: Failed to setup codec output\n", 35);
        }

        ctrl->codecs[ctrl->num_codecs++] = codec;
    }

    if (ctrl->num_codecs == 0) {
        serial_write("HDA: No usable codecs found\n", 29);
        return -1;
    }

    serial_write("HDA: Found ", 11);
    serial_putchar('0' + ctrl->num_codecs);
    serial_write(" codec(s)\n", 10);

    return 0;
}

/**
 * Read codec widgets (audio nodes)
 */
int hda_codec_read_widgets(hda_codec_t* codec, hda_controller_t* ctrl) {
    // Get AFG sub-nodes
    uint32_t sub_node_count = hda_send_command(ctrl, codec->addr, codec->afg_nid,
                                                HDA_VERB_GET_PARAMETER, HDA_PARAM_SUB_NODE_COUNT);

    codec->afg_start_nid = (sub_node_count >> 16) & 0xFF;
    codec->afg_num_nodes = sub_node_count & 0xFF;

    serial_write("HDA: AFG widgets: start=", 24);
    serial_putchar('0' + codec->afg_start_nid);
    serial_write(", count=", 8);
    serial_putchar('0' + codec->afg_num_nodes);
    serial_putchar('\n');

    // Read each widget
    codec->num_widgets = 0;
    for (uint8_t nid = codec->afg_start_nid;
         nid < codec->afg_start_nid + codec->afg_num_nodes && codec->num_widgets < HDA_MAX_WIDGETS;
         nid++) {

        hda_widget_t* widget = &codec->widgets[codec->num_widgets];
        widget->nid = nid;

        // Get widget capabilities
        widget->capabilities = hda_send_command(ctrl, codec->addr, nid,
                                                HDA_VERB_GET_PARAMETER, HDA_PARAM_AUDIO_WIDGET_CAP);

        // Extract widget type
        widget->type = (widget->capabilities >> 20) & 0xF;

        // Get connection list length
        uint32_t conn_list_len = hda_send_command(ctrl, codec->addr, nid,
                                                   HDA_VERB_GET_PARAMETER, HDA_PARAM_CONN_LIST_LEN);
        widget->conn_list_len = conn_list_len & 0x7F;

        // Read connection list (if any)
        if (widget->conn_list_len > 0 && widget->conn_list_len < HDA_MAX_WIDGETS) {
            for (uint8_t i = 0; i < widget->conn_list_len; i += 4) {
                uint32_t conn_list = hda_send_command(ctrl, codec->addr, nid,
                                                      HDA_VERB_GET_CONN_LIST, i);
                for (uint8_t j = 0; j < 4 && (i + j) < widget->conn_list_len; j++) {
                    widget->conn_list[i + j] = (conn_list >> (j * 8)) & 0xFF;
                }
            }
        }

        // For pin widgets, get additional info
        if (widget->type == HDA_WIDGET_PIN_COMPLEX) {
            widget->pin_caps = hda_send_command(ctrl, codec->addr, nid,
                                                HDA_VERB_GET_PARAMETER, HDA_PARAM_PIN_CAP);
            widget->config_default = hda_send_command(ctrl, codec->addr, nid,
                                                      HDA_VERB_GET_CONFIG_DEFAULT, 0);
        }

        codec->num_widgets++;
    }

    serial_write("HDA: Read ", 10);
    serial_putchar('0' + codec->num_widgets);
    serial_write(" widgets\n", 9);

    return 0;
}

/**
 * Setup codec output path (DAC -> Pin)
 */
int hda_codec_setup_output(hda_codec_t* codec, hda_controller_t* ctrl) {
    serial_write("HDA: Setting up codec output\n", 30);

    // Find DAC (Digital-to-Analog Converter)
    codec->dac_nid = 0;
    for (uint8_t i = 0; i < codec->num_widgets; i++) {
        if (codec->widgets[i].type == HDA_WIDGET_AUDIO_OUTPUT) {
            codec->dac_nid = codec->widgets[i].nid;
            serial_write("HDA: Found DAC at NID ", 23);
            serial_putchar('0' + codec->dac_nid);
            serial_putchar('\n');
            break;
        }
    }

    if (codec->dac_nid == 0) {
        serial_write("HDA: No DAC found\n", 18);
        return -1;
    }

    // Find output pin
    codec->pin_nid = 0;
    for (uint8_t i = 0; i < codec->num_widgets; i++) {
        hda_widget_t* widget = &codec->widgets[i];
        if (widget->type == HDA_WIDGET_PIN_COMPLEX) {
            // Check if it's an output pin (bit 4 of pin caps)
            if (widget->pin_caps & (1 << 4)) {
                codec->pin_nid = widget->nid;
                serial_write("HDA: Found output pin at NID ", 30);
                serial_putchar('0' + codec->pin_nid);
                serial_putchar('\n');
                break;
            }
        }
    }

    if (codec->pin_nid == 0) {
        serial_write("HDA: No output pin found\n", 26);
        return -1;
    }

    // Connect pin to DAC (if pin has connections)
    for (uint8_t i = 0; i < codec->num_widgets; i++) {
        if (codec->widgets[i].nid == codec->pin_nid) {
            if (codec->widgets[i].conn_list_len > 0) {
                // Find DAC in connection list
                for (uint8_t j = 0; j < codec->widgets[i].conn_list_len; j++) {
                    if (codec->widgets[i].conn_list[j] == codec->dac_nid) {
                        // Select this connection
                        hda_send_command(ctrl, codec->addr, codec->pin_nid,
                                       HDA_VERB_SET_CONN_SELECT, j);
                        serial_write("HDA: Connected pin to DAC\n", 27);
                        break;
                    }
                }
            }
            break;
        }
    }

    /*
     * Power up the AFG, DAC and pin to D0 BEFORE configuring amps.
     * D0 = fully powered; after reset nodes may be in D3 (off).
     */
    hda_send_command(ctrl, codec->addr, codec->afg_nid,
                    HDA_VERB_SET_POWER_STATE, 0);
    hda_msleep(10);  /* let nodes power up */
    hda_send_command(ctrl, codec->addr, codec->dac_nid,
                    HDA_VERB_SET_POWER_STATE, 0);
    hda_send_command(ctrl, codec->addr, codec->pin_nid,
                    HDA_VERB_SET_POWER_STATE, 0);
    hda_msleep(10);

    /*
     * Enable output on pin.
     * HDA_PIN_CTL_OUT_EN (0x40) = output enable.
     * HDA_PIN_CTL_HP_EN  (0x80) = headphone-amp enable.
     * QEMU's hda-output device responds to either; set both to be safe.
     */
    hda_send_command(ctrl, codec->addr, codec->pin_nid,
                    HDA_VERB_SET_PIN_CTRL,
                    HDA_PIN_CTL_OUT_EN | HDA_PIN_CTL_HP_EN);

    /*
     * Unmute and set gain on the DAC (Audio Output widget) output amp.
     *
     * SET_AMP_GAIN_MUTE is a 4-bit verb (0x3) with a 16-bit payload:
     *   bit 15: set OUTPUT amp
     *   bit 14: set INPUT amp  (0 here)
     *   bit 13: set LEFT channel
     *   bit 12: set RIGHT channel
     *   bit 7:  MUTE (0 = unmuted)
     *   bits[6:0]: gain (0x7F = maximum)
     *
     * We send: OUTPUT | LEFT | RIGHT | gain=0x57 (≈68%, loud but not max
     * to avoid clipping on QEMU's software mixer; set to 0x7F if too quiet).
     */
    {
        uint16_t amp_payload = (1<<15) | (1<<13) | (1<<12) | 0x57; /* unmuted, ~68% gain */
        hda_send_verb4(ctrl, codec->addr, codec->dac_nid, 0x3, amp_payload);
        serial_write("HDA: DAC amp unmuted (verb4 0x3, payload=0x", 44);
        serial_putchar('0' + ((amp_payload >> 12) & 0xF));
        serial_putchar('0' + ((amp_payload >> 8) & 0xF));
        serial_putchar('0' + ((amp_payload >> 4) & 0xF));
        serial_putchar('0' + (amp_payload & 0xF));
        serial_write(")\n", 2);
    }

    /*
     * Unmute and set gain on the PIN widget output amp.
     *
     * THIS IS THE #1 REASON QEMU HDA IS SILENT.
     * QEMU's codec (Realtek ALC887 emulation) has an OUTPUT amp on the pin
     * widget that defaults to MUTED after reset.  Sending exactly the same
     * verb as above — but targeting the pin NID — is mandatory for audible
     * output.
     *
     * Same 4-bit verb 0x3, same payload encoding.
     */
    {
        uint16_t pin_amp_payload = (1<<15) | (1<<13) | (1<<12) | 0x57;
        hda_send_verb4(ctrl, codec->addr, codec->pin_nid, 0x3, pin_amp_payload);
        serial_write("HDA: Pin amp unmuted (verb4 0x3, payload=0x", 44);
        serial_putchar('0' + ((pin_amp_payload >> 12) & 0xF));
        serial_putchar('0' + ((pin_amp_payload >> 8) & 0xF));
        serial_putchar('0' + ((pin_amp_payload >> 4) & 0xF));
        serial_putchar('0' + (pin_amp_payload & 0xF));
        serial_write(")\n", 2);
    }

    /*
     * EAPD (External Amplifier Power Down) enable — required on many codecs
     * (including QEMU's ALC887 emulation) to activate the speaker amp.
     * Verb 0x70C (SET_EAPD_BTLENABLE), bit 1 = EAPD enable, bit 2 = LR-swap.
     */
    hda_send_command(ctrl, codec->addr, codec->pin_nid,
                    HDA_VERB_SET_EAPD_ENABLE, 0x02);

    serial_write("HDA: Output path configured\n", 29);
    return 0;
}

/**
 * Setup codec input path (Pin -> ADC)
 */
int hda_codec_setup_input(hda_codec_t* codec, hda_controller_t* ctrl) {
    // Find ADC
    codec->adc_nid = 0;
    for (uint8_t i = 0; i < codec->num_widgets; i++) {
        if (codec->widgets[i].type == HDA_WIDGET_AUDIO_INPUT) {
            codec->adc_nid = codec->widgets[i].nid;
            break;
        }
    }

    if (codec->adc_nid == 0) {
        return -1;
    }

    // Find input pin
    codec->input_pin_nid = 0;
    for (uint8_t i = 0; i < codec->num_widgets; i++) {
        hda_widget_t* widget = &codec->widgets[i];
        if (widget->type == HDA_WIDGET_PIN_COMPLEX) {
            // Check if it's an input pin (bit 5 of pin caps)
            if (widget->pin_caps & (1 << 5)) {
                codec->input_pin_nid = widget->nid;
                break;
            }
        }
    }

    if (codec->input_pin_nid == 0) {
        return -1;
    }

    // Enable input on pin
    hda_send_command(ctrl, codec->addr, codec->input_pin_nid,
                    HDA_VERB_SET_PIN_CTRL, HDA_PIN_CTL_IN_EN);

    /*
     * Unmute the ADC INPUT amp using the 4-bit verb form (verb4 = 0x3,
     * HDA spec §7.3.3.7).
     *
     * The ADC (Audio Input widget) has an INPUT amp (not an OUTPUT amp).
     * The 16-bit payload must be:
     *   bit 15 = 0  (OUTPUT direction — NOT set for ADC input amp)
     *   bit 14 = 1  (INPUT direction  — set for ADC input amp)
     *   bit 13 = 1  (apply to LEFT channel)
     *   bit 12 = 1  (apply to RIGHT channel)
     *   bit 7  = 0  (MUTE = 0, i.e. unmuted)
     *   bits[6:0] = 0x7F (maximum gain)
     *
     * WRONG (previous code): hda_send_command(..., HDA_VERB_SET_AMP_GAIN_MUTE,
     *   amp_cmd) where HDA_VERB_SET_AMP_GAIN_MUTE = 0x300 (12-bit verb).
     * hda_send_command() calls hda_build_verb() which masks param to 8 bits
     * (param & 0xFF), silently discarding the upper byte of amp_cmd (which
     * carries the OUTPUT/INPUT/LEFT/RIGHT direction bits 15:8).  The result
     * is a verb that sets gain=0x7F but leaves the amp selector bits zero —
     * targeting no amp direction, producing no change on the hardware.
     *
     * CORRECT: use hda_send_verb4() with the 4-bit verb (0x3) and the full
     * 16-bit payload so direction bits 15:8 are preserved.
     */
    {
        uint16_t adc_amp_payload = (uint16_t)((1<<14) | (1<<13) | (1<<12) | 0x7F);
        hda_send_verb4(ctrl, codec->addr, codec->adc_nid,
                       HDA_VERB4_SET_AMP_GAIN_MUTE, adc_amp_payload);
    }

    return 0;
}

/**
 * Find HDA controller (returns global instance)
 */
hda_controller_t* hda_find_controller(void) {
    return g_hda_ctrl;
}
