/*
 * UHCI Host Controller Driver (USB 1.1, Universal Host Controller Interface)
 * ==========================================================================
 *
 * Brings up a real UHCI controller, enumerates the root hub, and attaches
 * HID keyboards/mice to the kernel input subsystem.
 *
 * Why UHCI? It is the simplest USB host controller to bring up from scratch:
 *   - All registers are in PCI I/O space (BAR4) -- no MMIO mapping needed.
 *   - DMA structures (frame list, Queue Heads, Transfer Descriptors) use
 *     32-bit physical pointers. This kernel identity-maps the low 1GB and
 *     pmm_alloc_page() returns identity-mapped low physical pages, so a PMM
 *     pointer IS its own physical address (and is < 4GB) -- ideal for UHCI.
 *   - QEMU's default PC ("pc"/i440fx) exposes a PIIX3 UHCI companion at
 *     00:01.2 (PCI 8086:7020), and `-device usb-kbd`/`-device usb-mouse`
 *     (or legacy `-usbdevice keyboard`) attach to it.
 *
 * This driver is POLL-DRIVEN: control transfers and interrupt-IN polling are
 * done synchronously by walking the controller's status bits / TD status.
 * It does not register a USB IRQ (UHCI IRQ is wired but masked); the
 * integrator calls uhci_poll() periodically (timer tick / idle loop).
 *
 * Scope: kernel/drivers/usb/* and kernel/include/usb.h only. The init call,
 * the periodic uhci_poll() hook, and the QEMU device flags are integrator
 * wiring -- see the report at the bottom of this comment is intentionally
 * brief; full wiring notes are in the agent report.
 */

#include "../../include/usb.h"
#include "../../include/input.h"
#include "../../include/kernel.h"
#include "../../include/types.h"
#include "../../include/mem.h"
#include "../../include/pci.h"
#include "../../include/x86_64.h"
#include <string.h>

/* ------------------------------------------------------------------ *
 * UHCI register layout (offsets from the I/O BAR base).
 * ------------------------------------------------------------------ */
#define UHCI_USBCMD       0x00  /* command (16-bit)                    */
#define UHCI_USBSTS       0x02  /* status  (16-bit)                    */
#define UHCI_USBINTR      0x04  /* interrupt enable (16-bit)           */
#define UHCI_FRNUM        0x06  /* frame number (16-bit)               */
#define UHCI_FRBASEADD    0x08  /* frame list base address (32-bit)    */
#define UHCI_SOFMOD       0x0C  /* start-of-frame modify (8-bit)       */
#define UHCI_PORTSC1      0x10  /* port 1 status/control (16-bit)      */
#define UHCI_PORTSC2      0x12  /* port 2 status/control (16-bit)      */

/* USBCMD bits */
#define UHCI_CMD_RS       0x0001  /* Run/Stop                          */
#define UHCI_CMD_HCRESET  0x0002  /* Host Controller Reset             */
#define UHCI_CMD_GRESET   0x0004  /* Global Reset                      */
#define UHCI_CMD_EGSM     0x0008  /* Enter Global Suspend              */
#define UHCI_CMD_FGR      0x0010  /* Force Global Resume               */
#define UHCI_CMD_MAXP     0x0080  /* Max packet (1 = 64 bytes)         */
#define UHCI_CMD_CF       0x0040  /* Configure Flag                    */

/* USBSTS bits */
#define UHCI_STS_USBINT   0x0001  /* USB interrupt (IOC / short pkt)   */
#define UHCI_STS_ERROR    0x0002  /* USB error interrupt               */
#define UHCI_STS_RD       0x0004  /* resume detect                     */
#define UHCI_STS_HSE      0x0008  /* host system error                 */
#define UHCI_STS_HCPE     0x0010  /* host controller process error     */
#define UHCI_STS_HCH      0x0020  /* HC halted                         */

/* PORTSC bits */
#define UHCI_PORT_CCS     0x0001  /* current connect status            */
#define UHCI_PORT_CSC     0x0002  /* connect status change (R/WC)      */
#define UHCI_PORT_PE      0x0004  /* port enabled                      */
#define UHCI_PORT_PEC     0x0008  /* port enable change (R/WC)         */
#define UHCI_PORT_LS      0x0030  /* line status (D+/D-)               */
#define UHCI_PORT_RD      0x0040  /* resume detect                     */
#define UHCI_PORT_LSDA    0x0100  /* low-speed device attached         */
#define UHCI_PORT_RESET   0x0200  /* port reset                        */
#define UHCI_PORT_SUSP    0x1000  /* suspend                           */

/* Bits in PORTSC that are write-clear; preserve them as 0 on writes so we
 * don't accidentally clear CSC/PEC when toggling other bits. */
#define UHCI_PORT_RWC     (UHCI_PORT_CSC | UHCI_PORT_PEC)

/* ------------------------------------------------------------------ *
 * UHCI schedule structures (must be 16-byte aligned, 32-bit phys ptrs).
 * ------------------------------------------------------------------ */

/* Link pointer flags (low 4 bits of FRBASEADD entries / TD/QH links). */
#define UHCI_PTR_TERMINATE  0x0001  /* T: link invalid                 */
#define UHCI_PTR_QH         0x0002  /* Q: points to a QH (else TD)      */
#define UHCI_PTR_DEPTH      0x0004  /* Vf: depth-first execution        */

