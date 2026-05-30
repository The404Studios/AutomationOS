/*
 * PS/2 Mouse Driver — standalone, dedicated file
 * kernel/drivers/input/ps2mouse.c
 *
 * Supports:
 *   - Standard 3-byte PS/2 mouse packets
 *   - Intellimouse (scroll wheel) via device-ID==3 magic sequence → 4-byte
 *   - Overflow / bad-sync detection with automatic resync
 *   - Reports REL_X, REL_Y, REL_WHEEL and BTN_LEFT/RIGHT/MIDDLE via the
 *     kernel input/evdev pipeline (input_report_rel / input_report_key /
 *     input_sync) so the compositor reads pointer events from /dev/input.
 *
 * Build (compile-check only, no link):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -nostdinc -fno-pic -fno-pie     \
 *       -fno-stack-protector -mno-red-zone -mcmodel=kernel                  \
 *       -DSYSCALL_QUIET -DSCHEDULER_QUIET -DCONTEXT_SWITCH_QUIET            \
 *       -Wno-unused-variable -Wno-unused-function                           \
 *       -Wno-builtin-declaration-mismatch -Wno-implicit-function-declaration \
 *       -Wno-int-conversion -Wno-incompatible-pointer-types                 \
 *       -Ikernel/include -Ikernel/include/compat                            \
 *       -c kernel/drivers/input/ps2mouse.c -o /tmp/x.o
 */

#include "../../include/x86_64.h"
#include "../../include/types.h"
#include "../../include/input.h"
#include "../../include/ps2mouse.h"

/* -----------------------------------------------------------------------
 * Forward declaration of kprintf (provided by kernel/lib/kprintf.c or
 * equivalent; declared in kernel/include/kernel.h which we pull in here
 * only for kprintf to avoid dragging in scheduler/process headers).
 * ----------------------------------------------------------------------- */
extern int kprintf(const char *fmt, ...);

/* -----------------------------------------------------------------------
 * 8042 PS/2 controller port definitions
 * (mirrored locally to keep the driver self-contained; must match ps2.c)
 * ----------------------------------------------------------------------- */
#define PS2_DATA_PORT        0x60   /* read/write data */
#define PS2_STATUS_PORT      0x64   /* read status */
#define PS2_COMMAND_PORT     0x64   /* write command */

/* Status register bits */
#define PS2_STATUS_OBF       0x01   /* output buffer full  (data to read) */
#define PS2_STATUS_IBF       0x02   /* input  buffer full  (busy writing) */
#define PS2_STATUS_MOUSE_OBF 0x20   /* bit5: data in OBF came from mouse  */

/* Controller commands */
#define PS2_CMD_READ_CONFIG  0x20
#define PS2_CMD_WRITE_CONFIG 0x60
#define PS2_CMD_ENABLE_AUX   0xA8   /* enable second (mouse) port          */
#define PS2_CMD_WRITE_AUX    0xD4   /* next byte written to 0x60 → mouse   */

/* Mouse commands */
#define MOUSE_CMD_RESET       0xFF
#define MOUSE_CMD_SET_DEFAULTS 0xF6
#define MOUSE_CMD_SET_SAMPLE  0xF3
#define MOUSE_CMD_GET_ID      0xF2
#define MOUSE_CMD_ENABLE      0xF4  /* enable data reporting               */

/* Special response bytes */
#define PS2_ACK               0xFA
#define PS2_SELFTEST_PASS     0xAA
#define MOUSE_ID_STANDARD     0x00  /* 3-byte packets                      */
#define MOUSE_ID_INTELLIMOUSE 0x03  /* 4-byte packets, scroll wheel         */

/* Intellimouse negotiation sample rates */
#define INTELLIMOUSE_RATE1    200
#define INTELLIMOUSE_RATE2    100
#define INTELLIMOUSE_RATE3     80

/* -----------------------------------------------------------------------
 * IRQ registration (matches what ps2.c uses — simple void(void) wrapper)
 * ----------------------------------------------------------------------- */
extern void irq_register_handler(uint8_t irq, void (*handler)(void));

/* -----------------------------------------------------------------------
 * Driver state
 * ----------------------------------------------------------------------- */
static input_device_t *mouse_dev   = NULL;  /* registered input device     */
static bool  intellimouse_enabled  = false; /* true → 4-byte packets        */
static uint8_t packet[4];                   /* raw packet accumulator        */
static uint8_t packet_idx          = 0;     /* next byte slot (0-based)      */
static uint8_t packet_len          = 3;     /* 3 normal, 4 with scroll wheel */
static uint8_t prev_buttons        = 0;     /* previous button bitmask       */

