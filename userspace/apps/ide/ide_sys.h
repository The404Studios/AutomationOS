/*
 * ide_sys.h -- syscall + file/dir IO + tiny string helpers for the IDE.
 * One shared syscall ABI so no module re-rolls its own.
 */
#ifndef IDE_SYS_H
#define IDE_SYS_H

#include <stdint.h>

/* raw 6-arg syscall */
long ide_sc(long n, long a1, long a2, long a3, long a4, long a5, long a6);

/* directory entry (mirrors kernel struct dirent copied by SYS_READDIR) */
typedef struct {
    uint64_t ino;
    int64_t  off;
    uint16_t reclen;
    uint8_t  type;
    char     name[256];
} IdeDirent;
#define IDE_DT_DIR 4
#define IDE_DT_REG 8

/* Read whole file into buf (cap bytes). Returns byte count (>=0) or <0 on error. */
int  ide_read_file(const char* path, char* buf, int cap);
/* Truncate+write len bytes to path (creates if needed). Returns 0 or <0. */
int  ide_write_file(const char* path, const char* buf, int len);
/* List directory into out[] (up to max). Returns count or <0. */
int  ide_list_dir(const char* path, IdeDirent* out, int max);

long ide_ticks_ms(void);
void ide_exit(int code);

/* string helpers (shared so modules don't duplicate) */
int  ide_strlen(const char* s);
int  ide_streq(const char* a, const char* b);
int  ide_strneq(const char* a, const char* b, int n);
void ide_strlcpy(char* d, const char* s, int cap);
int  ide_itoa(int v, char* out);                 /* writes decimal, returns len */

#endif /* IDE_SYS_H */