/* Transfer Descriptor (32 bytes). */
typedef struct uhci_td {
    volatile uint32_t link;     /* link to next TD/QH                  */
    volatile uint32_t status;   /* ctrl/status: actlen, status bits    */
    volatile uint32_t token;    /* PID, device addr, endpoint, maxlen  */
    volatile uint32_t buffer;   /* data buffer physical address        */
    /* software-only fields below (controller ignores) */
    uint32_t sw[4];
} __attribute__((aligned(16))) uhci_td_t;

/* TD status field bits */
#define TD_STS_ACTLEN_MASK  0x000007FF  /* actual length (n-1 encoding) */
#define TD_CTRL_ACTIVE      0x00800000  /* set: HC will execute TD      */
#define TD_CTRL_STALLED     0x00400000
#define TD_CTRL_DBUFERR     0x00200000
#define TD_CTRL_BABBLE      0x00100000
#define TD_CTRL_NAK         0x00080000
#define TD_CTRL_CRCTO       0x00040000  /* CRC / timeout error          */
#define TD_CTRL_BITSTUFF    0x00020000
#define TD_CTRL_IOC         0x01000000  /* interrupt on complete        */
#define TD_CTRL_IOS         0x02000000  /* isochronous                  */
#define TD_CTRL_LS          0x04000000  /* low-speed device             */
#define TD_CTRL_C_ERR3      0x18000000  /* 3 error retries              */
#define TD_CTRL_SPD         0x20000000  /* short packet detect          */
#define TD_STS_ANY_ERROR    (TD_CTRL_STALLED | TD_CTRL_DBUFERR | TD_CTRL_BABBLE | \
                             TD_CTRL_CRCTO | TD_CTRL_BITSTUFF)

/* TD token field */
#define TD_PID_SETUP        0x2D
#define TD_PID_IN           0x69
#define TD_PID_OUT          0xE1
#define TD_TOKEN(pid, addr, ep, toggle, maxlen) \
    (((uint32_t)(pid) & 0xFF) | \
     (((uint32_t)(addr) & 0x7F) << 8) | \
     (((uint32_t)(ep) & 0x0F) << 15) | \
     (((uint32_t)(toggle) & 0x1) << 19) | \
     ((((uint32_t)(maxlen) - 1) & 0x7FF) << 21))
/* maxlen of 0 must encode as 0x7FF (length 0). */
#define TD_TOKEN_LEN0(pid, addr, ep, toggle) \
    (((uint32_t)(pid) & 0xFF) | \
     (((uint32_t)(addr) & 0x7F) << 8) | \
     (((uint32_t)(ep) & 0x0F) << 15) | \
     (((uint32_t)(toggle) & 0x1) << 19) | \
     (0x7FFu << 21))

/* Queue Head (16 bytes, but we pad/align to 16). */
typedef struct uhci_qh {
    volatile uint32_t link;     /* horizontal link (to next QH)        */
    volatile uint32_t element;  /* vertical link (to first TD)         */
    uint32_t sw[2];             /* software padding                    */
} __attribute__((aligned(16))) uhci_qh_t;

/* ------------------------------------------------------------------ *
 * Driver state.
 * ------------------------------------------------------------------ */
#define UHCI_NUM_PORTS      2
#define UHCI_MAX_HID        4
/* Per-control-transfer completion timeout, in uhci_udelay-equivalent spins
 * (~1us each via inb(0x80), tick-INDEPENDENT so it bounds even with IF=0). A
 * healthy device completes a control transfer in microseconds; this is only the
 * stuck/absent-device cap. 2,000,000 (~2s) x the ~6 enumeration transfers was a
 * ~12s worst-case boot stall when USB is enabled; 250,000 (~250ms, still 250x the
 * normal completion) bounds it to ~1.5s -- USB-MOUSE-0 loop-law hardening. */
#define SETUP_TIMEOUT_SPINS 250000

typedef struct {
    usb_device_descriptor_t devdesc;
    uint8_t  addr;            /* assigned USB address                  */
    uint8_t  port;
    bool     low_speed;
    uint8_t  ep0_maxpkt;

    /* HID interrupt-IN endpoint */
    bool     is_hid;
    uint8_t  hid_proto;       /* 1 = keyboard, 2 = mouse               */
    uint8_t  in_ep;           /* endpoint address (0x81 etc.)          */
    uint8_t  in_maxpkt;
    uint8_t  in_toggle;
    uint8_t  config_value;

    /* poll state */
    uhci_qh_t* int_qh;        /* queue head for interrupt polling      */
    uhci_td_t* int_td;        /* the single interrupt-IN TD            */
    uint8_t*   in_buf;        /* DMA buffer for reports                */
    bool       td_armed;

    /* input integration */
    input_device_t* input_dev;
    uint8_t  prev_keys[6];
    uint8_t  prev_mods;
    uint8_t  prev_buttons;
} uhci_hid_t;

typedef struct {
    bool      present;
    uint16_t  io_base;
    pci_device_t* pci;

    uint32_t* frame_list;     /* 1024 entries (4KB, page-aligned)      */
    uhci_qh_t* ctrl_qh;       /* control queue head (frame[*] -> here) */
    uhci_td_t* td_pool;       /* small pool of TDs for control xfers   */
    uint8_t*   setup_buf;     /* 8-byte SETUP packet DMA buffer        */
    uint8_t*   data_buf;      /* control transfer data DMA buffer      */

    uhci_hid_t hid[UHCI_MAX_HID];
    uint32_t   num_hid;
    uint8_t    next_addr;
} uhci_t;

