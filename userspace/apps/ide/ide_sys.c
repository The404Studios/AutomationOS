/*
 * ide_sys.c -- shared syscall ABI + file/dir IO + string helpers.
 * Freestanding: no libc.
 */
#include "ide_sys.h"

#define SYS_EXIT          0
#define SYS_READ          2
#define SYS_WRITE         3
#define SYS_OPEN          4
#define SYS_CLOSE         5
#define SYS_YIELD         15
#define SYS_OPENDIR       30
#define SYS_READDIR       31
#define SYS_CLOSEDIR      32
#define SYS_GET_TICKS_MS  40

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_CREAT  0x0040
#define O_TRUNC  0x0200

long ide_sc(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5, r9 asm("r9") = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

int ide_read_file(const char* path, char* buf, int cap) {
    if (!path || !buf || cap <= 0) return -1;
    long fd = ide_sc(SYS_OPEN, (long)path, O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) return (int)fd;
    int total = 0;
    while (total < cap) {
        long n = ide_sc(SYS_READ, fd, (long)(buf + total), cap - total, 0, 0, 0);
        if (n <= 0) break;
        total += (int)n;
    }
    ide_sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return total;
}

int ide_write_file(const char* path, const char* buf, int len) {
    if (!path || !buf || len < 0) return -1;
    long fd = ide_sc(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0);
    if (fd < 0) return (int)fd;
    int total = 0;
    while (total < len) {
        long n = ide_sc(SYS_WRITE, fd, (long)(buf + total), len - total, 0, 0, 0);
        if (n <= 0) break;
        total += (int)n;
    }
    ide_sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return (total == len) ? 0 : -1;
}

int ide_list_dir(const char* path, IdeDirent* out, int max) {
    if (!path || !out || max <= 0) return -1;
    long fd = ide_sc(SYS_OPENDIR, (long)path, 0, 0, 0, 0, 0);
    if (fd < 0) return (int)fd;
    int n = 0;
    while (n < max) {
        long r = ide_sc(SYS_READDIR, fd, (long)&out[n], 0, 0, 0, 0);
        if (r != 0) break;     /* 0 = entry filled; <0 = end/error */
        if (out[n].name[0] == '\0') continue;  /* skip empty slot */
        n++;
    }
    ide_sc(SYS_CLOSEDIR, fd, 0, 0, 0, 0, 0);
    return n;
}

long ide_ticks_ms(void) { return ide_sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0); }
void ide_exit(int code) { ide_sc(SYS_EXIT, code, 0, 0, 0, 0, 0); for (;;) {} }

int ide_strlen(const char* s) { int n = 0; if (!s) return 0; while (s[n]) n++; return n; }
int ide_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
int ide_strneq(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0) return 1;
    }
    return 1;
}
void ide_strlcpy(char* d, const char* s, int cap) {
    int i = 0;
    if (cap <= 0) return;
    if (s) while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
int ide_itoa(int v, char* out) {
    char tmp[16]; int i = 0, len = 0; int neg = 0;
    unsigned int u;
    if (v < 0) { neg = 1; u = (unsigned int)(-(long)v); } else u = (unsigned int)v;
    do { tmp[i++] = (char)('0' + (u % 10)); u /= 10; } while (u);
    if (neg) out[len++] = '-';
    while (i > 0) out[len++] = tmp[--i];
    out[len] = 0;
    return len;
}
