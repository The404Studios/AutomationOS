/*
 * Optimized String Functions for AutomationOS
 * ===========================================
 *
 * High-performance implementations using 64-bit word operations.
 *
 * Strategy for memcpy / memset / memmove:
 *  1. Byte-wise head: advance dst to the next 8-byte boundary (0-7 bytes).
 *  2. Word body:      copy/set using uint64_t (8 bytes per iteration),
 *                     4x unrolled for instruction-level parallelism.
 *     NOTE: src reads are intentionally unaligned — x86 handles them at
 *     full speed; only the *store* alignment matters for performance.
 *  3. Byte-wise tail: copy/set the remaining 0-7 bytes.
 *
 * memmove backward path uses the same alignment trick in reverse: align
 * dst-end to an 8-byte boundary working toward lower addresses, then
 * 4x-unrolled word stores, then byte tail.
 *
 * No SSE / FP used — pure integer, freestanding, no-builtin safe.
 */

#include "../include/types.h"

/* -----------------------------------------------------------------------
 * memcpy — non-overlapping memory copy
 * ----------------------------------------------------------------------- */
void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t*       d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;

    if (!n) return dest;

    /* --- Head: align dst to 8-byte boundary --- */
    while (((uintptr_t)d & 7) && n) {
        *d++ = *s++;
        n--;
    }

    /* --- Body: 64-bit words, 4x unrolled --- */
    {
        uint64_t*       d64 = (uint64_t*)d;
        const uint64_t* s64 = (const uint64_t*)s;
        size_t quads = n >> 5;          /* 32-byte chunks (4 × 8) */
        n &= 31;

        while (quads--) {
            d64[0] = s64[0];
            d64[1] = s64[1];
            d64[2] = s64[2];
            d64[3] = s64[3];
            d64 += 4;
            s64 += 4;
        }

        /* Remaining whole 8-byte words (0-3) */
        size_t words = n >> 3;
        n &= 7;
        while (words--) {
            *d64++ = *s64++;
        }

        d = (uint8_t*)d64;
        s = (const uint8_t*)s64;
    }

    /* --- Tail: remaining bytes (0-7) --- */
    while (n--) {
        *d++ = *s++;
    }

    return dest;
}

/* -----------------------------------------------------------------------
 * memset — fill memory with a byte value
 * ----------------------------------------------------------------------- */
void* memset(void* dest, int val, size_t n) {
    uint8_t* d        = (uint8_t*)dest;
    uint8_t  byte_val = (uint8_t)val;

    if (!n) return dest;

    /* --- Head: align dst to 8-byte boundary --- */
    while (((uintptr_t)d & 7) && n) {
        *d++ = byte_val;
        n--;
    }

    /* --- Build 64-bit broadcast pattern (e.g. 0x42 → 0x4242424242424242) --- */
    uint64_t pattern = (uint64_t)byte_val;
    pattern |= pattern <<  8;
    pattern |= pattern << 16;
    pattern |= pattern << 32;

    /* --- Body: 64-bit words, 4x unrolled --- */
    {
        uint64_t* d64   = (uint64_t*)d;
        size_t    quads = n >> 5;
        n &= 31;

        while (quads--) {
            d64[0] = pattern;
            d64[1] = pattern;
            d64[2] = pattern;
            d64[3] = pattern;
            d64 += 4;
        }

        size_t words = n >> 3;
        n &= 7;
        while (words--) {
            *d64++ = pattern;
        }

        d = (uint8_t*)d64;
    }

    /* --- Tail --- */
    while (n--) {
        *d++ = byte_val;
    }

    return dest;
}

/* -----------------------------------------------------------------------
 * memmove — memory move (handles overlapping src/dst correctly)
 *
 * Overlap rule:
 *   dst <= src  OR  dst >= src+n  → no overlap → forward copy (like memcpy)
 *   dst >  src  AND dst <  src+n  → overlap, dst > src → backward copy
 * ----------------------------------------------------------------------- */