static uhci_t g_uhci;

/* HID boot-protocol USB usage -> Linux keycode (same table layout as hid.c). */
static const uint16_t uhci_kbd_keycode[256] = {
    [0]=KEY_RESERVED,[1]=KEY_ESC,[2]=KEY_1,[3]=KEY_2,[4]=KEY_3,[5]=KEY_4,
    [6]=KEY_5,[7]=KEY_6,[8]=KEY_7,[9]=KEY_8,[10]=KEY_9,[11]=KEY_0,
    [12]=KEY_MINUS,[13]=KEY_EQUAL,[14]=KEY_BACKSPACE,[15]=KEY_TAB,
    [16]=KEY_Q,[17]=KEY_W,[18]=KEY_E,[19]=KEY_R,[20]=KEY_T,[21]=KEY_Y,
    [22]=KEY_U,[23]=KEY_I,[24]=KEY_O,[25]=KEY_P,[26]=KEY_LEFTBRACE,
    [27]=KEY_RIGHTBRACE,[28]=KEY_ENTER,[29]=KEY_LEFTCTRL,[30]=KEY_A,
    [31]=KEY_S,[32]=KEY_D,[33]=KEY_F,[34]=KEY_G,[35]=KEY_H,[36]=KEY_J,
    [37]=KEY_K,[38]=KEY_L,[39]=KEY_SEMICOLON,[40]=KEY_APOSTROPHE,
    [41]=KEY_GRAVE,[42]=KEY_LEFTSHIFT,[43]=KEY_BACKSLASH,[44]=KEY_Z,
    [45]=KEY_X,[46]=KEY_C,[47]=KEY_V,[48]=KEY_B,[49]=KEY_N,[50]=KEY_M,
    [51]=KEY_COMMA,[52]=KEY_DOT,[53]=KEY_SLASH,[54]=KEY_RIGHTSHIFT,
    [55]=KEY_KPASTERISK,[56]=KEY_LEFTALT,[57]=KEY_SPACE,[58]=KEY_CAPSLOCK,
    [59]=KEY_F1,[60]=KEY_F2,[61]=KEY_F3,[62]=KEY_F4,[63]=KEY_F5,[64]=KEY_F6,
    [65]=KEY_F7,[66]=KEY_F8,[67]=KEY_F9,[68]=KEY_F10,[69]=KEY_NUMLOCK,
    [70]=KEY_SCROLLLOCK,
};

/* ------------------------------------------------------------------ *
 * Low-level I/O helpers.
 * ------------------------------------------------------------------ */
static inline uint16_t uhci_rd16(uint16_t reg) { return inw(g_uhci.io_base + reg); }
static inline void     uhci_wr16(uint16_t reg, uint16_t v) { outw(g_uhci.io_base + reg, v); }
static inline uint32_t uhci_rd32(uint16_t reg) { return inl(g_uhci.io_base + reg); }
static inline void     uhci_wr32(uint16_t reg, uint32_t v) { outl(g_uhci.io_base + reg, v); }
static inline uint8_t  uhci_rd8(uint16_t reg)  { return inb(g_uhci.io_base + reg); }
static inline void     uhci_wr8(uint16_t reg, uint8_t v)   { outb(g_uhci.io_base + reg, v); }

/* Crude busy-wait. The kernel's timer is not assumed available here, so we
 * spin on I/O port reads (each inb is ~1us on real HW / many cycles in QEMU). */
static void uhci_udelay(uint32_t us) {
    /* Reading an unused ISA port (0x80) is the classic ~1us delay. */
    for (uint32_t i = 0; i < us; i++) {
        (void)inb(0x80);
    }
}

/* Physical address of an identity-mapped low pointer. */
static inline uint32_t phys_of(void* p) { return (uint32_t)(uintptr_t)p; }

/* Allocate one 4KB identity-mapped page (zeroed) suitable for DMA. */
static void* uhci_alloc_page(void) {
    void* p = pmm_alloc_page();
    if (p) memset(p, 0, PAGE_SIZE);
    return p;
}

/* ------------------------------------------------------------------ *
 * Controller reset + schedule setup.
 * ------------------------------------------------------------------ */
static int uhci_reset_controller(void) {
    /* Global reset: assert GRESET for >=10ms, then clear. (USB spec: 50ms) */
    uhci_wr16(UHCI_USBCMD, UHCI_CMD_GRESET);
    uhci_udelay(50000);
    uhci_wr16(UHCI_USBCMD, 0);
    uhci_udelay(10000);

    /* Host controller reset: set HCRESET, wait for it to self-clear. */
    uhci_wr16(UHCI_USBCMD, UHCI_CMD_HCRESET);
    for (int i = 0; i < 100; i++) {
        uhci_udelay(1000);
        if (!(uhci_rd16(UHCI_USBCMD) & UHCI_CMD_HCRESET)) {
            break;
        }
    }
    if (uhci_rd16(UHCI_USBCMD) & UHCI_CMD_HCRESET) {
        kprintf("[UHCI] HCRESET did not clear\n");
        return -1;
    }

    /* Disable all interrupts -- we are poll-driven. */
    uhci_wr16(UHCI_USBINTR, 0);
    /* Clear any pending status (write-clear). */
    uhci_wr16(UHCI_USBSTS, 0xFFFF);
    /* Frame number = 0. */
    uhci_wr16(UHCI_FRNUM, 0);
    /* SOF timing default. */
    uhci_wr8(UHCI_SOFMOD, 0x40);
    return 0;
}