/* -----------------------------------------------------------------------
 * Low-level 8042 helpers (bounded waits — never hang)
 * ----------------------------------------------------------------------- */

/* Wait until the input buffer is empty (controller ready to accept a byte) */
static void ps2m_wait_write(void) {
    uint32_t timeout = 100000;
    while (timeout--) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_IBF))
            return;
    }
}

/* Wait until the output buffer has a byte (controller has data for us) */
static uint8_t ps2m_read_byte_timeout(bool *ok) {
    uint32_t timeout = 100000;
    while (timeout--) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OBF) {
            if (ok) *ok = true;
            return inb(PS2_DATA_PORT);
        }
    }
    if (ok) *ok = false;
    return 0;
}

/* Drain any stale bytes sitting in the controller output buffer */
static void ps2m_flush(void) {
    uint32_t limit = 64;
    while (limit-- && (inb(PS2_STATUS_PORT) & PS2_STATUS_OBF)) {
        inb(PS2_DATA_PORT);
    }
}

/* Send a byte to the mouse via the auxiliary-port write prefix */
static void ps2m_send(uint8_t data) {
    ps2m_wait_write();
    outb(PS2_COMMAND_PORT, PS2_CMD_WRITE_AUX);
    ps2m_wait_write();
    outb(PS2_DATA_PORT, data);
}

/* Send a byte to the mouse and wait for an ACK (0xFA).
 * Returns true on ACK, false on timeout or NAK.            */
static bool ps2m_send_ack(uint8_t data) {
    ps2m_send(data);
    bool ok;
    /* The controller may queue the ACK byte from the mouse behind any
     * previously queued bytes; drain them until we see 0xFA.           */
    uint32_t retries = 8;
    while (retries--) {
        uint8_t r = ps2m_read_byte_timeout(&ok);
        if (!ok)        return false;
        if (r == PS2_ACK) return true;
        /* 0xFE = resend — ignore here and keep draining */
    }
    return false;
}

/* -----------------------------------------------------------------------
 * Intellimouse scroll-wheel negotiation
 *
 * The protocol: write sample rates 200, 100, 80 then query device ID.
 * If the mouse supports scroll it returns 0x03 and switches to 4-byte mode.
 * ----------------------------------------------------------------------- */
static bool ps2m_probe_intellimouse(void) {
    /* Set sample rate 200 */
    if (!ps2m_send_ack(MOUSE_CMD_SET_SAMPLE)) return false;
    if (!ps2m_send_ack(INTELLIMOUSE_RATE1))   return false;

    /* Set sample rate 100 */
    if (!ps2m_send_ack(MOUSE_CMD_SET_SAMPLE)) return false;
    if (!ps2m_send_ack(INTELLIMOUSE_RATE2))   return false;

    /* Set sample rate 80 */
    if (!ps2m_send_ack(MOUSE_CMD_SET_SAMPLE)) return false;
    if (!ps2m_send_ack(INTELLIMOUSE_RATE3))   return false;

    /* Query device ID */
    if (!ps2m_send_ack(MOUSE_CMD_GET_ID))     return false;
    bool ok;
    uint8_t id = ps2m_read_byte_timeout(&ok);
    if (!ok) return false;

    if (id == MOUSE_ID_INTELLIMOUSE) {
        kprintf("[PS2MOUSE] Intellimouse scroll wheel detected (device ID 0x03)\n");
        return true;
    }
    kprintf("[PS2MOUSE] Standard mouse (device ID 0x%02x)\n", id);
    return false;
}

/* -----------------------------------------------------------------------
 * Packet processing
 *
 * Byte 0 (flags):
 *   bit7 = Y overflow
 *   bit6 = X overflow
 *   bit5 = Y sign   (extends byte2 to 9 bits, two's complement)
 *   bit4 = X sign   (extends byte1 to 9 bits, two's complement)
 *   bit3 = always 1 (sync bit — used to re-align the packet stream)
 *   bit2 = middle button
 *   bit1 = right  button
 *   bit0 = left   button
 *
 * Byte 1: dX (unsigned; sign in byte0 bit4)
 * Byte 2: dY (unsigned; sign in byte0 bit5)  — PS/2 Y axis is screen-inverted
 * Byte 3: scroll (Intellimouse only, signed 4-bit two's complement in bits 3:0)
 * ----------------------------------------------------------------------- */
