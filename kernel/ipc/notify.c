/*
 * kernel/ipc/notify.c — Desktop toast-notification ring queue
 *
 * Design
 * ------
 * A statically-allocated ring buffer of NOTIFY_QUEUE_DEPTH slots, each
 * holding up to NOTIFY_PAYLOAD_MAX bytes of packed "title\0body\0" data.
 * The ring uses a simple head/tail/count scheme protected by a spinlock so
 * it is safe to call from any context (including interrupt handlers, though
 * that would be unusual).
 *
 * Overflow policy: when the queue is full and a new notification arrives,
 * the oldest entry is silently dropped (head advances) so the producer
 * never blocks.  The compositor is expected to drain the queue each frame.
 *
 * Wire format (per entry)
 * -----------------------
 *   [ title bytes ] 0x00 [ body bytes ] 0x00   (total <= NOTIFY_PAYLOAD_MAX)
 *
 * Both title and body may be empty strings (single NUL each), giving a
 * minimum payload of 2 bytes.
 *
 * Syscall wiring (integrator adds to handlers.c dispatch table)
 * -------------------------------------------------------------
 *   case SYS_NOTIFY      (65): return sys_notify_post(arg1, arg2, ...);
 *   case SYS_NOTIFY_POLL (66): return sys_notify_poll(arg1, arg2, ...);
 *
 * notify_init() should be called from kernel_main() or ipc_init(), after
 * the memory subsystem is ready.
 */

#include "../include/notify.h"
#include "../include/types.h"
#include "../include/mem.h"
#include "../include/kernel.h"
#include "../include/spinlock.h"
#include "../include/string.h"
#include "../include/errno.h"   /* canonical negative errno (EINVAL/EFAULT/ENOSPC) */

/* ---- ring queue storage ---- */

typedef struct {
    uint8_t  data[NOTIFY_PAYLOAD_MAX]; /* packed "title\0body\0"            */
    uint32_t len;                       /* valid bytes in data[]             */
} notify_entry_t;

static notify_entry_t  g_queue[NOTIFY_QUEUE_DEPTH];
static uint32_t        g_head;          /* index of oldest entry             */
static uint32_t        g_tail;          /* index of next free slot           */
static uint32_t        g_count;         /* current occupancy                 */
static spinlock_t      g_lock;

/* ---- internal helpers ---- */

static inline uint32_t ring_next(uint32_t idx)
{
    return (idx + 1) % NOTIFY_QUEUE_DEPTH;
}

/* ---- public kernel API ---- */

void notify_init(void)
{
    g_head  = 0;
    g_tail  = 0;
    g_count = 0;
    spin_lock_init(&g_lock);
    kprintf("[notify] notification queue initialised (%d slots x %d bytes)\n",
            NOTIFY_QUEUE_DEPTH, NOTIFY_PAYLOAD_MAX);
}

int notify_post(const void *user_text, uint32_t len)
{
    if (len == 0 || len > NOTIFY_PAYLOAD_MAX)
        return EINVAL;

    /* Copy payload from user space before taking the lock. */
    uint8_t tmp[NOTIFY_PAYLOAD_MAX];
    if (copy_from_user(tmp, user_text, len) != 0)
        return EFAULT;

    spin_lock(&g_lock);

    if (g_count == NOTIFY_QUEUE_DEPTH) {
        /*
         * Queue full — drop oldest entry (ring / fire-and-forget policy).
         * The compositor is responsible for draining fast enough; we never
         * block a posting process.
         */
        g_head = ring_next(g_head);
        g_count--;
    }

    notify_entry_t *slot = &g_queue[g_tail];
    memcpy(slot->data, tmp, len);
    slot->len = len;

    g_tail = ring_next(g_tail);
    g_count++;

    spin_unlock(&g_lock);
    return 0;
}

int notify_poll(void *user_buf, uint32_t max)
{
    if (max == 0)
        return EINVAL;
    if (!user_buf)
        return EFAULT;  /* reject before dequeue so a NULL buf can't drain the queue */

    spin_lock(&g_lock);

    if (g_count == 0) {
        spin_unlock(&g_lock);
        return 0;          /* empty — compositor should just skip this frame */
    }

    notify_entry_t *slot = &g_queue[g_head];
    uint32_t copy_len = (slot->len < max) ? slot->len : max;

    /* Snapshot into a local buffer so we can release the lock before the
     * potentially-faulting copy_to_user call. */
    uint8_t tmp[NOTIFY_PAYLOAD_MAX];
    memcpy(tmp, slot->data, copy_len);

    g_head  = ring_next(g_head);
    g_count--;

    spin_unlock(&g_lock);

    if (copy_to_user(user_buf, tmp, copy_len) != 0)
        return EFAULT;

    return (int)copy_len;
}

/* ---- syscall handler shims ---- */

/*
 * sys_notify_post — backing handler for SYS_NOTIFY (65)
 *
 *   arg1 = (uint64_t) user pointer to "title\0body\0" buffer
 *   arg2 = (uint64_t) byte length of buffer (<= NOTIFY_PAYLOAD_MAX)
 */
int64_t sys_notify_post(uint64_t user_text, uint64_t len,
                        uint64_t arg3, uint64_t arg4,
                        uint64_t arg5, uint64_t arg6)
{
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    return (int64_t)notify_post((const void *)user_text, (uint32_t)len);
}

/*
 * sys_notify_poll — backing handler for SYS_NOTIFY_POLL (66)
 *
 *   arg1 = (uint64_t) user pointer to destination buffer
 *   arg2 = (uint64_t) capacity of destination buffer
 *
 * Returns byte count written (>0), 0 if queue empty, negative on error.
 */
int64_t sys_notify_poll(uint64_t user_buf, uint64_t max,
                        uint64_t arg3, uint64_t arg4,
                        uint64_t arg5, uint64_t arg6)
{
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    return (int64_t)notify_poll((void *)user_buf, (uint32_t)max);
}