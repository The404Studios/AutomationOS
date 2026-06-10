/*
 * Kernel Printf Implementation
 * ============================
 *
 * Printf for kernel debugging and logging via serial port.
 * Supports: %s, %c, %d, %i, %u, %x, %X, %p, %%, %l*, %ll*, %z*, %h*,
 *           width, zero-padding, %.Nf (placeholder), unknown specifiers.
 *
 * Performance
 * -----------
 * All output is assembled into a fixed-size stack buffer (KPRINTF_BUF_SIZE).
 * A single serial_write() call is made at the end, so the LSR busy-poll in
 * the serial driver is amortized over the entire formatted line rather than
 * firing once per character.  For a typical 80-byte log line this reduces
 * LSR polls from ~80 to ~5 (serial_write FIFO batching) — roughly a 16x
 * reduction in I/O port traffic per kprintf() call.
 *
 * If the formatted output exceeds KPRINTF_BUF_SIZE-1 bytes it is silently
 * truncated (safe — no overflow possible).  Increase the constant if you
 * need longer lines.
 */

#include "../include/kernel.h"
#include "../include/types.h"

/* Maximum number of characters kprintf can emit per call (inc. NUL). */
#define KPRINTF_BUF_SIZE  1024

void serial_write(const char* str, size_t len);
extern size_t strlen(const char* str);

/* -------------------------------------------------------------------------
 * Buffer helpers
 * ---------------------------------------------------------------------- */

typedef struct {
    char   buf[KPRINTF_BUF_SIZE];
    size_t pos;   /* next write position */
} kbuf_t;

/* Append a single character, silently drop if full. */
static inline void buf_putc(kbuf_t* b, char c) {
    if (b->pos < KPRINTF_BUF_SIZE - 1)
        b->buf[b->pos++] = c;
}

/* Append a counted string, clamped to available space. */
static inline void buf_write(kbuf_t* b, const char* s, size_t n) {
    size_t avail = (KPRINTF_BUF_SIZE - 1) - b->pos;
    if (n > avail) n = avail;
    for (size_t i = 0; i < n; i++)
        b->buf[b->pos++] = s[i];
}

/* -------------------------------------------------------------------------
 * Number formatters
 * ---------------------------------------------------------------------- */

/*
 * Format an unsigned integer into a local reverse buffer, then append to b
 * with optional zero/space padding.  Returns the number of characters added
 * to b (including padding).
 */
static int buf_print_number_padded(kbuf_t* b, uint64_t num, int base,
                                   int min_width, char pad_char, int uppercase)
{
    char tmp[32];
    int  i = 0;
    int  count = 0;

    if (num == 0) {
        tmp[i++] = '0';
    } else {
        while (num > 0) {
            int digit = (int)(num % (uint64_t)base);
            if (digit < 10)
                tmp[i++] = (char)('0' + digit);
            else
                tmp[i++] = (char)((uppercase ? 'A' : 'a') + digit - 10);
            num /= (uint64_t)base;
        }
    }

    /* Left-pad to min_width */
    for (int j = i; j < min_width; j++) {
        buf_putc(b, pad_char);
        count++;
    }

    /* Emit digits in correct (reverse) order */
    while (i > 0) {
        buf_putc(b, tmp[--i]);
        count++;
    }

    return count;
}

static int buf_print_signed(kbuf_t* b, int64_t val, int min_width, char pad_char)
{
    int count = 0;
    if (val < 0) {
        buf_putc(b, '-');
        count++;
        if (min_width > 0) min_width--;
        /* unsigned negate is UB-free for INT64_MIN (where -val would overflow) */
        count += buf_print_number_padded(b, -(uint64_t)val, 10, min_width, pad_char, 0);
        return count;
    }
    count += buf_print_number_padded(b, (uint64_t)val, 10, min_width, pad_char, 0);
    return count;
}

/* -------------------------------------------------------------------------
 * kprintf
 * ---------------------------------------------------------------------- */