static int uhci_setup_schedule(void) {
    g_uhci.frame_list = (uint32_t*)uhci_alloc_page();   /* 1024 * 4 = 4KB */
    g_uhci.ctrl_qh    = (uhci_qh_t*)uhci_alloc_page();
    g_uhci.td_pool    = (uhci_td_t*)uhci_alloc_page();
    g_uhci.setup_buf  = (uint8_t*)uhci_alloc_page();
    g_uhci.data_buf   = (uint8_t*)uhci_alloc_page();

    if (!g_uhci.frame_list || !g_uhci.ctrl_qh || !g_uhci.td_pool ||
        !g_uhci.setup_buf || !g_uhci.data_buf) {
        kprintf("[UHCI] Failed to allocate DMA structures\n");
        return -1;
    }

    /* Control queue head: empty for now, terminates horizontally. */
    g_uhci.ctrl_qh->link    = UHCI_PTR_TERMINATE;
    g_uhci.ctrl_qh->element = UHCI_PTR_TERMINATE;

    /* Point every frame-list entry at the control QH. */
    uint32_t qh_link = phys_of(g_uhci.ctrl_qh) | UHCI_PTR_QH;
    for (int i = 0; i < 1024; i++) {
        g_uhci.frame_list[i] = qh_link;
    }

    /* Program frame list base and (re)start the controller. */
    uhci_wr32(UHCI_FRBASEADD, phys_of(g_uhci.frame_list));
    uhci_wr16(UHCI_FRNUM, 0);
    /* Run, MAXP=64 bytes, configure flag set. */
    uhci_wr16(UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_MAXP | UHCI_CMD_CF);
    return 0;
}

/* ------------------------------------------------------------------ *
 * Root-hub port reset / enable.
 * ------------------------------------------------------------------ */
