/*
 * tar.h -- POSIX ustar header layout + public tar API for the freestanding
 *          userspace `tar` tool.
 *
 * The tar_header_t below is byte-for-byte identical to the kernel's own
 * initrd parser header (kernel/init/initrd.c). It is a 512-byte packed
 * POSIX ustar header:
 *
 *   offset  size  field
 *   ------  ----  -----------------------------------------------
 *      0    100   name      (NUL-terminated path, no prefix split)
 *    100      8   mode      (octal, NUL-terminated)
 *    108      8   uid       (octal)
 *    116      8   gid       (octal)
 *    124     12   size      (octal byte count of file data)
 *    136     12   mtime     (octal mod time; 0 here -- no RTC dependency)
 *    148      8   checksum  ("NNNNNN\0 " 6-octal-digit + NUL + space)
 *    156      1   typeflag  ('0' regular file, '5' directory)
 *    157    100   linkname
 *    257      6   magic     ("ustar\0")
 *    263      2   version   ("00")
 *    265     32   uname
 *    297     32   gname
 *    329      8   devmajor
 *    337      8   devminor
 *    345    155   prefix
 *    500     12   padding
 *   ------  ----
 *    512   total
 *
 * Archives written with this layout mount directly as a kernel initrd.
 */

#ifndef TAR_H
#define TAR_H

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];      /* "ustar" */
    char version[2];    /* "00"    */
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} __attribute__((packed)) tar_header_t;

/* Compile-time guarantee the header is exactly one tar block. */
_Static_assert(sizeof(tar_header_t) == 512, "ustar header must be 512 bytes");

/* ----------------------------------------------------------------------
 * Public tar operations. All paths are kernel VFS paths. Each returns 0 on
 * success, -1 on error, and writes progress/diagnostics to fd 1.
 * -------------------------------------------------------------------- */

/* Create `archive` containing `count` paths (files shallow, dirs recursive). */
int tar_create(const char *archive, const char *const *paths, int count);

/* Extract `archive` into `destdir` (NULL or "." => current behavior). */
int tar_extract(const char *archive, const char *destdir);

/* List the entries of `archive` to fd 1. */
int tar_list(const char *archive);

#endif /* TAR_H */