void* memmove(void* dest, const void* src, size_t n) {
    uint8_t*       d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;

    if (!n || d == s) return dest;

    if (d < s || d >= s + n) {
        /* Forward copy — identical to memcpy body (no recursive call to
         * avoid any chance of the compiler tail-calling through a PLT stub
         * in a weird freestanding build). */

        /* Head */
        while (((uintptr_t)d & 7) && n) { *d++ = *s++; n--; }

        /* Body 4x unrolled */
        {
            uint64_t*       d64   = (uint64_t*)d;
            const uint64_t* s64   = (const uint64_t*)s;
            size_t          quads = n >> 5;
            n &= 31;
            while (quads--) {
                d64[0] = s64[0]; d64[1] = s64[1];
                d64[2] = s64[2]; d64[3] = s64[3];
                d64 += 4; s64 += 4;
            }
            size_t words = n >> 3; n &= 7;
            while (words--) *d64++ = *s64++;
            d = (uint8_t*)d64;
            s = (const uint8_t*)s64;
        }

        /* Tail */
        while (n--) *d++ = *s++;

    } else {
        /* Backward copy: dst > src and ranges overlap.
         * Start from the end and work toward lower addresses.
         * Align the dst *end* pointer to an 8-byte boundary first. */

        d += n;
        s += n;

        /* Head (toward lower addresses): align dst to 8-byte boundary */
        while (((uintptr_t)d & 7) && n) {
            *--d = *--s;
            n--;
        }

        /* Body: 64-bit words, 4x unrolled, walking backward */
        {
            uint64_t*       d64   = (uint64_t*)d;
            const uint64_t* s64   = (const uint64_t*)s;
            size_t          quads = n >> 5;
            n &= 31;

            while (quads--) {
                d64 -= 4; s64 -= 4;
                d64[3] = s64[3];
                d64[2] = s64[2];
                d64[1] = s64[1];
                d64[0] = s64[0];
            }

            size_t words = n >> 3; n &= 7;
            while (words--) *--d64 = *--s64;

            d = (uint8_t*)d64;
            s = (const uint8_t*)s64;
        }

        /* Tail */
        while (n--) *--d = *--s;
    }

    return dest;
}

/* -----------------------------------------------------------------------
 * memcmp — compare two memory regions
 * Returns < 0, 0, > 0 (like standard memcmp)
 * ----------------------------------------------------------------------- */
int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* a = (const uint8_t*)s1;
    const uint8_t* b = (const uint8_t*)s2;

    while (n--) {
        if (*a != *b) {
            return (int)*a - (int)*b;
        }
        a++;
        b++;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * strlen — length of null-terminated string
 * ----------------------------------------------------------------------- */
size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

/* -----------------------------------------------------------------------
 * strcmp / strncmp
 * ----------------------------------------------------------------------- */
int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const uint8_t*)s1 - *(const uint8_t*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++; s2++; n--;
    }
    if (n == 0) return 0;
    return *(const uint8_t*)s1 - *(const uint8_t*)s2;
}

/* -----------------------------------------------------------------------
 * strcpy / strncpy
 * ----------------------------------------------------------------------- */
char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    char* d = dest;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dest;
}

/* -----------------------------------------------------------------------
 * strstr — find first occurrence of needle in haystack
 * ----------------------------------------------------------------------- */
char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)haystack;
    }
    return (char*)0;
}

/* -----------------------------------------------------------------------
 * strcat / strncat
 * ----------------------------------------------------------------------- */
char* strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char* strncat(char* dest, const char* src, size_t n) {
    char* d = dest;
    while (*d) d++;
    while (n-- && (*d = *src++)) d++;
    *d = '\0';
    return dest;
}

/* -----------------------------------------------------------------------
 * strnlen / strchr / strrchr
 * ----------------------------------------------------------------------- */
size_t strnlen(const char* str, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && str[len]) len++;
    return len;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return (c == '\0') ? (char*)s : (char*)0;
}

char* strrchr(const char* s, int c) {
    const char* last = (char*)0;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (c == '\0') ? (char*)s : (char*)last;
}

/* -----------------------------------------------------------------------
 * vsnprintf — formatted print to buffer with va_list
 *
 * Supports: %d, %i, %u, %x, %X, %lx, %lu, %ld, %llx, %llu, %lld,
 *           %s, %c, %p, %%  with width, zero-pad, left-align, precision.
 * ----------------------------------------------------------------------- */