static uint16_t port_reg(int port) {
    return (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
}

/* Write PORTSC while preserving write-clear (RWC) bits as 0. */
static void port_write(int port, uint16_t val) {
    uhci_wr16(port_reg(port), val & ~UHCI_PORT_RWC);
}

static bool port_reset_enable(int port) {
    uint16_t reg = port_reg(port);
    uint16_t st = uhci_rd16(reg);

    if (!(st & UHCI_PORT_CCS)) {
        return false;  /* nothing connected */
    }

    /* Clear connect-status-change by writing it back (RWC). */
    uhci_wr16(reg, st | UHCI_PORT_CSC);

    /* Assert reset for >=50ms. */
    st = uhci_rd16(reg);
    port_write(port, st | UHCI_PORT_RESET);
    uhci_udelay(50000);

    /* Deassert reset. */
    st = uhci_rd16(reg);
    port_write(port, st & ~UHCI_PORT_RESET);
    uhci_udelay(10000);

    /* Enable the port; retry while waiting for PE to stick (USB devices
     * need a few ms after reset before they enable). */
    for (int i = 0; i < 10; i++) {
        st = uhci_rd16(reg);
        /* Clear any enable/connect change bits, then set PE. */
        uhci_wr16(reg, (st & ~UHCI_PORT_RWC) | UHCI_PORT_PE | UHCI_PORT_CSC | UHCI_PORT_PEC);
        uhci_udelay(10000);
        st = uhci_rd16(reg);
        if (st & UHCI_PORT_PE) {
            return true;
        }
    }
    kprintf("[UHCI] Port %d failed to enable (PORTSC=%04x)\n", port + 1, st);
    return false;
}

/* ------------------------------------------------------------------ *
 * Control transfer (synchronous, poll for completion).
 *
 * Builds SETUP + (optional) DATA IN/OUT + STATUS stage TDs, queues them on
 * the control QH, and spins until the controller drains them.
 * Returns bytes transferred in the data stage, or negative on error.
 * ------------------------------------------------------------------ */
static int uhci_wait_td_chain(uhci_td_t* first, int count) {
    /* Spin until the QH element pointer terminates (all TDs consumed) or a
     * TD reports an error / goes inactive with an error bit. */
    for (uint32_t spins = 0; spins < SETUP_TIMEOUT_SPINS; spins++) {
        bool all_done = true;
        for (int i = 0; i < count; i++) {
            uint32_t s = first[i].status;
            if (s & TD_CTRL_ACTIVE) {
                all_done = false;
                break;
            }
            if (s & TD_STS_ANY_ERROR) {
                return -2;  /* transfer error (stall/CRC/babble) */
            }
        }
        if (all_done) {
            return 0;
        }
        (void)inb(0x80);
    }
    return -3;  /* timeout */
}

static int uhci_control_transfer(uhci_hid_t* dev_or_null, uint8_t addr,
                                 uint8_t maxpkt, bool low_speed,
                                 uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex,
                                 void* data, uint16_t wLength) {
    (void)dev_or_null;
    uhci_td_t* td = g_uhci.td_pool;
    uint8_t* setup = g_uhci.setup_buf;
    uint8_t* dbuf = g_uhci.data_buf;

    bool dir_in = (bmRequestType & 0x80) != 0;
    uint32_t ls = low_speed ? TD_CTRL_LS : 0;

    /* Build the 8-byte SETUP packet. */
    setup[0] = bmRequestType;
    setup[1] = bRequest;
    setup[2] = (uint8_t)(wValue & 0xFF);
    setup[3] = (uint8_t)(wValue >> 8);
    setup[4] = (uint8_t)(wIndex & 0xFF);
    setup[5] = (uint8_t)(wIndex >> 8);
    setup[6] = (uint8_t)(wLength & 0xFF);
    setup[7] = (uint8_t)(wLength >> 8);

    if (!dir_in && wLength && data) {
        memcpy(dbuf, data, wLength);
    }

    int n = 0;
    uint8_t toggle = 0;

    /* SETUP stage (DATA0). */
    td[n].status = TD_CTRL_ACTIVE | TD_CTRL_C_ERR3 | ls;
    td[n].token  = TD_TOKEN(TD_PID_SETUP, addr, 0, 0, 8);
    td[n].buffer = phys_of(setup);
    n++;
    toggle = 1;

    /* DATA stage (one or more packets), if any. */
    uint16_t remaining = wLength;
    uint16_t offset = 0;
    uint8_t data_pid = dir_in ? TD_PID_IN : TD_PID_OUT;
    while (remaining > 0) {
        uint16_t pkt = remaining > maxpkt ? maxpkt : remaining;
        td[n].status = TD_CTRL_ACTIVE | TD_CTRL_C_ERR3 | TD_CTRL_SPD | ls;
        td[n].token  = TD_TOKEN(data_pid, addr, 0, toggle, pkt);
        td[n].buffer = phys_of(dbuf + offset);
        n++;
        toggle ^= 1;
        offset += pkt;
        remaining -= pkt;
        if (n >= 30) break;  /* TD pool safety */
    }

    /* STATUS stage: opposite direction, zero length, DATA1. */
    uint8_t status_pid = dir_in ? TD_PID_OUT : TD_PID_IN;
    td[n].status = TD_CTRL_ACTIVE | TD_CTRL_C_ERR3 | ls;
    td[n].token  = TD_TOKEN_LEN0(status_pid, addr, 0, 1);
    td[n].buffer = 0;
    n++;

    /* Link the TDs vertically (depth-first), last terminates. */
    for (int i = 0; i < n; i++) {
        if (i + 1 < n) {
            td[i].link = phys_of(&td[i + 1]) | UHCI_PTR_DEPTH;
        } else {
            td[i].link = UHCI_PTR_TERMINATE;
        }
    }

    /* Attach to the control QH and let the HC run it. */
    g_uhci.ctrl_qh->element = phys_of(&td[0]);

    int rc = uhci_wait_td_chain(td, n);

    /* Detach. */
    g_uhci.ctrl_qh->element = UHCI_PTR_TERMINATE;

    if (rc != 0) {
        return rc;
    }

    /* Compute actual data length from the DATA-stage TDs. */
    int actual = 0;
    for (int i = 1; i < n - 1; i++) {
        uint32_t al = td[i].status & TD_STS_ACTLEN_MASK;
        /* actlen encodes (len-1); 0x7FF means zero bytes. */
        if (al == 0x7FF) {
            /* zero bytes transferred this TD */
        } else {
            actual += (int)(al + 1);
        }
    }

    if (dir_in && data && actual > 0) {
        int copy = actual > wLength ? wLength : actual;
        memcpy(data, dbuf, copy);
    }
    return actual;
}

/* ------------------------------------------------------------------ *
 * Enumeration.
 * ------------------------------------------------------------------ */

/* Parse a configuration descriptor blob: find a HID boot interface and its
 * interrupt-IN endpoint. Returns 0 if a HID interface was found. */
static int parse_config(uhci_hid_t* hid, uint8_t* cfg, int len) {
    int i = 0;
    bool in_hid_iface = false;
    while (i + 2 <= len) {
        uint8_t dlen = cfg[i];
        uint8_t dtype = cfg[i + 1];
        if (dlen == 0) break;
        if (i + dlen > len) break;

        if (dtype == USB_DESC_INTERFACE && dlen >= 9) {
            uint8_t iclass = cfg[i + 5];
            uint8_t iproto = cfg[i + 7];
            if (iclass == USB_CLASS_HID) {
                in_hid_iface = true;
                hid->is_hid = true;
                hid->hid_proto = iproto;  /* 1=kbd, 2=mouse (boot protocol) */
            } else {
                in_hid_iface = false;
            }
        } else if (dtype == USB_DESC_ENDPOINT && dlen >= 7 && in_hid_iface) {
            uint8_t epaddr = cfg[i + 2];
            uint8_t attr = cfg[i + 3];
            uint16_t mps = cfg[i + 4] | (cfg[i + 5] << 8);
            if ((attr & 0x03) == 0x03 && (epaddr & 0x80)) {  /* interrupt IN */
                hid->in_ep = epaddr;
                hid->in_maxpkt = (uint8_t)(mps & 0xFF ? mps : 8);
                if (hid->in_maxpkt == 0 || hid->in_maxpkt > 64) hid->in_maxpkt = 8;
                return 0;
            }
        }
        i += dlen;
    }
    return hid->is_hid ? 0 : -1;
}

static void hid_register_input(uhci_hid_t* hid) {
    const char* name = (hid->hid_proto == USB_HID_PROTOCOL_KEYBOARD)
                       ? "USB Keyboard (UHCI)"
                       : (hid->hid_proto == USB_HID_PROTOCOL_MOUSE)
                         ? "USB Mouse (UHCI)" : "USB HID (UHCI)";
    hid->input_dev = input_allocate_device(name);
    if (!hid->input_dev) {
        kprintf("[UHCI] input_allocate_device failed for %s\n", name);
        return;
    }
    hid->input_dev->driver_data = hid;
    if (hid->hid_proto == USB_HID_PROTOCOL_KEYBOARD) {
        hid->input_dev->supports_key = true;
    } else {
        hid->input_dev->supports_key = true;   /* buttons */
        hid->input_dev->supports_rel = true;
    }
    input_register_device(hid->input_dev);
    kprintf("[UHCI] Registered input device: %s\n", name);
}

/* Set up the interrupt-IN polling QH/TD for a HID device. */
static int hid_setup_interrupt(uhci_hid_t* hid) {
    hid->int_qh = (uhci_qh_t*)uhci_alloc_page();
    hid->int_td = (uhci_td_t*)uhci_alloc_page();
    hid->in_buf = (uint8_t*)uhci_alloc_page();
    if (!hid->int_qh || !hid->int_td || !hid->in_buf) {
        kprintf("[UHCI] interrupt QH/TD alloc failed\n");
        return -1;
    }

    hid->int_qh->element = UHCI_PTR_TERMINATE;

    /* Insert this interrupt QH into the schedule ahead of the control QH.
     * frame[*] -> int_qh -> ctrl_qh. (All frames share the same QH chain.) */
    hid->int_qh->link = phys_of(g_uhci.ctrl_qh) | UHCI_PTR_QH;
    uint32_t int_link = phys_of(hid->int_qh) | UHCI_PTR_QH;
    for (int i = 0; i < 1024; i++) {
        g_uhci.frame_list[i] = int_link;
    }

    hid->td_armed = false;
    return 0;
}

/* Arm (or re-arm) the single interrupt-IN TD. */
static void hid_arm_td(uhci_hid_t* hid) {
    uint32_t ls = hid->low_speed ? TD_CTRL_LS : 0;
    uint8_t epnum = hid->in_ep & 0x0F;
    uhci_td_t* td = hid->int_td;

    td->link   = UHCI_PTR_TERMINATE;
    td->status = TD_CTRL_ACTIVE | TD_CTRL_C_ERR3 | TD_CTRL_SPD | ls;
    td->token  = TD_TOKEN(TD_PID_IN, hid->addr, epnum, hid->in_toggle,
                          hid->in_maxpkt);
    td->buffer = phys_of(hid->in_buf);

    hid->int_qh->element = phys_of(td);
    hid->td_armed = true;
}

static void process_keyboard(uhci_hid_t* hid, uint8_t* data, int len) {
    if (len < 3 || !hid->input_dev) return;
    uint8_t mods = data[0];
    uint8_t* keys = &data[2];
    int nk = len - 2; if (nk > 6) nk = 6;

    uint8_t mc = mods ^ hid->prev_mods;
    if (mc & 0x01) input_report_key(hid->input_dev, KEY_LEFTCTRL,  (mods>>0)&1);
    if (mc & 0x02) input_report_key(hid->input_dev, KEY_LEFTSHIFT, (mods>>1)&1);
    if (mc & 0x04) input_report_key(hid->input_dev, KEY_LEFTALT,   (mods>>2)&1);
    if (mc & 0x20) input_report_key(hid->input_dev, KEY_RIGHTSHIFT,(mods>>5)&1);
    hid->prev_mods = mods;

    /* released keys */
    for (int i = 0; i < 6; i++) {
        uint8_t pk = hid->prev_keys[i];
        if (!pk) continue;
        bool still = false;
        for (int j = 0; j < nk; j++) if (keys[j] == pk) { still = true; break; }
        if (!still) {
            uint16_t kc = uhci_kbd_keycode[pk];
            if (kc != KEY_RESERVED)
                input_report_key(hid->input_dev, kc, KEY_STATE_RELEASED);
        }
    }
    /* newly pressed keys */
    for (int i = 0; i < nk; i++) {
        uint8_t k = keys[i];
        if (!k) continue;
        bool was = false;
        for (int j = 0; j < 6; j++) if (hid->prev_keys[j] == k) { was = true; break; }
        if (!was) {
            uint16_t kc = uhci_kbd_keycode[k];
            if (kc != KEY_RESERVED)
                input_report_key(hid->input_dev, kc, KEY_STATE_PRESSED);
        }
    }
    memset(hid->prev_keys, 0, 6);
    for (int i = 0; i < nk; i++) hid->prev_keys[i] = keys[i];
    input_sync(hid->input_dev);
}

static void process_mouse(uhci_hid_t* hid, uint8_t* data, int len) {
    if (len < 3 || !hid->input_dev) return;
    uint8_t btn = data[0];
    int8_t dx = (int8_t)data[1];
    int8_t dy = (int8_t)data[2];
    int8_t wheel = (len >= 4) ? (int8_t)data[3] : 0;

    uint8_t bc = btn ^ hid->prev_buttons;
    if (bc & 0x01) input_report_key(hid->input_dev, BTN_LEFT,   btn & 0x01 ? 1 : 0);
    if (bc & 0x02) input_report_key(hid->input_dev, BTN_RIGHT,  btn & 0x02 ? 1 : 0);
    if (bc & 0x04) input_report_key(hid->input_dev, BTN_MIDDLE, btn & 0x04 ? 1 : 0);
    hid->prev_buttons = btn;

    if (dx) input_report_rel(hid->input_dev, REL_X, dx);
    if (dy) input_report_rel(hid->input_dev, REL_Y, dy);
    if (wheel) input_report_rel(hid->input_dev, REL_WHEEL, wheel);
    input_sync(hid->input_dev);
}

/* Enumerate one connected, enabled port. */
static int enumerate_port(int port, bool low_speed) {
    if (g_uhci.num_hid >= UHCI_MAX_HID) return -1;
    uhci_hid_t* hid = &g_uhci.hid[g_uhci.num_hid];
    memset(hid, 0, sizeof(*hid));
    hid->port = (uint8_t)(port + 1);
    hid->low_speed = low_speed;
    hid->ep0_maxpkt = 8;  /* USB requires EP0 maxpkt >= 8 at address 0 */

    /* 1) Read first 8 bytes of the device descriptor at address 0. */
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    int rc = uhci_control_transfer(hid, 0, 8, low_speed,
                                   0x80, USB_REQ_GET_DESCRIPTOR,
                                   (USB_DESC_DEVICE << 8), 0, buf, 8);
    if (rc < 8) {
        kprintf("[UHCI] Port %d: GET_DESCRIPTOR(dev,8) failed rc=%d\n", port+1, rc);
        return -1;
    }
    hid->ep0_maxpkt = buf[7] ? buf[7] : 8;
    kprintf("[UHCI] Port %d: device descriptor[0..7]: "
            "%02x %02x %02x %02x %02x %02x %02x %02x (ep0 maxpkt=%u)\n",
            port+1, buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],
            hid->ep0_maxpkt);

    /* 2) Assign an address. */
    uint8_t newaddr = g_uhci.next_addr++;
    rc = uhci_control_transfer(hid, 0, hid->ep0_maxpkt, low_speed,
                               0x00, USB_REQ_SET_ADDRESS, newaddr, 0, NULL, 0);
    if (rc < 0) {
        kprintf("[UHCI] Port %d: SET_ADDRESS(%u) failed rc=%d\n", port+1, newaddr, rc);
        return -1;
    }
    uhci_udelay(5000);  /* device needs <=2ms to switch address */
    hid->addr = newaddr;

    /* 3) Read the full 18-byte device descriptor at the new address. */
    rc = uhci_control_transfer(hid, hid->addr, hid->ep0_maxpkt, low_speed,
                               0x80, USB_REQ_GET_DESCRIPTOR,
                               (USB_DESC_DEVICE << 8), 0, buf, 18);
    if (rc >= 18) {
        memcpy(&hid->devdesc, buf, sizeof(usb_device_descriptor_t));
        kprintf("[UHCI] addr=%u VID:PID=%04x:%04x class=%02x\n",
                hid->addr, hid->devdesc.vendor_id, hid->devdesc.product_id,
                hid->devdesc.device_class);
    }

    /* 4) Read configuration descriptor (first 9 bytes -> total length). */
    rc = uhci_control_transfer(hid, hid->addr, hid->ep0_maxpkt, low_speed,
                               0x80, USB_REQ_GET_DESCRIPTOR,
                               (USB_DESC_CONFIGURATION << 8), 0, buf, 9);
    if (rc < 9) {
        kprintf("[UHCI] addr=%u: GET cfg(9) failed rc=%d\n", hid->addr, rc);
        return -1;
    }
    uint16_t total = buf[2] | (buf[3] << 8);
    hid->config_value = buf[5];
    if (total > 64) total = 64;

    /* 5) Read the full configuration blob. */
    uint8_t cfg[64];
    memset(cfg, 0, sizeof(cfg));
    rc = uhci_control_transfer(hid, hid->addr, hid->ep0_maxpkt, low_speed,
                               0x80, USB_REQ_GET_DESCRIPTOR,
                               (USB_DESC_CONFIGURATION << 8), 0, cfg, total);
    if (rc < 9) {
        kprintf("[UHCI] addr=%u: GET cfg(full) failed rc=%d\n", hid->addr, rc);
        return -1;
    }

    if (parse_config(hid, cfg, rc) < 0 || !hid->is_hid) {
        kprintf("[UHCI] addr=%u: not a HID device (skipping)\n", hid->addr);
        return -1;
    }
    kprintf("[UHCI] addr=%u: HID interface proto=%u in_ep=%02x maxpkt=%u\n",
            hid->addr, hid->hid_proto, hid->in_ep, hid->in_maxpkt);

    /* 6) SET_CONFIGURATION. */
    rc = uhci_control_transfer(hid, hid->addr, hid->ep0_maxpkt, low_speed,
                               0x00, USB_REQ_SET_CONFIGURATION,
                               hid->config_value, 0, NULL, 0);
    if (rc < 0) {
        kprintf("[UHCI] addr=%u: SET_CONFIGURATION failed rc=%d\n", hid->addr, rc);
        return -1;
    }

    /* 7) HID boot protocol: SET_PROTOCOL(boot=0) + SET_IDLE(0). */
    uhci_control_transfer(hid, hid->addr, hid->ep0_maxpkt, low_speed,
                          0x21, USB_HID_REQ_SET_PROTOCOL, 0, 0, NULL, 0);
    uhci_control_transfer(hid, hid->addr, hid->ep0_maxpkt, low_speed,
                          0x21, USB_HID_REQ_SET_IDLE, 0, 0, NULL, 0);

    /* 8) Build the interrupt polling schedule and register input device. */
    if (hid_setup_interrupt(hid) < 0) return -1;
    hid_register_input(hid);

    g_uhci.num_hid++;
    hid_arm_td(hid);
    return 0;
}

