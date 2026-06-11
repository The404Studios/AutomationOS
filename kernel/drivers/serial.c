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
 * IMPORTANT: each iteration is an `inb` of the UART LSR — a PORT-I/O cycle that
 * costs ~1 µs on REAL hardware (LPC Super-I/O), NOT the ~3 ns of a cached read.
 * The old 200000 bound therefore meant ~200 ms of spinning PER stalled write —
 * and serial writes run with interrupts disabled (syscalls clear IF via FMASK),
 * so that spin FREEZES the PIT and the whole cooperative system. On the T410,
 * COM1 physically exists (probe passes) but nothing drains it, so the TX FIFO
 * fills and every write hit the full spin -> desktop froze ~1 s in. A byte
 * transmits in <1 ms at any real baud, so 20000 (~20 ms worst case on real HW,
 * instant on QEMU) is already very generous. See also the wedge auto-disable. */
#define SERIAL_TX_SPIN_LIMIT  20000U

/* Consecutive serial_wait_tx timeouts after which we declare the UART WEDGED and
 * disable serial output entirely (every subsequent write becomes a no-op). This
 * bounds the total damage a dead/undrained UART can do to ONE-TIME
 * SERIAL_WEDGE_LIMIT * SERIAL_TX_SPIN_LIMIT iterations, instead of re-spinning
 * forever on every write. Critical on real hardware where a present-but-unread
 * COM1 would otherwise stall the timer on every log line. */
#define SERIAL_WEDGE_LIMIT    4U

/* Transmit FIFO depth of the 16550A. */
#define SERIAL_FIFO_DEPTH     16U

static bool serial_initialized = false;

/*
 * Minimal spinlock for serial output.  Prevents interleaved output when
 * kprintf is called from both CPUs or from interrupt context concurrently.
 * Uses a simple test-and-set with a volatile flag; on a uniprocessor this
 * is sufficient, on SMP it serialises accesses without a full ticket lock.
 */
static volatile uint32_t serial_lock = 0;
static uint64_t serial_lock_flags = 0;

/* IRQ-SAFE acquire: clear IF for the whole critical section. Without this, a
 * ring-0 IF=1 path holding serial_lock could be interrupted by an IRQ whose
 * handler also kprintf()s (e.g. the PS/2 mouse IRQ logging a bad packet) — that
 * handler would spin on serial_lock forever while the holder, preempted, never
 * releases it => hard freeze. On the T410 the real touchpad mis-syncs and the
 * IRQ12 handler logs constantly, so this deadlock is reachable (invisible on
 * QEMU, which never fires IRQ12). Clearing IF makes the section uninterruptible
 * on this (uniprocessor, cooperative) kernel, so no IRQ can re-enter the lock.
 * The test-and-set is retained as SMP belt-and-suspenders. */
static inline void serial_lock_acquire(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(&serial_lock, 1)) {
        asm volatile("pause");
    }
    serial_lock_flags = flags;   /* safe: IF=0, no other context can race here */
}

static inline void serial_lock_release(void) {
    uint64_t flags = serial_lock_flags;
    __sync_lock_release(&serial_lock);
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

/*
 * Probe whether a real UART is present at COM1.  The scratch register
 * (offset 7) is a read/write register that has no side-effects.  We write
 * a test byte and read it back; if the value round-trips the UART is real.
 * If the port is absent the read typically returns 0xFF (floating bus).
 */
static bool serial_probe_uart(uint16_t port) {
    uint8_t test_val = 0xA5;
    outb(port + SERIAL_SCRATCH, test_val);
    uint8_t readback = inb(port + SERIAL_SCRATCH);
    if (readback != test_val) return false;

    /* Second pattern to rule out a stuck bus. */
    test_val = 0x5A;
    outb(port + SERIAL_SCRATCH, test_val);
    readback = inb(port + SERIAL_SCRATCH);
    return (readback == test_val);
}

void serial_init(void) {
    /* Probe first -- if no UART is wired we skip init entirely so later
     * serial_putchar/serial_write calls are silent no-ops. */
    if (!serial_probe_uart(COM1_PORT)) {
        serial_initialized = false;
        return;
    }

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
static unsigned int serial_tx_timeouts = 0;  /* consecutive TX-wait timeouts */

static int serial_wait_tx(void) {
    unsigned int spins = SERIAL_TX_SPIN_LIMIT;
    while (spins--) {
        if (inb(COM1_PORT + SERIAL_LINE_STATUS) & SERIAL_LSR_TX_EMPTY) {
            serial_tx_timeouts = 0;   /* drained OK — reset the wedge counter */
            return 1;
        }
    }
    /* Timed out. After a few consecutive timeouts the UART is wedged (present
     * but undrained, e.g. T410 COM1 with no reader). Disable serial entirely so
     * every future write is an immediate no-op — otherwise each write re-spins
     * with interrupts off and freezes the timer/scheduler. */
    if (++serial_tx_timeouts >= SERIAL_WEDGE_LIMIT)
        serial_initialized = false;
    return 0; /* UART absent or wedged — drop the byte */
}

void serial_putchar(char c) {
    if (!serial_initialized)
        return;
    serial_lock_acquire();
    if (serial_wait_tx())
        outb(COM1_PORT + SERIAL_DATA_REG, (uint8_t)c);
    serial_lock_release();
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

    serial_lock_acquire();

    size_t i = 0;
    while (i < len) {
        /* Wait for FIFO to drain before starting a new burst. */
        if (!serial_wait_tx()) {
            serial_lock_release();
            return; /* UART wedged — abort remaining output */
        }

        /* Burst up to FIFO_DEPTH bytes without re-polling. */
        size_t burst = len - i;
        if (burst > SERIAL_FIFO_DEPTH)
            burst = SERIAL_FIFO_DEPTH;

        for (size_t j = 0; j < burst; j++)
            outb(COM1_PORT + SERIAL_DATA_REG, (uint8_t)buf[i + j]);

        i += burst;
    }

    serial_lock_release();
}
