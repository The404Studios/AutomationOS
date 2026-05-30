/**
 * AutomationOS - Pseudo-Terminal (PTY) Driver
 *
 * POSIX-compatible PTY implementation with master/slave pairs.
 * Supports terminal I/O, line discipline, and termios ioctls.
 *
 * Architecture:
 *  - PTY master: /dev/ptmx (multiplexer)
 *  - PTY slaves: /dev/pts/0, /dev/pts/1, etc.
 *  - Bidirectional buffered I/O between master and slave
 *  - Line discipline for canonical mode, echo, signal generation
 */

#include "../../include/types.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/string.h"
#include "../../include/vfs.h"
#include "pty.h"

// PTY configuration
#define PTY_MAX_PAIRS      32      // Maximum number of PTY pairs
#define PTY_BUFFER_SIZE    4096    // Buffer size for each direction
#define PTY_NAME_MAX       64      // Max length of PTY name

// Line discipline control characters (termios)
#define CTRL(c) ((c) & 0x1F)
#define VINTR   0   // Interrupt (Ctrl+C)
#define VQUIT   1   // Quit (Ctrl+\)
#define VERASE  2   // Erase (Backspace)
#define VKILL   3   // Kill line (Ctrl+U)
#define VEOF    4   // EOF (Ctrl+D)
#define VSTART  5   // Start (Ctrl+Q)
#define VSTOP   6   // Stop (Ctrl+S)
#define VSUSP   7   // Suspend (Ctrl+Z)

// Terminal flags (simplified termios)
#define ECHO    0x0001  // Echo input
#define ICANON  0x0002  // Canonical mode (line buffering)
#define ISIG    0x0004  // Generate signals (INT, QUIT, SUSP)
#define IEXTEN  0x0008  // Extended processing

/**
 * Circular buffer for PTY data
 */
typedef struct {
    uint8_t data[PTY_BUFFER_SIZE];
    uint32_t head;      // Write position
    uint32_t tail;      // Read position
    uint32_t count;     // Number of bytes in buffer
} pty_buffer_t;

/**
 * PTY pair structure (master + slave)
 */
typedef struct {
    uint32_t index;             // PTY index (0-31)
    bool allocated;             // Is this pair allocated?

    // Master side
    pty_buffer_t master_to_slave;
    pty_buffer_t slave_to_master;

    // Terminal attributes (termios)
    struct {
        uint32_t c_lflag;       // Local flags (ECHO, ICANON, ISIG)
        uint8_t c_cc[8];        // Control characters
    } termios;

    // Window size
    struct {
        uint16_t ws_row;        // Rows
        uint16_t ws_col;        // Columns
        uint16_t ws_xpixel;     // Horizontal pixels (unused)
        uint16_t ws_ypixel;     // Vertical pixels (unused)
    } winsize;

    // Canonical mode line buffer
    uint8_t line_buffer[PTY_BUFFER_SIZE];
    uint32_t line_pos;

    // Reference counts
    uint32_t master_refs;
    uint32_t slave_refs;

    // Process group ID for signal handling
    int32_t pgrp;
} pty_pair_t;

// Global PTY state.
// Placed in .bss.deferred (emitted last by linker.ld) so this large buffer sits
// ABOVE the kernel's control-path globals, keeping them below the 0x200000
// userspace-ELF load base where a big user image cannot shadow them in its
// deep-copied low-half page tables. PTY access is not on the scheduling/fault/
// syscall-dispatch path that runs under a foreign CR3, so deferring it is safe.
static struct {
    pty_pair_t pairs[PTY_MAX_PAIRS];
    uint32_t next_index;
    bool initialized;
} pty_state __attribute__((section(".bss.deferred")));

// Function prototypes
static int pty_buffer_write(pty_buffer_t *buf, const uint8_t *data, uint32_t size);
static int pty_buffer_read(pty_buffer_t *buf, uint8_t *data, uint32_t size);
static uint32_t pty_buffer_available(const pty_buffer_t *buf);
static void pty_process_input(pty_pair_t *pty, uint8_t c);
static void pty_echo(pty_pair_t *pty, uint8_t c);

