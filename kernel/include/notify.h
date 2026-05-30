/*
 * kernel/include/notify.h — Desktop notification ring queue
 *
 * Provides a small in-kernel queue of pending toast notifications.
 * Any process can post a notification via SYS_NOTIFY (65).
 * The compositor (or any privileged consumer) drains them via
 * SYS_NOTIFY_POLL (66) and renders toasts.
 *
 * Wire format: a single flat buffer "title\0body\0" where title and
 * body are NUL-terminated strings packed back-to-back.  Maximum total
 * payload per entry: NOTIFY_PAYLOAD_MAX bytes (including both NULs).
 *
 * Syscall wiring (for the integrator):
 *   SYS_NOTIFY      = 65   -> sys_notify_post(arg1=user_buf, arg2=len)
 *   SYS_NOTIFY_POLL = 66   -> sys_notify_poll(arg1=user_buf, arg2=max_len)
 */

#ifndef NOTIFY_H
#define NOTIFY_H

#include "types.h"

/* ---- tunables ---- */
#define NOTIFY_QUEUE_DEPTH   16    /* max pending notifications        */
#define NOTIFY_PAYLOAD_MAX  128    /* bytes per entry (title+NUL+body+NUL) */

/* ---- public kernel API ---- */

/*
 * notify_init() — initialise the ring queue.
 * Call once during kernel init (e.g. from kernel_main or ipc_init).
 */
void notify_init(void);

/*
 * notify_post() — copy a user-space notification payload into the queue.
 *
 *   user_text  : user-space pointer to "title\0body\0" buffer
 *   len        : total byte count (<= NOTIFY_PAYLOAD_MAX)
 *
 * Returns 0 on success, negative errno on error:
 *   -EINVAL  bad length (0 or > NOTIFY_PAYLOAD_MAX)
 *   -EFAULT  copy_from_user failed
 *   -ENOSPC  queue full (oldest entry silently dropped — ring policy)
 *
 * Backing syscall: SYS_NOTIFY = 65
 *   int64_t sys_notify_post(uint64_t user_text, uint64_t len, ...)
 */
int notify_post(const void *user_text, uint32_t len);

/*
 * notify_poll() — dequeue one notification into user space.
 *
 *   user_buf   : user-space destination buffer
 *   max        : capacity of user_buf
 *
 * Returns the byte length of the payload written (>0) if an entry was
 * available, 0 if the queue is empty, or negative errno on error:
 *   -EINVAL  max == 0
 *   -EFAULT  copy_to_user failed
 *
 * Backing syscall: SYS_NOTIFY_POLL = 66
 *   int64_t sys_notify_poll(uint64_t user_buf, uint64_t max, ...)
 */
int notify_poll(void *user_buf, uint32_t max);

/* ---- syscall handler prototypes (for handlers.c wiring) ---- */
int64_t sys_notify_post(uint64_t user_text, uint64_t len,
                        uint64_t arg3, uint64_t arg4,
                        uint64_t arg5, uint64_t arg6);

int64_t sys_notify_poll(uint64_t user_buf,  uint64_t max,
                        uint64_t arg3, uint64_t arg4,
                        uint64_t arg5, uint64_t arg6);

#endif /* NOTIFY_H */