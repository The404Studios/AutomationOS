/*
 * Serial Port Driver (16550 UART)
 * ===============================
 *
 * Provides basic serial output via COM1 for kernel debugging.
 * Configured for 38400 baud, 8N1, FIFO enabled.
 *
 * Performance notes
 * -----------------
 * The 16550 has a 16-byte transmit FIFO.  We exploit it in serial_write():
 * once the THR-empty bit (LSR bit 5) is set the FIFO is fully drained and we
 * can burst up to SERIAL_FIFO_DEPTH bytes before needing to poll again.  This
 * reduces the number of LSR reads from 1-per-byte to roughly 1-per-16-bytes,
 * a ~16x reduction in I/O port traffic.
 *
 * The spin loops are bounded by SERIAL_TX_SPIN_LIMIT so the kernel does not
 * hang forever if no UART is present (e.g. virtualised environments that
 * ignore writes to 0x3F8 and always return 0xFF from reads).
 */

#include "../include/x86_64.h"
#include "../include/types.h"
#include "../include/serial_regs.h"

/* Maximum poll iterations before giving up on a single byte.
 * At 38400 baud one character takes ~260 µs.  At ~3 ns/iteration (one inb)
 * that is ~87 000 iterations.  We use 200 000 as a generous upper bound. */
#define SERIAL_TX_SPIN_LIMIT  200000U

/* Transmit FIFO depth of the 16550A. */
#define SERIAL_FIFO_DEPTH     16U

static bool serial_initialized = false;

void serial_init(void) {
    outb(COM1_PORT + SERIAL_INT_ENABLE, SERIAL_INT_DISABLE_ALL);
    outb(COM1_PORT + SERIAL_LINE_CTRL, SERIAL_LCR_DLAB);
    outb(COM1_PORT + SERIAL_DIVISOR_LOW, SERIAL_BAUD_38400);
    outb(COM1_PORT + SERIAL_DIVISOR_HIGH, 0x00);
    outb(COM1_PORT + SERIAL_LINE_CTRL, SERIAL_LCR_8N1);
    outb(COM1_PORT + SERIAL_FIFO_CTRL, SERIAL_FIFO_CONFIG);
    outb(COM1_PORT + SERIAL_MODEM_CTRL, SERIAL_MCR_CONFIG);
    serial_initialized = true;
}

/*
 * serial_wait_tx - spin until the transmitter holding register is empty or
 * the spin limit is reached.  Returns 1 if the UART is ready, 0 on timeout.
 */
static int serial_wait_tx(void) {
    unsigned int spins = SERIAL_TX_SPIN_LIMIT;
    while (spins--) {
        if (inb(COM1_PORT + SERIAL_LINE_STATUS) & SERIAL_LSR_TX_EMPTY)
            return 1;
    }
    return 0; /* UART absent or wedged — drop the byte */
}

void serial_putchar(char c) {
    if (!serial_initialized)
        return;
    if (serial_wait_tx())
        outb(COM1_PORT + SERIAL_DATA_REG, (uint8_t)c);
}

/*
 * serial_write - write a buffer to COM1 with FIFO-aware batching.
 *
 * Strategy: when the FIFO reports empty (LSR_TX_EMPTY) we can safely push
 * up to SERIAL_FIFO_DEPTH bytes before the hardware needs draining again.
 * We track how many bytes we have deposited in the current FIFO slot; once
 * that slot is full we poll once more before continuing.
 *
 * This means for a typical 80-character log line we poll the LSR ~5 times
 * instead of ~80 times — a 16x reduction.
 */
void serial_write(const char* buf, size_t len) {
    if (!serial_initialized || !buf || len == 0)
        return;

    size_t i = 0;
    while (i < len) {
        /* Wait for FIFO to drain before starting a new burst. */
        if (!serial_wait_tx())
            return; /* UART wedged — abort remaining output */

        /* Burst up to FIFO_DEPTH bytes without re-polling. */
        size_t burst = len - i;
        if (burst > SERIAL_FIFO_DEPTH)
            burst = SERIAL_FIFO_DEPTH;

        for (size_t j = 0; j < burst; j++)
            outb(COM1_PORT + SERIAL_DATA_REG, (uint8_t)buf[i + j]);

        i += burst;
    }
}
