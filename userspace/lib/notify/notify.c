/*
 * userspace/lib/notify/notify.c — Desktop toast-notification client library
 *
 * Freestanding (no libc, no headers beyond our own).
 *
 * Build command (as given):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone \
 *       -O2 -c userspace/lib/notify/notify.c -o /tmp/un.o
 */

#include "notify.h"

/* ---- freestanding type aliases ---- */
typedef unsigned long  size_t;
typedef long           ssize_t;

/* ---- raw x86-64 syscall helper ---- */

static inline long sc(long nr, long a1, long a2, long a3)
{
    long ret;
    register long r10 asm("r10") = a3;
    asm volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"((long)0), "r"(r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* ---- tiny freestanding string helpers ---- */

static size_t ns_strlen(const char *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void ns_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

/* ---- public API ---- */

/*
 * notify() — post a toast notification.
 *
 * Packs "title\0body\0" into a stack buffer and calls SYS_NOTIFY (65).
 * Silently returns on -ENOSYS (unimplemented) or queue-full.
 */
void notify(const char *title, const char *body)
{
    /* Treat NULL as empty string */
    if (!title) title = "";
    if (!body)  body  = "";

    size_t tlen = ns_strlen(title);
    size_t blen = ns_strlen(body);

    /*
     * Enforce NOTIFY_PAYLOAD_MAX (128 bytes including both NULs).
     * Truncate body first; if title alone is already too long, truncate it
     * to leave at least one NUL for body.
     */
    /* Reserve 2 bytes for the two NUL terminators */
    size_t max_content = NOTIFY_PAYLOAD_MAX - 2;

    if (tlen > max_content) {
        tlen = max_content;
        blen = 0;
    } else if (tlen + blen > max_content) {
        blen = max_content - tlen;
    }

    /* Build the payload on the stack */
    char buf[NOTIFY_PAYLOAD_MAX];
    size_t pos = 0;

    ns_memcpy(buf + pos, title, tlen);
    pos += tlen;
    buf[pos++] = '\0';

    ns_memcpy(buf + pos, body, blen);
    pos += blen;
    buf[pos++] = '\0';

    long ret = sc(SYS_NOTIFY, (long)buf, (long)pos, 0);

    /*
     * Graceful degradation:
     *   -ENOSYS (-38) => syscall not wired yet, just ignore
     *   -ENOSPC (-28) => queue full, drop silently
     *   other errors  => drop silently (fire-and-forget)
     */
    (void)ret;
}

/*
 * notify_poll() — dequeue one notification into a caller buffer.
 *
 * Returns byte count written, 0 if empty, or negative on error.
 * The buffer will contain "title\0body\0".
 */
long notify_poll(void *buf, unsigned int max)
{
    return sc(SYS_NOTIFY_POLL, (long)buf, (long)max, 0);
}