/* ------------------------------------------------------------------ *
 * Public API.
 * ------------------------------------------------------------------ */
int uhci_init(void) {
    memset(&g_uhci, 0, sizeof(g_uhci));
    g_uhci.next_addr = 1;

    pci_device_t* pci = pci_find_class(PCI_CLASS_SERIAL_BUS,
                                       PCI_SUBCLASS_USB, PCI_PROGIF_UHCI);
    if (!pci) {
        kprintf("[UHCI] No UHCI controller found on PCI bus\n");
        kprintf("[UHCI]   (QEMU: add a UHCI/EHCI USB device, e.g. the default\n");
        kprintf("[UHCI]    PIIX3 USB, or use xHCI with a separate driver)\n");
        return -1;
    }

    /* UHCI registers live in I/O space at BAR4. */
    uint64_t bar4 = pci_get_bar(pci, 4);
    if (bar4 == 0 || !(pci->bar[4] & PCI_BAR_TYPE_IO)) {
        kprintf("[UHCI] BAR4 is not an I/O BAR (raw=%lx)\n",
                (unsigned long)pci->bar[4]);
        return -1;
    }
    g_uhci.io_base = (uint16_t)bar4;
    g_uhci.pci = pci;
    g_uhci.present = true;

    kprintf("[UHCI] Controller %02x:%02x.%x found, I/O base 0x%04x\n",
            pci->bus, pci->device, pci->function, g_uhci.io_base);

    /* Enable I/O space + bus master in the PCI command register. */
    {
        uint16_t cmd = pci_config_read_word(pci->bus, pci->device, pci->function,
                                            PCI_CONFIG_COMMAND);
        cmd |= PCI_COMMAND_IO_SPACE | PCI_COMMAND_BUS_MASTER;
        pci_config_write_word(pci->bus, pci->device, pci->function,
                              PCI_CONFIG_COMMAND, cmd);
    }
    /* Disable legacy SMI/keyboard emulation (PIIX USB_LEGSUP @ PCI 0xC0). */
    pci_config_write_word(pci->bus, pci->device, pci->function, 0xC0, 0x8F00);

    if (uhci_reset_controller() < 0) {
        kprintf("[UHCI] Controller reset failed\n");
        return -1;
    }
    kprintf("[UHCI] Controller reset OK\n");

    if (uhci_setup_schedule() < 0) {
        return -1;
    }
    kprintf("[UHCI] Schedule running (frame list @ phys 0x%08x)\n",
            phys_of(g_uhci.frame_list));

    /* Probe and enumerate root-hub ports. */
    for (int port = 0; port < UHCI_NUM_PORTS; port++) {
        uint16_t st = uhci_rd16(port_reg(port));
        kprintf("[UHCI] Port %d initial PORTSC=%04x (%sconnected)\n",
                port + 1, st, (st & UHCI_PORT_CCS) ? "" : "not ");
        if (!(st & UHCI_PORT_CCS)) continue;

        bool low_speed = (st & UHCI_PORT_LSDA) != 0;
        if (!port_reset_enable(port)) continue;

        kprintf("[UHCI] Port %d enabled (%s-speed), enumerating...\n",
                port + 1, low_speed ? "low" : "full");
        enumerate_port(port, low_speed);
    }

    kprintf("[UHCI] Init complete: %u HID device(s) attached\n", g_uhci.num_hid);
    return 0;
}

