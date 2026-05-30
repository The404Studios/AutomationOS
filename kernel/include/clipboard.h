#ifndef CLIPBOARD_H
#define CLIPBOARD_H

/*
 * Kernel Clipboard Service
 * ========================
 *
 * A single global text clipboard shared by all processes.  It holds an
 * arbitrary byte payload (not necessarily NUL-terminated) up to CLIP_MAX
 * bytes in a kmalloc'd kernel buffer.
 *
 * Syscall numbers (wired by the integrator):
 *   SYS_CLIP_SET = 63   -- write to clipboard
 *   SYS_CLIP_GET = 64   -- read from clipboard
 *
 * Buffer-size / return-value conventions
 * ----------------------------------------
 * clipboard_set(user_buf, len)
 *   - Copies min(len, CLIP_MAX) bytes from user space into the kernel buffer.
 *   - Returns the number of bytes stored (>= 0) on success.
 *   - Returns -EINVAL if len == 0 or user_buf is NULL.
 *   - Returns -EFAULT if copy_from_user fails (bad user pointer).
 *   - Returns -ENOMEM if the internal buffer has not been allocated.
 *
 * clipboard_get(user_buf, max)
 *   - Copies min(clip_len, max) bytes from the kernel buffer to user space.
 *   - Passing max == 0 with user_buf == NULL is a valid "size query": the
 *     function returns the current clipboard length without touching user
 *     memory.  Apps can use this to allocate a buffer of the right size
 *     before calling again with a real pointer and max == returned_size.
 *   - Returns the number of bytes copied (>= 0) on success.
 *   - Returns -EFAULT if copy_to_user fails (bad user pointer / max > 0).
 *   - Returns -ENOMEM if the internal buffer has not been allocated.
 */

#include "types.h"

/* Maximum clipboard payload in bytes (64 KiB) */
#define CLIP_MAX (64u * 1024u)

/* Initialise the clipboard subsystem.  Must be called once from kernel_main
 * after the heap is ready (i.e. after heap_init()).  Safe to call multiple
 * times – subsequent calls are no-ops. */
void clipboard_init(void);

/*
 * clipboard_set - replace clipboard contents from user space.
 *
 * Copies up to CLIP_MAX bytes from user_buf into the internal kernel buffer,
 * then updates clip_len.  Any previously stored content is overwritten.
 *
 * Returns: bytes stored (>= 0), or -EINVAL / -EFAULT / -ENOMEM on error.
 */
int clipboard_set(const void *user_buf, uint32_t len);

/*
 * clipboard_get - copy clipboard contents to user space.
 *
 * If user_buf == NULL and max == 0 this is a size-query: returns clip_len
 * without touching user memory.
 *
 * Otherwise copies min(clip_len, max) bytes into user_buf.
 *
 * Returns: bytes copied (>= 0), or -EFAULT / -ENOMEM on error.
 */
int clipboard_get(void *user_buf, uint32_t max);

#endif /* CLIPBOARD_H */