static void ps2m_process_packet(void) {
    uint8_t flags = packet[0];

    /* Resync guard: bit3 of byte0 must be 1 in a valid packet header.
     * If it is 0 we have lost sync; discard and restart.              */
    if (!(flags & 0x08)) {
        kprintf("[PS2MOUSE] Bad sync (bit3=0), resyncing\n");
        packet_idx = 0;
        return;
    }

    /* Overflow: if either overflow bit is set the deltas are garbage.
     * Discard the packet but keep the sync counter rolling.           */
    if (flags & 0xC0) {
        packet_idx = 0;
        return;
    }

    /* Sign-extend dX and dY to 16-bit using the sign bits in byte0 */
    int16_t dx = (int16_t)packet[1];
    int16_t dy = (int16_t)packet[2];
    if (flags & 0x10) dx |= (int16_t)0xFF00;   /* X sign bit */
    if (flags & 0x20) dy |= (int16_t)0xFF00;   /* Y sign bit */

    /* PS/2 Y axis is INVERTED relative to screen coordinates:
     * positive dy from the hardware means the physical ball/sensor moved
     * upward, which on screen should be negative Y (cursor moves up).
     * Negate to convert to screen-space.                               */
    dy = -dy;

    /* Scroll wheel (Intellimouse, byte3, bits 3:0, signed 4-bit) */
    int8_t dw = 0;
    if (intellimouse_enabled) {
        /* Extract the 4-bit signed value from byte3 */
        int8_t raw = (int8_t)(packet[3] & 0x0F);
        if (raw & 0x08)                 /* sign-extend from 4 bits */
            raw |= (int8_t)0xF0;
        dw = raw;
    }

    /* Button bitmask from bits 0-2 of byte0 */
    uint8_t buttons = flags & 0x07;

    if (!mouse_dev) {
        packet_idx = 0;
        return;
    }

    /* --- Report relative motion --- */
    if (dx != 0)
        input_report_rel(mouse_dev, REL_X, (int32_t)dx);
    if (dy != 0)
        input_report_rel(mouse_dev, REL_Y, (int32_t)dy);
    if (dw != 0)
        input_report_rel(mouse_dev, REL_WHEEL, (int32_t)dw);

    /* --- Report button state changes --- */
    if ((buttons ^ prev_buttons) & 0x01)
        input_report_key(mouse_dev, BTN_LEFT,   (buttons & 0x01) ? 1 : 0);
    if ((buttons ^ prev_buttons) & 0x02)
        input_report_key(mouse_dev, BTN_RIGHT,  (buttons & 0x02) ? 1 : 0);
    if ((buttons ^ prev_buttons) & 0x04)
        input_report_key(mouse_dev, BTN_MIDDLE, (buttons & 0x04) ? 1 : 0);

    prev_buttons = buttons;

    /* Flush the event batch to evdev clients */
    input_sync(mouse_dev);

    packet_idx = 0;
}

/* -----------------------------------------------------------------------
 * IRQ 12 handler — called for every byte the mouse sends
 *
 * Strategy: accumulate bytes in packet[].  On each IRQ we check the status
 * register; bit5 confirms the byte came from the auxiliary (mouse) port.
 * We only act on a full packet (packet_idx == packet_len).
 * ----------------------------------------------------------------------- */
void ps2mouse_irq_handler(void) {
    uint8_t status = inb(PS2_STATUS_PORT);

    /* Nothing in the output buffer — spurious IRQ */
    if (!(status & PS2_STATUS_OBF))
        return;

    /* Byte came from keyboard port, not the mouse — don't steal it */
    if (!(status & PS2_STATUS_MOUSE_OBF))
        return;

    uint8_t data = inb(PS2_DATA_PORT);

    /* Sync heuristic for byte 0:
     * If we are expecting byte 0 but the incoming byte has bit3==0, it is
     * NOT a valid packet start — keep discarding until we see a byte with
     * bit3 set.  This handles the case where we boot with the mouse mid-
     * stream (e.g. a button was held at power-on).                      */
    if (packet_idx == 0 && !(data & 0x08)) {
        /* Not a valid packet header; skip this byte */
        return;
    }

    packet[packet_idx++] = data;

    if (packet_idx == packet_len) {
        ps2m_process_packet();
        /* ps2m_process_packet() resets packet_idx to 0 */
    }
}