/**
 * Initialize PTY subsystem
 */
void pty_init(void) {
    kprintf("[PTY] Initializing pseudo-terminal driver...\n");

    memset(&pty_state, 0, sizeof(pty_state));

    for (uint32_t i = 0; i < PTY_MAX_PAIRS; i++) {
        pty_state.pairs[i].index = i;
        pty_state.pairs[i].allocated = false;
    }

    pty_state.next_index = 0;
    pty_state.initialized = true;

    kprintf("[PTY] Pseudo-terminal driver initialized (%u max pairs)\n", PTY_MAX_PAIRS);
}

/**
 * Allocate a new PTY pair
 * Returns PTY index, or -1 on failure
 */
int pty_allocate(void) {
    if (!pty_state.initialized) {
        return -1;
    }

    // Find free PTY
    for (uint32_t i = 0; i < PTY_MAX_PAIRS; i++) {
        uint32_t index = (pty_state.next_index + i) % PTY_MAX_PAIRS;
        if (!pty_state.pairs[index].allocated) {
            pty_pair_t *pty = &pty_state.pairs[index];

            // Initialize PTY
            memset(pty, 0, sizeof(pty_pair_t));
            pty->index = index;
            pty->allocated = true;
            pty->master_refs = 1;
            pty->slave_refs = 0;

            // Set default termios
            pty->termios.c_lflag = ECHO | ICANON | ISIG;
            pty->termios.c_cc[VINTR] = CTRL('C');
            pty->termios.c_cc[VQUIT] = CTRL('\\');
            pty->termios.c_cc[VERASE] = 0x7F;  // DEL/Backspace
            pty->termios.c_cc[VKILL] = CTRL('U');
            pty->termios.c_cc[VEOF] = CTRL('D');
            pty->termios.c_cc[VSTART] = CTRL('Q');
            pty->termios.c_cc[VSTOP] = CTRL('S');
            pty->termios.c_cc[VSUSP] = CTRL('Z');

            // Set default window size
            pty->winsize.ws_row = 24;
            pty->winsize.ws_col = 80;

            pty_state.next_index = (index + 1) % PTY_MAX_PAIRS;

            kprintf("[PTY] Allocated PTY pair %u\n", index);
            return (int)index;
        }
    }

    kprintf("[PTY] Failed to allocate PTY (all %u pairs in use)\n", PTY_MAX_PAIRS);
    return -1;  // All PTYs in use
}

/**
 * Free a PTY pair
 */
void pty_free(uint32_t index) {
    if (index >= PTY_MAX_PAIRS) {
        return;
    }

    pty_pair_t *pty = &pty_state.pairs[index];
    if (pty->allocated) {
        pty->allocated = false;
        pty->master_refs = 0;
        pty->slave_refs = 0;
        kprintf("[PTY] Freed PTY pair %u\n", index);
    }
}

/**
 * Get PTY pair by index
 */
pty_pair_t *pty_get(uint32_t index) {
    if (index >= PTY_MAX_PAIRS || !pty_state.pairs[index].allocated) {
        return NULL;
    }
    return &pty_state.pairs[index];
}

/**
 * Write data to master side (output from application)
 * Data goes to slave process
 */
ssize_t pty_master_write(uint32_t index, const uint8_t *data, uint32_t size) {
    pty_pair_t *pty = pty_get(index);
    if (!pty || !data) {
        return -1;
    }

    return pty_buffer_write(&pty->master_to_slave, data, size);
}

/**
 * Read data from master side (input from slave process)
 */
ssize_t pty_master_read(uint32_t index, uint8_t *data, uint32_t size) {
    pty_pair_t *pty = pty_get(index);
    if (!pty || !data) {
        return -1;
    }

    return pty_buffer_read(&pty->slave_to_master, data, size);
}

/**
 * Write data to slave side (input from user)
 * Data goes through line discipline
 */
