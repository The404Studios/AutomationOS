/*
 * kernel/include/errno.h — Canonical kernel errno set (negative, Linux ABI)
 *
 * All kernel-internal error codes are NEGATIVE integers. Syscall handlers
 * return these directly to userspace; userspace sees the same values Linux uses
 * (a return < 0 is an error, its negation the errno).
 *
 * DO NOT include this header together with kernel/include/compat/errno.h in the
 * same translation unit. compat/errno.h defines the same names POSITIVE (for
 * legacy libc-style code); the guard below fires at compile time if both are
 * pulled into one TU, so the divergence can never silently flip a sign again.
 */
#ifndef KERNEL_ERRNO_H
#define KERNEL_ERRNO_H

#ifdef _KERNEL_COMPAT_ERRNO_H
#error "Cannot include both kernel/include/errno.h (negative) and kernel/include/compat/errno.h (positive) in the same translation unit."
#endif

#define ESUCCESS     0

#define EPERM       (-1)    /* Operation not permitted */
#define ENOENT      (-2)    /* No such file or directory */
#define ESRCH       (-3)    /* No such process */
#define EINTR       (-4)    /* Interrupted system call */
#define EIO         (-5)    /* I/O error */
#define ENXIO       (-6)    /* No such device or address */
#define E2BIG       (-7)    /* Argument list too long */
#define ENOEXEC     (-8)    /* Exec format error */
#define EBADF       (-9)    /* Bad file descriptor */
#define ECHILD      (-10)   /* No child processes */
#define EAGAIN      (-11)   /* Try again */
#define ENOMEM      (-12)   /* Out of memory */
#define EACCES      (-13)   /* Permission denied */
#define EFAULT      (-14)   /* Bad address */
#define EBUSY       (-16)   /* Device or resource busy */
#define EEXIST      (-17)   /* File exists */
#define EXDEV       (-18)   /* Cross-device link */
#define ENODEV      (-19)   /* No such device */
#define ENOTDIR     (-20)   /* Not a directory */
#define EISDIR      (-21)   /* Is a directory */
#define EINVAL      (-22)   /* Invalid argument */
#define ENFILE      (-23)   /* File table overflow */
#define EMFILE      (-24)   /* Too many open files */
#define ENOTTY      (-25)   /* Not a typewriter */
#define EFBIG       (-27)   /* File too large */
#define ENOSPC      (-28)   /* No space left on device */
#define ESPIPE      (-29)   /* Illegal seek */
#define EROFS       (-30)   /* Read-only file system */
#define EMLINK      (-31)   /* Too many links */
#define ERANGE      (-34)   /* Math result not representable */
#define ENOSYS      (-38)   /* Function not implemented */
#define ENOTEMPTY   (-39)   /* Directory not empty */
#define ENODATA     (-61)   /* No data available */
#define EOVERFLOW   (-75)   /* Value too large for data type */
#define EOPNOTSUPP  (-95)   /* Operation not supported */
#define ENOTSUP     EOPNOTSUPP  /* alias */
#define EAPFAULT    (-200)  /* Application processor fault */

#endif /* KERNEL_ERRNO_H */
