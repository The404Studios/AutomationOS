/*
 * clib.h — Mini-libc public API for x86_64 freestanding userspace
 *
 * Freestanding, no system headers.  Include this and link clib.c.
 * All pointers are 16-byte aligned.  malloc is backed by SYS_MMAP=37.
 */

#ifndef CLIB_H
#define CLIB_H

/* ── Primitive types ──────────────────────────────────────────────────────── */
typedef unsigned long  size_t;
typedef long           ssize_t;
typedef long           off_t;
typedef unsigned long  uintptr_t;
typedef long           intptr_t;
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef unsigned long  uint64_t;
typedef signed char    int8_t;
typedef short          int16_t;
typedef int            int32_t;
typedef long           int64_t;

#define NULL ((void *)0)
#define EOF  (-1)

/* ── Syscall numbers (must match kernel/include/syscall.h) ───────────────── */
#define SYS_EXIT    0
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_OPEN    4
#define SYS_CLOSE   5
#define SYS_MMAP    37
#define SYS_MUNMAP  38

/* ── Open flags ───────────────────────────────────────────────────────────── */
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

/* ── Standard file descriptors ───────────────────────────────────────────── */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* ── va_list (built-in; no <stdarg.h> needed) ────────────────────────────── */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)
#define va_copy(dst, src)  __builtin_va_copy(dst, src)

/* ═══════════════════════════════════════════════════════════════════════════
 * Memory allocation  (mmap-backed free-list, 16-byte aligned)
 * ═══════════════════════════════════════════════════════════════════════════ */
void *malloc (size_t size);
void  free   (void  *ptr);
void *calloc (size_t nmemb, size_t size);
void *realloc(void  *ptr,   size_t size);

/* ═══════════════════════════════════════════════════════════════════════════
 * String & memory primitives
 * ═══════════════════════════════════════════════════════════════════════════ */
size_t strlen (const char *s);
int    strcmp (const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);
char  *strcpy (char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat (char *dst, const char *src);
char  *strchr (const char *s, int c);
char  *strstr (const char *haystack, const char *needle);

void  *memcpy (void *dst,       const void *src, size_t n);
void  *memset (void *dst,       int val,          size_t n);
void  *memmove(void *dst,       const void *src, size_t n);
int    memcmp (const void *s1, const void *s2,   size_t n);

/* ═══════════════════════════════════════════════════════════════════════════
 * Number <-> string conversion
 * ═══════════════════════════════════════════════════════════════════════════ */
int  atoi(const char *s);
long atol(const char *s);

/* itoa: convert signed integer to string, base 2..36.
 * buf must be at least 66 bytes.  Returns buf. */
char *itoa(int value, char *buf, int base);

/* utoa: unsigned variant.  buf must be at least 66 bytes. */
char *utoa(unsigned long value, char *buf, int base);

/* ultohex: write exactly 'digits' hex nibbles into buf (no NUL appended
 * unless caller adds it).  Handy for %p-style output. */
void  ultohex(unsigned long value, char *buf, int digits, int uppercase);

/* ═══════════════════════════════════════════════════════════════════════════
 * Formatted output
 *
 * Specifiers: %d %i %u %x %X %p %s %c %%
 * Length mods: l (long), ll (long long)
 * Flags/width: optional width, optional 0-pad  e.g. "%08x"
 * ═══════════════════════════════════════════════════════════════════════════ */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int  snprintf(char *buf, size_t size, const char *fmt, ...);

/* printf writes to fd 1 (STDOUT).  Returns chars written. */
void printf(const char *fmt, ...);

/* ═══════════════════════════════════════════════════════════════════════════
 * Simple file I/O wrappers  (thin syscall shims)
 * ═══════════════════════════════════════════════════════════════════════════ */
int    cl_open (const char *path, int flags, int mode);
long   cl_read (int fd, void *buf, size_t count);
long   cl_write(int fd, const void *buf, size_t count);
int    cl_close(int fd);

/* getline: read one '\n'-terminated line (or up to bufsz-1 chars) from fd.
 * Returns chars read (>=0) or -1 on error/EOF.  Always NUL-terminates. */
int    cl_getline(int fd, char *buf, size_t bufsz);

/* ═══════════════════════════════════════════════════════════════════════════
 * Process helpers
 * ═══════════════════════════════════════════════════════════════════════════ */
void cl_exit(int status) __attribute__((noreturn));

#endif /* CLIB_H */