ssize_t pty_slave_write(uint32_t index, const uint8_t *data, uint32_t size) {
    pty_pair_t *pty = pty_get(index);
    if (!pty || !data) {
        return -1;
    }

    // Process input through line discipline
    for (uint32_t i = 0; i < size; i++) {
        pty_process_input(pty, data[i]);
    }

    return (ssize_t)size;
}

/**
 * Read data from slave side (output to slave process)
 */
ssize_t pty_slave_read(uint32_t index, uint8_t *data, uint32_t size) {
    pty_pair_t *pty = pty_get(index);
    if (!pty || !data) {
        return -1;
    }

    return pty_buffer_read(&pty->master_to_slave, data, size);
}

/**
 * Set window size (TIOCSWINSZ ioctl)
 */
int pty_set_winsize(uint32_t index, uint16_t rows, uint16_t cols) {
    pty_pair_t *pty = pty_get(index);
    if (!pty) {
        return -1;
    }

    pty->winsize.ws_row = rows;
    pty->winsize.ws_col = cols;

    // TODO: Send SIGWINCH to slave process group

    kprintf("[PTY] Set PTY %u window size to %ux%u\n", index, cols, rows);
    return 0;
}

/**
 * Get window size (TIOCGWINSZ ioctl)
 */
int pty_get_winsize(uint32_t index, uint16_t *rows, uint16_t *cols) {
    pty_pair_t *pty = pty_get(index);
    if (!pty || !rows || !cols) {
        return -1;
    }

    *rows = pty->winsize.ws_row;
    *cols = pty->winsize.ws_col;
    return 0;
}

/**
 * Set terminal attributes (termios flags)
 */
int pty_set_termios(uint32_t index, uint32_t flags) {
    pty_pair_t *pty = pty_get(index);
    if (!pty) {
        return -1;
    }

    pty->termios.c_lflag = flags;
    return 0;
}

/**
 * Get terminal attributes
 */
int pty_get_termios(uint32_t index, uint32_t *flags) {
    pty_pair_t *pty = pty_get(index);
    if (!pty || !flags) {
        return -1;
    }

    *flags = pty->termios.c_lflag;
    return 0;
}

/**
 * Get slave device name
 */
int pty_get_slave_name(uint32_t index, char *name, uint32_t size) {
    if (index >= PTY_MAX_PAIRS || !name || size < 16) {
        return -1;
    }

    // Format: /dev/pts/N
    int written = snprintf(name, size, "/dev/pts/%u", index);
    return (written > 0 && written < (int)size) ? 0 : -1;
}

/**
 * Check if data is available for reading
 */
uint32_t pty_master_available(uint32_t index) {
    pty_pair_t *pty = pty_get(index);
    if (!pty) {
        return 0;
    }
    return pty_buffer_available(&pty->slave_to_master);
}

uint32_t pty_slave_available(uint32_t index) {
    pty_pair_t *pty = pty_get(index);
    if (!pty) {
        return 0;
    }
    return pty_buffer_available(&pty->master_to_slave);
}

// ============================================================================
// INTERNAL BUFFER OPERATIONS
// ============================================================================

/**
 * Write to circular buffer
 */
static int pty_buffer_write(pty_buffer_t *buf, const uint8_t *data, uint32_t size) {
    if (!buf || !data) {
        return -1;
    }

    uint32_t written = 0;
    for (uint32_t i = 0; i < size; i++) {
        if (buf->count >= PTY_BUFFER_SIZE) {
            break;  // Buffer full
        }

        buf->data[buf->head] = data[i];
        buf->head = (buf->head + 1) % PTY_BUFFER_SIZE;
        buf->count++;
        written++;
    }

    return (int)written;
}

/**
 * Read from circular buffer
 */
static int pty_buffer_read(pty_buffer_t *buf, uint8_t *data, uint32_t size) {
    if (!buf || !data) {
        return -1;
    }

    uint32_t read_count = 0;
    for (uint32_t i = 0; i < size; i++) {
        if (buf->count == 0) {
            break;  // Buffer empty
        }

        data[i] = buf->data[buf->tail];
        buf->tail = (buf->tail + 1) % PTY_BUFFER_SIZE;
        buf->count--;
        read_count++;
    }

    return (int)read_count;
}

