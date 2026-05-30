/*
 * userspace/lib/notify/notify.h — Desktop toast-notification client API
 *
 * Usage
 * -----
 *   #include "notify.h"
 *
 *   notify("Build finished", "kernel.elf compiled successfully");
 *
 * The function packs title and body as "title\0body\0" and invokes
 * SYS_NOTIFY (65).  If the syscall returns -ENOSYS the call is silently
 * ignored so the binary runs on kernels that have not yet wired the syscall.
 *
 * Wire format: title\0body\0   (both NUL-terminated, packed back-to-back)
 * Max total payload: 128 bytes (NOTIFY_PAYLOAD_MAX in kernel/include/notify.h)
 */

#ifndef USERSPACE_NOTIFY_H
#define USERSPACE_NOTIFY_H

/* Syscall numbers — must match kernel/include/notify.h + syscall.h */
#define SYS_NOTIFY      65
#define SYS_NOTIFY_POLL 66

/* Maximum total payload in bytes (title + NUL + body + NUL) */
#define NOTIFY_PAYLOAD_MAX 128

/*
 * notify() — post a desktop toast notification.
 *
 *   title : short notification title   (NULL treated as "")
 *   body  : longer description text    (NULL treated as "")
 *
 * Both strings are truncated to fit within NOTIFY_PAYLOAD_MAX bytes
 * (including the two NUL terminators).  The call is fire-and-forget;
 * it never blocks and returns void.
 *
 * Graceful degradation: if SYS_NOTIFY is unimplemented (-ENOSYS) or the
 * kernel queue is full the call silently does nothing.
 */
void notify(const char *title, const char *body);

/*
 * notify_poll() — dequeue one pending notification.
 *
 *   buf  : destination buffer
 *   max  : capacity of buf
 *
 * Returns the number of bytes written (>0) when a notification was
 * available, 0 if the queue is empty, or a negative value on error.
 * The format in buf is "title\0body\0".
 *
 * Intended for compositor / notification-daemon use.
 */
long notify_poll(void *buf, unsigned int max);

#endif /* USERSPACE_NOTIFY_H */