int kprintf(const char* format, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, format);

    kbuf_t b;
    b.pos = 0;

    int count = 0;

    for (const char* p = format; *p; p++) {
        if (*p != '%') {
            buf_putc(&b, *p);
            count++;
            continue;
        }

        p++; /* skip '%' */
        if (!*p) break;

        /* --- flags --- */
        char pad_char = ' ';
        int left_align = 0;
        if (*p == '-') {
            left_align = 1;
            p++;
        }
        if (*p == '0') {
            pad_char = '0';
            p++;
        }

        /* --- width --- */
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        /* --- precision (for %.Nf) --- */
        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            while (*p >= '0' && *p <= '9') {
                precision = precision * 10 + (*p - '0');
                p++;
            }
        }
        (void)precision; /* used only for %f placeholder path */

        /* --- length modifier --- */
        int long_count = 0; /* 0=int, 1=long, 2=long long / size_t */
        if (*p == 'l') {
            long_count = 1;
            p++;
            if (*p == 'l') {
                long_count = 2;
                p++;
            }
        } else if (*p == 'h') {
            p++;
            if (*p == 'h') p++; /* %hh -> char */
        } else if (*p == 'z') {
            long_count = 2; /* size_t is 64-bit on this target */
            p++;
        }

        if (!*p) break;

        switch (*p) {
            case 's': {
                const char* str = __builtin_va_arg(args, const char*);
                if (!str) str = "(null)";
                int slen = (int)strlen(str);
                if (!left_align) {
                    /* Right-align: pad with spaces before the string */
                    for (int i = slen; i < width; i++) {
                        buf_putc(&b, ' ');
                        count++;
                    }
                }
                buf_write(&b, str, (size_t)slen);
                count += slen;
                if (left_align) {
                    /* Left-align: pad with spaces after the string */
                    for (int i = slen; i < width; i++) {
                        buf_putc(&b, ' ');
                        count++;
                    }
                }
                break;
            }
            case 'c': {
                char c = (char)__builtin_va_arg(args, int);
                buf_putc(&b, c);
                count++;
                break;
            }
            case 'd':
            case 'i': {
                int64_t val;
                if (long_count >= 2)
                    val = __builtin_va_arg(args, int64_t);
                else if (long_count == 1)
                    val = __builtin_va_arg(args, long);
                else
                    val = __builtin_va_arg(args, int);
                count += buf_print_signed(&b, val, width, pad_char);
                break;
            }
            case 'u': {
                uint64_t val;
                if (long_count >= 2)
                    val = __builtin_va_arg(args, uint64_t);
                else if (long_count == 1)
                    val = __builtin_va_arg(args, unsigned long);
                else
                    val = __builtin_va_arg(args, unsigned int);
                count += buf_print_number_padded(&b, val, 10, width, pad_char, 0);
                break;
            }
            case 'x': {
                uint64_t val;
                if (long_count >= 2)
                    val = __builtin_va_arg(args, uint64_t);
                else if (long_count == 1)
                    val = __builtin_va_arg(args, unsigned long);
                else
                    val = __builtin_va_arg(args, unsigned int);
                count += buf_print_number_padded(&b, val, 16, width, pad_char, 0);
                break;
            }
            case 'X': {
                uint64_t val;
                if (long_count >= 2)
                    val = __builtin_va_arg(args, uint64_t);
                else if (long_count == 1)
                    val = __builtin_va_arg(args, unsigned long);
                else
                    val = __builtin_va_arg(args, unsigned int);
                count += buf_print_number_padded(&b, val, 16, width, pad_char, 1);
                break;
            }
            case 'p': {
                void* ptr = __builtin_va_arg(args, void*);
                buf_write(&b, "0x", 2);
                count += 2;
                count += buf_print_number_padded(&b, (uint64_t)ptr, 16, 16, '0', 0);
                break;
            }
            case 'f': {
                /*
                 * FPU may not be initialised in kernel context (-mno-sse).
                 * Pop the double-width va_list slot to keep the list in sync,
                 * then emit a placeholder.
                 */
                (void)__builtin_va_arg(args, uint64_t); /* same ABI width as double */
                buf_write(&b, "[float]", 7);
                count += 7;
                break;
            }
            case '%': {
                buf_putc(&b, '%');
                count++;
                break;
            }
            default: {
                /* Unknown specifier: emit '%' + the character verbatim. */
                buf_putc(&b, '%');
                buf_putc(&b, *p);
                count += 2;
                break;
            }
        }
    }

    __builtin_va_end(args);

#ifdef SMP_BATCH
    /* SMP-F3-7: cross-CPU serial LINE lock. With BATCH-class work live on
     * CPU1, both CPUs kprintf concurrently and the per-byte UART interleave
     * SHREDS whole lines ("[INIT] Process 16 exited with status [SYSCALL]
     * sys_stat...") -- which destroyed acceptance evidence (the batchdemo
     * reap line) before it destroyed anything else. Each formatted line is
     * already ONE buffer; serializing the single serial_write below makes
     * lines atomic across CPUs. BOUNDED + best-effort: a same-CPU IRQ that
     * kprintfs while this CPU holds the lock must not self-deadlock, so on
     * spin exhaustion we proceed UNSERIALIZED (= today's behavior, a
     * shredded line) rather than hang. Gated so every non-BATCH build keeps
     * byte-identical printf.c code. */
    {
        static volatile uint32_t kprintf_line_lock = 0;
        uint64_t spins = 0;
        int got = 1;
        while (__atomic_test_and_set(&kprintf_line_lock, __ATOMIC_ACQUIRE)) {
            if (++spins > 3000000ULL) { got = 0; break; }
            __asm__ volatile("pause" ::: "memory");
        }
        if (b.pos > 0)
            serial_write(b.buf, b.pos);
        if (got)
            __atomic_clear(&kprintf_line_lock, __ATOMIC_RELEASE);
    }
#else
    /* Single batched write — one LSR poll loop amortised over the whole line */
    if (b.pos > 0)
        serial_write(b.buf, b.pos);
#endif

    return count;
}