/**
 * Get number of bytes available in buffer
 */
static uint32_t pty_buffer_available(const pty_buffer_t *buf) {
    return buf ? buf->count : 0;
}

/**
 * Process input character through line discipline
 */
static void pty_process_input(pty_pair_t *pty, uint8_t c) {
    if (!pty) {
        return;
    }

    bool canonical = pty->termios.c_lflag & ICANON;
    bool echo_enabled = pty->termios.c_lflag & ECHO;
    bool signals = pty->termios.c_lflag & ISIG;

    // Signal generation
    if (signals) {
        if (c == pty->termios.c_cc[VINTR]) {
            // Ctrl+C - send SIGINT
            // TODO: Send signal to process group
            kprintf("[PTY] SIGINT\n");
            if (echo_enabled) {
                pty_echo(pty, '^');
                pty_echo(pty, 'C');
                pty_echo(pty, '\n');
            }
            if (canonical) {
                pty->line_pos = 0;
            }
            return;
        } else if (c == pty->termios.c_cc[VQUIT]) {
            // Ctrl+\ - send SIGQUIT
            // TODO: Send signal to process group
            kprintf("[PTY] SIGQUIT\n");
            return;
        } else if (c == pty->termios.c_cc[VSUSP]) {
            // Ctrl+Z - send SIGTSTP
            // TODO: Send signal to process group
            kprintf("[PTY] SIGTSTP\n");
            return;
        }
    }

    // Canonical mode (line buffering)
    if (canonical) {
        if (c == pty->termios.c_cc[VERASE]) {
            // Backspace
            if (pty->line_pos > 0) {
                pty->line_pos--;
                if (echo_enabled) {
                    pty_echo(pty, '\b');
                    pty_echo(pty, ' ');
                    pty_echo(pty, '\b');
                }
            }
            return;
        } else if (c == pty->termios.c_cc[VKILL]) {
            // Kill line (Ctrl+U)
            while (pty->line_pos > 0) {
                pty->line_pos--;
                if (echo_enabled) {
                    pty_echo(pty, '\b');
                    pty_echo(pty, ' ');
                    pty_echo(pty, '\b');
                }
            }
            return;
        } else if (c == '\n' || c == '\r' || c == pty->termios.c_cc[VEOF]) {
            // End of line - flush to slave
            if (echo_enabled && (c == '\n' || c == '\r')) {
                pty_echo(pty, '\n');
            }

            /*
             * Add newline to line buffer only if there is space for it.
             * Without this guard, a buffer already filled to
             * PTY_BUFFER_SIZE-1 chars would be incremented to
             * PTY_BUFFER_SIZE-1 by the regular-char guard below, and then
             * a following newline/CR would write one byte past the array
             * (line_buffer[PTY_BUFFER_SIZE]).
             */
            if (c != pty->termios.c_cc[VEOF]) {
                if (pty->line_pos < PTY_BUFFER_SIZE) {
                    pty->line_buffer[pty->line_pos++] = '\n';
                }
            }

            // Write line to slave input
            pty_buffer_write(&pty->slave_to_master, pty->line_buffer, pty->line_pos);
            pty->line_pos = 0;
            return;
        } else {
            // Regular character - add to line buffer
            if (pty->line_pos < PTY_BUFFER_SIZE - 1) {
                pty->line_buffer[pty->line_pos++] = c;
                if (echo_enabled) {
                    pty_echo(pty, c);
                }
            }
            return;
        }
    } else {
        // Raw mode - pass character directly
        pty_buffer_write(&pty->slave_to_master, &c, 1);
        if (echo_enabled) {
            pty_echo(pty, c);
        }
    }
}

/**
 * Echo character back to master (for display to user)
 */
static void pty_echo(pty_pair_t *pty, uint8_t c) {
    if (!pty) {
        return;
    }
    pty_buffer_write(&pty->master_to_slave, &c, 1);
}