int vsnprintf(char* buf, size_t size, const char* fmt, __builtin_va_list args) {
    if (!buf || size == 0) return 0;

    size_t pos   = 0;
    size_t limit = size - 1;   /* reserve space for NUL */

    #define PUTC(ch) do { if (pos < limit) buf[pos] = (ch); pos++; } while (0)

    while (*fmt) {
        if (*fmt != '%') {
            PUTC(*fmt);
            fmt++;
            continue;
        }
        fmt++;   /* skip '%' */

        /* flags */
        int zero_pad   = 0;
        int left_align = 0;
        while (*fmt == '0' || *fmt == '-') {
            if (*fmt == '0') zero_pad   = 1;
            if (*fmt == '-') left_align = 1;
            fmt++;
        }

        /* width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* precision */
        int has_precision = 0;
        int precision     = 0;
        if (*fmt == '.') {
            has_precision = 1;
            fmt++;
            if (*fmt == '*') {
                precision = __builtin_va_arg(args, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        /* length modifier */
        int long_flag = 0;
        while (*fmt == 'l') { long_flag++; fmt++; }
        if (*fmt == 'z') { long_flag = 1; fmt++; }  /* size_t */

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t val;
            if (long_flag >= 2)     val = __builtin_va_arg(args, int64_t);
            else if (long_flag == 1) val = __builtin_va_arg(args, long);
            else                     val = __builtin_va_arg(args, int);

            char tmp[24]; int ti = 0;
            uint64_t uval;
            if (val < 0) { PUTC('-'); uval = (uint64_t)(-val); width--; }
            else           uval = (uint64_t)val;
            if (uval == 0) tmp[ti++] = '0';
            else while (uval) { tmp[ti++] = '0' + (uval % 10); uval /= 10; }
            int pad = width - ti;
            if (!left_align) while (pad-- > 0) PUTC(zero_pad ? '0' : ' ');
            while (ti--) PUTC(tmp[ti]);
            if (left_align)  while (pad-- > 0) PUTC(' ');
            break;
        }
        case 'u': {
            uint64_t val;
            if (long_flag >= 2)     val = __builtin_va_arg(args, uint64_t);
            else if (long_flag == 1) val = __builtin_va_arg(args, unsigned long);
            else                     val = __builtin_va_arg(args, unsigned int);

            char tmp[24]; int ti = 0;
            if (val == 0) tmp[ti++] = '0';
            else while (val) { tmp[ti++] = '0' + (val % 10); val /= 10; }
            int pad = width - ti;
            if (!left_align) while (pad-- > 0) PUTC(zero_pad ? '0' : ' ');
            while (ti--) PUTC(tmp[ti]);
            if (left_align)  while (pad-- > 0) PUTC(' ');
            break;
        }
        case 'x':
        case 'X': {
            uint64_t val;
            if (long_flag >= 2)     val = __builtin_va_arg(args, uint64_t);
            else if (long_flag == 1) val = __builtin_va_arg(args, unsigned long);
            else                     val = __builtin_va_arg(args, unsigned int);

            const char* hex = (*fmt == 'X') ? "0123456789ABCDEF"
                                             : "0123456789abcdef";
            char tmp[20]; int ti = 0;
            if (val == 0) tmp[ti++] = '0';
            else while (val) { tmp[ti++] = hex[val & 0xF]; val >>= 4; }
            int pad = width - ti;
            if (!left_align) while (pad-- > 0) PUTC(zero_pad ? '0' : ' ');
            while (ti--) PUTC(tmp[ti]);
            if (left_align)  while (pad-- > 0) PUTC(' ');
            break;
        }
        case 'p': {
            uint64_t val = (uint64_t)__builtin_va_arg(args, void*);
            PUTC('0'); PUTC('x');
            char tmp[20]; int ti = 0;
            if (val == 0) tmp[ti++] = '0';
            else while (val) { tmp[ti++] = "0123456789abcdef"[val & 0xF]; val >>= 4; }
            while (ti--) PUTC(tmp[ti]);
            break;
        }
        case 's': {
            const char* s = __builtin_va_arg(args, const char*);
            if (!s) s = "(null)";
            int slen = 0;
            while (s[slen]) slen++;
            if (has_precision && precision < slen) slen = precision;
            int pad = width - slen;
            if (!left_align) while (pad-- > 0) PUTC(' ');
            for (int j = 0; j < slen; j++) PUTC(s[j]);
            if (left_align)  while (pad-- > 0) PUTC(' ');
            break;
        }
        case 'c': {
            char c = (char)__builtin_va_arg(args, int);
            PUTC(c);
            break;
        }
        case '%':
            PUTC('%');
            break;
        default:
            PUTC('%');
            PUTC(*fmt);
            break;
        }
        fmt++;
    }

    #undef PUTC

    if (pos < size) buf[pos] = '\0';
    else            buf[size - 1] = '\0';

    return (int)pos;
}

/* -----------------------------------------------------------------------
 * snprintf / ksnprintf
 * ----------------------------------------------------------------------- */
int snprintf(char* buf, size_t size, const char* fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int ret = vsnprintf(buf, size, fmt, args);
    __builtin_va_end(args);
    return ret;
}

int ksnprintf(char* buf, size_t size, const char* fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int ret = vsnprintf(buf, size, fmt, args);
    __builtin_va_end(args);
    return ret;
}
