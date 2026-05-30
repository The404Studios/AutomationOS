// userspace/libc/errno.c - errno storage and strerror()

#include "errno.h"

// Single global errno slot. Not thread-safe (no threads yet); the accessor
// is provided so a future per-thread implementation only changes this file.
static int __errno_value = 0;

int* __errno_location(void) {
    return &__errno_value;
}

// Human-readable error strings indexed by errno value. Kept compact; unknown
// codes fall through to a generic message.
static const char* const error_strings[] = {
    [0]            = "Success",
    [EPERM]        = "Operation not permitted",
    [ENOENT]       = "No such file or directory",
    [ESRCH]        = "No such process",
    [EINTR]        = "Interrupted system call",
    [EIO]          = "Input/output error",
    [ENXIO]        = "No such device or address",
    [E2BIG]        = "Argument list too long",
    [ENOEXEC]      = "Exec format error",
    [EBADF]        = "Bad file descriptor",
    [ECHILD]       = "No child processes",
    [EAGAIN]       = "Resource temporarily unavailable",
    [ENOMEM]       = "Cannot allocate memory",
    [EACCES]       = "Permission denied",
    [EFAULT]       = "Bad address",
    [ENOTBLK]      = "Block device required",
    [EBUSY]        = "Device or resource busy",
    [EEXIST]       = "File exists",
    [EXDEV]        = "Invalid cross-device link",
    [ENODEV]       = "No such device",
    [ENOTDIR]      = "Not a directory",
    [EISDIR]       = "Is a directory",
    [EINVAL]       = "Invalid argument",
    [ENFILE]       = "Too many open files in system",
    [EMFILE]       = "Too many open files",
    [ENOTTY]       = "Inappropriate ioctl for device",
    [ETXTBSY]      = "Text file busy",
    [EFBIG]        = "File too large",
    [ENOSPC]       = "No space left on device",
    [ESPIPE]       = "Illegal seek",
    [EROFS]        = "Read-only file system",
    [EMLINK]       = "Too many links",
    [EPIPE]        = "Broken pipe",
    [EDOM]         = "Numerical argument out of domain",
    [ERANGE]       = "Numerical result out of range",
    [ENAMETOOLONG] = "File name too long",
    [ENOSYS]       = "Function not implemented",
    [ENOTEMPTY]    = "Directory not empty",
    [ELOOP]        = "Too many levels of symbolic links",
    [EOVERFLOW]    = "Value too large for defined data type",
    [EILSEQ]       = "Invalid or incomplete multibyte or wide character",
};

#define NUM_ERROR_STRINGS (int)(sizeof(error_strings) / sizeof(error_strings[0]))

char* strerror(int errnum) {
    if (errnum >= 0 && errnum < NUM_ERROR_STRINGS && error_strings[errnum]) {
        return (char*)error_strings[errnum];
    }
    return (char*)"Unknown error";
}

// XSI-style strerror_r: copies the message into buf, returns 0 on success.
int strerror_r(int errnum, char* buf, unsigned long buflen) {
    if (!buf || buflen == 0) {
        return EINVAL;
    }
    const char* msg = strerror(errnum);
    unsigned long i = 0;
    while (msg[i] && i < buflen - 1) {
        buf[i] = msg[i];
        i++;
    }
    buf[i] = '\0';
    return 0;
}