/* -----------------------------------------------------------------------
 * ps2mouse_init — public entry point
 *
 * Call this AFTER input_init(), dev_input_init(), and ps2_init() (keyboard).
 * In kernel/kernel.c, immediately after the existing ps2_init() call:
 *
 *   extern void ps2mouse_init(void);
 *   ps2mouse_init();
 *
 * Sequence:
 *   1. Enable the 8042 auxiliary (mouse) port (command 0xA8)
 *   2. Drain stale bytes
 *   3. Read + modify controller config byte: set bit1 (aux IRQ enable) and
 *      clear bit5 (aux clock disable → enable mouse clock)
 *   4. Set mouse defaults (0xF6)
 *   5. Attempt Intellimouse negotiation; if device ID == 3 use 4-byte mode
 *   6. Enable data reporting (0xF4)
 *   7. Allocate and register an input_device_t with REL + KEY capabilities
 *   8. Register IRQ12 handler via irq_register_handler()
 * ----------------------------------------------------------------------- */
void ps2mouse_init(void) {
    kprintf("[PS2MOUSE] Initialising PS/2 mouse driver\n");

    /* Step 1 — enable the auxiliary port */
    ps2m_wait_write();
    outb(PS2_COMMAND_PORT, PS2_CMD_ENABLE_AUX);

    /* Drain any garbage in the output buffer */
    ps2m_flush();

    /* Step 2 — update controller config byte:
     *   bit1 (0x02): enable IRQ12 for auxiliary port
     *   bit5 (0x20): 0 = enable auxiliary port clock (mouse gets power)
     */
    ps2m_wait_write();
    outb(PS2_COMMAND_PORT, PS2_CMD_READ_CONFIG);
    bool ok;
    uint8_t config = ps2m_read_byte_timeout(&ok);
    if (!ok) {
        kprintf("[PS2MOUSE] Failed to read controller config; aborting\n");
        return;
    }
    config |=  0x02;   /* enable aux IRQ12          */
    config &= ~0x20;   /* enable aux clock           */

    ps2m_wait_write();
    outb(PS2_COMMAND_PORT, PS2_CMD_WRITE_CONFIG);
    ps2m_wait_write();
    outb(PS2_DATA_PORT, config);

    /* Drain again after config change */
    ps2m_flush();

    /* Step 3 — set mouse defaults */
    if (!ps2m_send_ack(MOUSE_CMD_SET_DEFAULTS)) {
        kprintf("[PS2MOUSE] Set defaults failed (no ACK); continuing\n");
        /* Non-fatal: proceed anyway — QEMU mouse usually tolerates this */
    }

    /* Step 4 — try Intellimouse negotiation */
    ps2m_flush();
    intellimouse_enabled = ps2m_probe_intellimouse();
    packet_len = intellimouse_enabled ? 4 : 3;
    kprintf("[PS2MOUSE] Packet size: %u bytes%s\n",
            (unsigned)packet_len,
            intellimouse_enabled ? " (Intellimouse)" : "");

    /* Step 5 — enable data reporting */
    if (!ps2m_send_ack(MOUSE_CMD_ENABLE)) {
        kprintf("[PS2MOUSE] Enable data reporting failed (no ACK); continuing\n");
    }

    /* Step 6 — flush any ACK / self-test bytes emitted after enable */
    ps2m_flush();

    /* Reset driver state */
    packet_idx   = 0;
    prev_buttons = 0;

    /* Step 7 — allocate and register the input device */
    mouse_dev = input_allocate_device("PS/2 Mouse (ps2mouse.c)");
    if (!mouse_dev) {
        kprintf("[PS2MOUSE] Failed to allocate input device\n");
        return;
    }
    mouse_dev->supports_key = true;   /* BTN_LEFT, BTN_RIGHT, BTN_MIDDLE */
    mouse_dev->supports_rel = true;   /* REL_X, REL_Y, REL_WHEEL          */
    mouse_dev->supports_abs = false;
    mouse_dev->supports_led = false;

    if (input_register_device(mouse_dev) < 0) {
        kprintf("[PS2MOUSE] input_register_device failed\n");
        input_free_device(mouse_dev);
        mouse_dev = NULL;
        return;
    }
    kprintf("[PS2MOUSE] Registered as /dev/input/event%u\n",
            (unsigned)(mouse_dev->id - 1));

    /* Step 8 — register IRQ12 handler.
     * irq_register_handler(irq, handler) expects a void(*)(void) function.
     * ps2mouse_irq_handler() already has that exact signature.
     * Registering via this API also unmasks IRQ12 in the PIC.
     */
    irq_register_handler(12, ps2mouse_irq_handler);
    kprintf("[PS2MOUSE] IRQ12 registered\n");

    kprintf("[PS2MOUSE] PS/2 mouse driver ready\n");
}
