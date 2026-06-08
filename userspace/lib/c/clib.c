/*
 * clib.c — Mini-libc implementation for x86_64 freestanding userspace
 *
 * Build flags (MUST match):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *       -fno-pic -fno-pie -mno-red-zone -O2 -c clib.c -o clib.o
 *
 * No system headers used anywhere.  All syscalls are done inline.
 * memcpy/memset/memmove are defined as real functions so the compiler can
 * emit calls to them for struct copies even under -fno-builtin.
 */

#include "clib.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Raw x86_64 syscall helper
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Six-argument syscall.  Uses the x86_64 ABI:
 *   rax=nr, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6
 * rcx and r11 are clobbered by the CPU.                                      */
static __attribute__((always_inline)) inline long
sc(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Process helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

void cl_exit(int status)
{
    sc(SYS_EXIT, status, 0, 0, 0, 0, 0);
    for (;;) ; /* unreachable; silences noreturn warning */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Memory primitives
 *     Defined as real symbols so the compiler can call them for struct
 *     copies/zeroing even with -fno-builtin.
 * ═══════════════════════════════════════════════════════════════════════════ */

void *memset(void *dst, int val, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char  v = (unsigned char)val;

    /* Byte-fill until 8-byte aligned. */
    while (n && ((uintptr_t)d & 7)) { *d++ = v; n--; }

    /* Bulk 8-byte stores (REP STOSQ path for the compiler). */
    if (n >= 8) {
        uint64_t w = v;
        w |= w << 8;  w |= w << 16;  w |= w << 32;
        uint64_t *d8 = (uint64_t *)d;
        size_t qwords = n >> 3;
        for (size_t i = 0; i < qwords; i++)
            d8[i] = w;
        size_t done = qwords << 3;
        d += done; n -= done;
    }

    /* Tail bytes. */
    while (n--) *d++ = v;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    /* Byte-copy until 8-byte aligned on the destination. */
    while (n && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }

    /* Bulk 8-byte copies (widest GPR store -> coalesces into WC bursts
     * on UC/WC framebuffer memory; ~8x fewer store instructions). */
    if (n >= 8) {
        uint64_t       *d8 = (uint64_t *)d;
        const uint64_t *s8 = (const uint64_t *)s;
        size_t qwords = n >> 3;
        for (size_t i = 0; i < qwords; i++)
            d8[i] = s8[i];
        size_t done = qwords << 3;
        d += done; s += done; n -= done;
    }

    /* Tail bytes. */
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0)
        return dst;
    if (d < s) {
        /* Forward: use 64-bit bulk when possible. */
        while (n && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }
        if (n >= 8) {
            uint64_t       *d8 = (uint64_t *)d;
            const uint64_t *s8 = (const uint64_t *)s;
            size_t qwords = n >> 3;
            for (size_t i = 0; i < qwords; i++)
                d8[i] = s8[i];
            size_t done = qwords << 3;
            d += done; s += done; n -= done;
        }
        while (n--) *d++ = *s++;
    } else {
        /* Backward copy. */
        d += n; s += n;
        while (n && ((uintptr_t)d & 7)) { *--d = *--s; n--; }
        if (n >= 8) {
            size_t qwords = n >> 3;
            uint64_t       *d8 = (uint64_t *)(d - (qwords << 3));
            const uint64_t *s8 = (const uint64_t *)(s - (qwords << 3));
            for (size_t i = 0; i < qwords; i++)
                d8[i] = s8[i];
            n -= qwords << 3;
            d -= qwords << 3; s -= qwords << 3;
        }
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)a[i] - (int)b[i];
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  String functions
 * ═══════════════════════════════════════════════════════════════════════════ */

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    for (; n; n--, s1++, s2++) {
        if (*s1 != *s2)
            return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
        if (!*s1)
            return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++)) ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++)) ;
    return dst;
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;
    for (; *s; s++)
        if (*s == ch) return (char *)s;
    return (ch == '\0') ? (char *)s : NULL;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (*haystack == needle[0] && strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Number <-> string conversion
 * ═══════════════════════════════════════════════════════════════════════════ */

int atoi(const char *s)
{
    int sign = 1, val = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9')
        val = val * 10 + (*s++ - '0');
    return sign * val;
}

long atol(const char *s)
{
    int sign = 1;
    long val = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9')
        val = val * 10 + (*s++ - '0');
    return sign * val;
}

/* itoa: signed integer to string in any base 2..36. */
char *itoa(int value, char *buf, int base)
{
    char  tmp[66];
    int   i = 0, j = 0;
    unsigned int uval;

    if (base < 2 || base > 36) { buf[0] = '\0'; return buf; }

    if (value < 0 && base == 10) {
        buf[j++] = '-';
        uval = (unsigned int)(-(value + 1)) + 1u; /* avoids INT_MIN overflow */
    } else {
        uval = (unsigned int)value;
    }

    if (uval == 0) {
        tmp[i++] = '0';
    } else {
        while (uval) {
            unsigned int digit = uval % (unsigned int)base;
            tmp[i++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
            uval /= (unsigned int)base;
        }
    }
    while (i > 0)
        buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

/* utoa: unsigned long to string in any base 2..36. */
char *utoa(unsigned long value, char *buf, int base)
{
    char  tmp[66];
    int   i = 0, j = 0;

    if (base < 2 || base > 36) { buf[0] = '\0'; return buf; }

    if (value == 0) {
        tmp[i++] = '0';
    } else {
        unsigned long b = (unsigned long)base;
        while (value) {
            unsigned long digit = value % b;
            tmp[i++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
            value /= b;
        }
    }
    while (i > 0)
        buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

/* ultohex: write exactly 'digits' hex nibbles (no NUL). */
void ultohex(unsigned long value, char *buf, int digits, int uppercase)
{
    const char *lo = "0123456789abcdef";
    const char *up = "0123456789ABCDEF";
    const char *t  = uppercase ? up : lo;
    for (int i = digits - 1; i >= 0; i--) {
        buf[i] = t[value & 0xf];
        value >>= 4;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  vsnprintf / snprintf / printf
 *
 * Supported conversions:
 *   %d %i  — signed decimal   (int; %ld long; %lld long long)
 *   %u     — unsigned decimal (unsigned int; %lu; %llu)
 *   %x     — lowercase hex    (unsigned int; %lx; %llx)
 *   %X     — uppercase hex    (unsigned int; %lX; %llX)
 *   %p     — pointer (0x + 16 hex digits)
 *   %s     — string (NULL → "(null)")
 *   %c     — character
 *   %%     — literal %
 *
 * Width & zero-pad:  %08x  %5d  %10s  (left-pad; no '-' flag for now)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Internal sink ─ tracks buffer + count separately so we correctly report
 * the would-be length even when the buffer is full. */
typedef struct {
    char   *buf;
    size_t  cap;   /* total capacity including NUL slot */
    size_t  pos;   /* chars written so far (not counting NUL) */
} Sink;

static void sink_put(Sink *s, char c)
{
    if (s->buf && s->pos < s->cap - 1)
        s->buf[s->pos] = c;
    s->pos++;
}

static void sink_pad(Sink *s, char pad, int n)
{
    for (int i = 0; i < n; i++)
        sink_put(s, pad);
}

static void sink_str(Sink *s, const char *str, int width, char pad)
{
    if (!str) str = "(null)";
    int len = (int)strlen(str);
    int extra = width - len;
    if (extra > 0) sink_pad(s, pad, extra);
    for (int i = 0; i < len; i++)
        sink_put(s, str[i]);
}

/* Emit an unsigned 64-bit integer in the requested base.
 * Returns the number of digit characters that would be emitted. */
static void sink_uint(Sink *s, unsigned long long v, int base,
                      int uppercase, int width, char pad)
{
    char tmp[66];
    int  i = 0;
    const char *lo = "0123456789abcdef";
    const char *up = "0123456789ABCDEF";
    const char *t  = uppercase ? up : lo;

    if (v == 0) {
        tmp[i++] = '0';
    } else {
        unsigned long long b = (unsigned long long)base;
        while (v) {
            tmp[i++] = t[(int)(v % b)];
            v /= b;
        }
    }

    int extra = width - i;
    if (extra > 0) sink_pad(s, pad, extra);
    while (i > 0)
        sink_put(s, tmp[--i]);
}

static void sink_sint(Sink *s, long long v, int base,
                      int uppercase, int width, char pad)
{
    if (v < 0) {
        /* If zero-padding, put '-' before the zeros */
        if (pad == '0' && width > 0) {
            sink_put(s, '-');
            width--;
            sink_uint(s, (unsigned long long)(-(v + 1)) + 1ULL,
                      base, uppercase, width, pad);
        } else {
            sink_put(s, '-');
            sink_uint(s, (unsigned long long)(-(v + 1)) + 1ULL,
                      base, uppercase, width - 1, pad);
        }
    } else {
        sink_uint(s, (unsigned long long)v, base, uppercase, width, pad);
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    Sink s;
    s.buf = buf;
    s.cap = (buf && size > 0) ? size : 1; /* guard against size==0 */
    s.pos = 0;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { sink_put(&s, *p); continue; }
        p++;
        if (!*p) break;

        /* ── flags ── */
        char pad = ' ';
        if (*p == '0') { pad = '0'; p++; }

        /* ── width ── (may start with any digit now that leading '0' was consumed) */
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        /* ignore precision for now */
        if (*p == '.') {
            p++;
            while (*p >= '0' && *p <= '9') p++;
        }

        /* ── length modifier ── */
        int lmod = 0; /* 0=int, 1=long, 2=long long */
        while (*p == 'l') { lmod++; p++; }

        /* ── conversion ── */
        switch (*p) {
        case 'd': case 'i': {
            long long v = (lmod >= 2) ? va_arg(ap, long long)
                        : (lmod == 1) ? (long long)va_arg(ap, long)
                                      : (long long)va_arg(ap, int);
            sink_sint(&s, v, 10, 0, width, pad);
            break;
        }
        case 'u': {
            unsigned long long v = (lmod >= 2) ? va_arg(ap, unsigned long long)
                                 : (lmod == 1) ? (unsigned long long)va_arg(ap, unsigned long)
                                               : (unsigned long long)va_arg(ap, unsigned int);
            sink_uint(&s, v, 10, 0, width, pad);
            break;
        }
        case 'x': {
            unsigned long long v = (lmod >= 2) ? va_arg(ap, unsigned long long)
                                 : (lmod == 1) ? (unsigned long long)va_arg(ap, unsigned long)
                                               : (unsigned long long)va_arg(ap, unsigned int);
            sink_uint(&s, v, 16, 0, width, pad);
            break;
        }
        case 'X': {
            unsigned long long v = (lmod >= 2) ? va_arg(ap, unsigned long long)
                                 : (lmod == 1) ? (unsigned long long)va_arg(ap, unsigned long)
                                               : (unsigned long long)va_arg(ap, unsigned int);
            sink_uint(&s, v, 16, 1, width, pad);
            break;
        }
        case 'p': {
            unsigned long long addr = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            sink_put(&s, '0'); sink_put(&s, 'x');
            sink_uint(&s, addr, 16, 0, 16, '0');
            break;
        }
        case 's': {
            const char *sv = va_arg(ap, const char *);
            sink_str(&s, sv, width, pad);
            break;
        }
        case 'c': {
            char cv = (char)va_arg(ap, int);
            if (width > 1) sink_pad(&s, ' ', width - 1);
            sink_put(&s, cv);
            break;
        }
        case '%':
            sink_put(&s, '%');
            break;
        default:
            sink_put(&s, '%');
            sink_put(&s, *p);
            break;
        }
    }

    /* NUL-terminate if there is any buffer space at all */
    if (buf && size > 0) {
        size_t idx = (s.pos < size) ? s.pos : size - 1;
        buf[idx] = '\0';
    }
    return (int)s.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

/* printf: format to a stack buffer, then write to fd 1. */
void printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
        sc(SYS_WRITE, STDOUT_FILENO, (long)buf, (long)len, 0, 0, 0);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  File I/O wrappers
 * ═══════════════════════════════════════════════════════════════════════════ */

int cl_open(const char *path, int flags, int mode)
{
    return (int)sc(SYS_OPEN, (long)path, flags, mode, 0, 0, 0);
}

long cl_read(int fd, void *buf, size_t count)
{
    return sc(SYS_READ, fd, (long)buf, (long)count, 0, 0, 0);
}

long cl_write(int fd, const void *buf, size_t count)
{
    return sc(SYS_WRITE, fd, (long)buf, (long)count, 0, 0, 0);
}

int cl_close(int fd)
{
    return (int)sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
}

int cl_getline(int fd, char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0) return -1;
    size_t n = 0;
    while (n < bufsz - 1) {
        char c;
        long r = sc(SYS_READ, fd, (long)&c, 1, 0, 0, 0);
        if (r <= 0) break;
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = '\0';
    return (int)n;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Heap allocator
 *
 * Design
 * ──────
 *  • Backed by SYS_MMAP=37: sc(37, hint=0, len, prot=3, flags=0x22, 0, 0)
 *    where prot=3 means PROT_READ|PROT_WRITE, flags=0x22 = MAP_PRIVATE|MAP_ANON.
 *    The kernel uses a 4-arg form (hint, len, prot, flags), which maps onto
 *    a1..a4 in our syscall ABI.
 *
 *  • Single linked-list of arenas.  Each arena begins with an Arena header
 *    followed by a chain of Block headers and their payloads.
 *
 *  • Blocks are first-fit; on free, forward-coalesce.  16-byte payload align.
 *
 *  • Block header = 32 bytes (size + free + magic + next + 8-byte pad).
 *    With the header a multiple of 16 and the arena itself page-aligned,
 *    every payload pointer is ≡ 0 (mod 16).
 *
 *  • New arenas are grown on demand (min 2 MB each).
 *
 *  • Magic cookie provides cheap double-free / heap-corruption detection.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define HEAP_MAGIC      0xA110CA7EUL
#define HEAP_MIN_ARENA  (2u * 1024u * 1024u)   /* 2 MB per mmap call  */
#define HEAP_ALIGN      16u
#define HEAP_MIN_SPLIT  32u                     /* don't split tiny remnants */

/* ── Block header: exactly 32 bytes ── */
typedef struct Block Block;
struct Block {
    size_t   size;    /* payload size in bytes (multiple of HEAP_ALIGN) */
    unsigned free;    /* 1 = available, 0 = allocated                   */
    unsigned magic;   /* HEAP_MAGIC                                      */
    Block   *next;    /* next block in this arena chain, or NULL         */
    unsigned char _pad[8];
};
typedef char _blk_size_check[ (sizeof(Block) == 32) ? 1 : -1 ];

/* ── Arena header: linked list of mmap'd regions ── */
/* Padded to 32 bytes so the first Block that follows is 32-byte aligned,
 * and the first payload (Block+32) is 16-byte aligned on a page base.   */
typedef struct Arena Arena;
struct Arena {
    Arena  *next;          /* next arena                                 */
    size_t  capacity;      /* total usable bytes after the Arena header  */
    Block  *blocks;        /* first Block in this arena                  */
    unsigned char _pad[8]; /* pad to 32 bytes                            */
};

static Arena *heap_arenas = NULL;  /* NULL = no arenas yet */

static inline void *blk_payload(Block *b) {
    return (void *)((unsigned char *)b + sizeof(Block));
}
static inline Block *payload_blk(void *ptr) {
    return (Block *)((unsigned char *)ptr - sizeof(Block));
}

/* Round up to HEAP_ALIGN multiple. */
static inline size_t align_up(size_t n) {
    return (n + (HEAP_ALIGN - 1u)) & ~(HEAP_ALIGN - 1u);
}

/* Coalesce b with its free successor(s). */
static void coalesce_forward(Block *b)
{
    while (b->next && b->next->free) {
        b->size += sizeof(Block) + b->next->size;
        b->next  = b->next->next;
    }
}

/* Allocate a new arena of at least `need` payload bytes via mmap. */
static Arena *arena_new(size_t need)
{
    size_t total = need + sizeof(Arena) + sizeof(Block);
    if (total < HEAP_MIN_ARENA)
        total = HEAP_MIN_ARENA;
    /* round up to page (4 KB) */
    total = (total + 4095u) & ~4095u;

    /* prot = PROT_READ|PROT_WRITE = 3; flags = MAP_PRIVATE|MAP_ANON = 0x22 */
    void *ptr = (void *)sc(SYS_MMAP, 0, (long)total, 3, 0x22, 0, 0);

    /* mmap returns a small positive value or -1 on error; valid addresses
     * are >= 4096 on this kernel.  Cast to uintptr_t for the comparison.  */
    if ((uintptr_t)ptr < 4096u)
        return NULL;

    Arena *a = (Arena *)ptr;
    a->next     = NULL;
    a->capacity = total - sizeof(Arena);

    /* Carve the initial single free block */
    Block *b = (Block *)((unsigned char *)ptr + sizeof(Arena));
    b->size  = a->capacity - sizeof(Block);
    b->free  = 1;
    b->magic = HEAP_MAGIC;
    b->next  = NULL;
    a->blocks = b;

    return a;
}

void *malloc(size_t size)
{
    if (size == 0) return NULL;
    size = align_up(size);

    /* Walk existing arenas first-fit */
    for (Arena *a = heap_arenas; a; a = a->next) {
        for (Block *b = a->blocks; b; b = b->next) {
            if (!b->free || b->size < size)
                continue;
            /* Split if there's room for a useful remainder */
            if (b->size >= size + sizeof(Block) + HEAP_MIN_SPLIT) {
                Block *r  = (Block *)((unsigned char *)blk_payload(b) + size);
                r->size   = b->size - size - sizeof(Block);
                r->free   = 1;
                r->magic  = HEAP_MAGIC;
                r->next   = b->next;
                b->next   = r;
                b->size   = size;
            }
            b->free = 0;
            return blk_payload(b);
        }
    }

    /* No fit — grow the heap */
    Arena *a = arena_new(size + sizeof(Block));
    if (!a) return NULL;

    /* Append to arena list */
    if (!heap_arenas) {
        heap_arenas = a;
    } else {
        Arena *tail = heap_arenas;
        while (tail->next) tail = tail->next;
        tail->next = a;
    }

    /* Now retry allocation in the fresh arena */
    Block *b = a->blocks;
    if (!b->free || b->size < size)
        return NULL; /* shouldn't happen */

    if (b->size >= size + sizeof(Block) + HEAP_MIN_SPLIT) {
        Block *r  = (Block *)((unsigned char *)blk_payload(b) + size);
        r->size   = b->size - size - sizeof(Block);
        r->free   = 1;
        r->magic  = HEAP_MAGIC;
        r->next   = b->next;
        b->next   = r;
        b->size   = size;
    }
    b->free = 0;
    return blk_payload(b);
}

void free(void *ptr)
{
    if (!ptr) return;
    Block *b = payload_blk(ptr);
    if (b->magic != HEAP_MAGIC || b->free)
        return; /* corrupt / double-free — silently ignore */
    b->free = 1;
    coalesce_forward(b);

    /* Backward coalesce: find predecessor within the same arena block chain */
    for (Arena *a = heap_arenas; a; a = a->next) {
        Block *prev = NULL;
        for (Block *cur = a->blocks; cur; cur = cur->next) {
            if (cur == b) {
                if (prev && prev->free)
                    coalesce_forward(prev);
                goto done;
            }
            prev = cur;
        }
    }
done:;
}

void *calloc(size_t nmemb, size_t size)
{
    /* Overflow check */
    if (nmemb && size > (size_t)-1 / nmemb) return NULL;
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    Block *b = payload_blk(ptr);
    if (b->magic != HEAP_MAGIC) return NULL;

    size_t asize = align_up(size);
    if (b->size >= asize)
        return ptr; /* already fits — no move needed */

    void *np = malloc(size);
    if (!np) return NULL;
    size_t copy = (b->size < size) ? b->size : size;
    memcpy(np, ptr, copy);
    free(ptr);
    return np;
}
