/*
 * kernel/lib/strerror.c - Human-readable error messages
 *
 * Converts kernel error codes to readable strings for better UX.
 */

#include "../include/errno.h"

/**
 * Convert error code to human-readable string
 *
 * @param err Error code (negative kernel errno)
 * @return Descriptive error message
 */
const char* strerror(int err) {
    /* Handle positive errors by negating (for flexibility) */
    if (err > 0) {
        err = -err;
    }

    switch (err) {
        case ESUCCESS:
            return "Success";
        case EPERM:
            return "Operation not permitted";
        case ENOENT:
            return "No such file or directory";
        case ESRCH:
            return "No such process";
        case EINTR:
            return "Interrupted system call";
        case EIO:
            return "Input/output error";
        case ENXIO:
            return "No such device or address";
        case E2BIG:
            return "Argument list too long";
        case ENOEXEC:
            return "Exec format error";
        case EBADF:
            return "Bad file descriptor";
        case ECHILD:
            return "No child processes";
        case EAGAIN:
            return "Resource temporarily unavailable";
        case ENOMEM:
            return "Out of memory";
        case EACCES:
            return "Permission denied";
        case EFAULT:
            return "Bad address";
        case EBUSY:
            return "Device or resource busy";
        case EEXIST:
            return "File exists";
        case EXDEV:
            return "Cross-device link";
        case ENODEV:
            return "No such device";
        case ENOTDIR:
            return "Not a directory";
        case EISDIR:
            return "Is a directory";
        case EINVAL:
            return "Invalid argument";
        case ENFILE:
            return "File table overflow";
        case EMFILE:
            return "Too many open files";
        case ENOTTY:
            return "Not a typewriter (inappropriate ioctl)";
        case EFBIG:
            return "File too large";
        case ENOSPC:
            return "No space left on device";
        case ESPIPE:
            return "Illegal seek";
        case EROFS:
            return "Read-only file system";
        case EMLINK:
            return "Too many links";
        case ERANGE:
            return "Math result not representable";
        case ENOSYS:
            return "Function not implemented";
        case ENOTEMPTY:
            return "Directory not empty";
        case ENODATA:
            return "No data available";
        case EOVERFLOW:
            return "Value too large for data type";
        case EOPNOTSUPP:
            return "Operation not supported";
        default:
            return "Unknown error";
    }
}

/**
 * Get short error name (e.g., "ENOENT")
 */
const char* strerrorname(int err) {
    if (err > 0) {
        err = -err;
    }

    switch (err) {
        case ESUCCESS:    return "SUCCESS";
        case EPERM:       return "EPERM";
        case ENOENT:      return "ENOENT";
        case ESRCH:       return "ESRCH";
        case EINTR:       return "EINTR";
        case EIO:         return "EIO";
        case ENXIO:       return "ENXIO";
        case E2BIG:       return "E2BIG";
        case ENOEXEC:     return "ENOEXEC";
        case EBADF:       return "EBADF";
        case ECHILD:      return "ECHILD";
        case EAGAIN:      return "EAGAIN";
        case ENOMEM:      return "ENOMEM";
        case EACCES:      return "EACCES";
        case EFAULT:      return "EFAULT";
        case EBUSY:       return "EBUSY";
        case EEXIST:      return "EEXIST";
        case EXDEV:       return "EXDEV";
        case ENODEV:      return "ENODEV";
        case ENOTDIR:     return "ENOTDIR";
        case EISDIR:      return "EISDIR";
        case EINVAL:      return "EINVAL";
        case ENFILE:      return "ENFILE";
        case EMFILE:      return "EMFILE";
        case ENOTTY:      return "ENOTTY";
        case EFBIG:       return "EFBIG";
        case ENOSPC:      return "ENOSPC";
        case ESPIPE:      return "ESPIPE";
        case EROFS:       return "EROFS";
        case EMLINK:      return "EMLINK";
        case ERANGE:      return "ERANGE";
        case ENOSYS:      return "ENOSYS";
        case ENOTEMPTY:   return "ENOTEMPTY";
        case ENODATA:     return "ENODATA";
        case EOVERFLOW:   return "EOVERFLOW";
        case EOPNOTSUPP:  return "EOPNOTSUPP";
        default:          return "UNKNOWN";
    }
}