void uhci_poll(void) {
    if (!g_uhci.present) return;

    for (uint32_t i = 0; i < g_uhci.num_hid; i++) {
        uhci_hid_t* hid = &g_uhci.hid[i];
        if (!hid->is_hid || !hid->td_armed) continue;

        uint32_t s = hid->int_td->status;
        if (s & TD_CTRL_ACTIVE) {
            continue;  /* still pending */
        }

        if (s & TD_STS_ANY_ERROR) {
            /* NAK is normal (no data); real errors halt the endpoint -- just
             * re-arm with the same toggle on NAK, reset toggle on stall. */
            if (s & TD_CTRL_STALLED) {
                hid->in_toggle = 0;
            }
            hid_arm_td(hid);
            continue;
        }

        /* Completed. Compute actual length. */
        uint32_t al = s & TD_STS_ACTLEN_MASK;
        int len = (al == 0x7FF) ? 0 : (int)(al + 1);

        if (len > 0) {
            if (hid->hid_proto == USB_HID_PROTOCOL_KEYBOARD) {
                process_keyboard(hid, hid->in_buf, len);
            } else if (hid->hid_proto == USB_HID_PROTOCOL_MOUSE) {
                process_mouse(hid, hid->in_buf, len);
            }
        }

        /* Advance data toggle and re-arm for the next report. */
        hid->in_toggle ^= 1;
        hid_arm_td(hid);
    }
}

int usb_init_hc(void) {
    return uhci_init();
}
