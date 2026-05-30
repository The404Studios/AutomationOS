/*
 * kernel/ipc/clipboard.c
 * ======================
 *
 * Kernel-resident clipboard service.
 *
 * A single kmalloc'd buffer of CLIP_MAX bytes holds the most-recently-set
 * payload.  All access is serialised with a spinlock so concurrent
 * clipboard_set / clipboard_get from different CPUs are safe.
 *
 * Wiring required (performed by the integrator, NOT this file):
 *   syscall.h  -- add  #define SYS_CLIP_SET 63 / SYS_CLIP_GET 64
 *   handlers.c -- add  sys_clip_set / sys_clip_get wrapper bodies
 *   syscall.c  -- register the two handlers in the dispatch table
 *   kernel.c   -- call clipboard_init() after heap_init()
 */

#include "../include/clipboard.h"
#include "../include/kernel.h"
#include "../include/mem.h"
#include "../include/spinlock.h"
#include "../include/string.h"

/* ------------------------------------------------------------------ */
/* Internal state                                                       */
/* ------------------------------------------------------------------ */

static spinlock_t  clip_lock;          /* protects all fields below   */
static uint8_t    *clip_buf  = NULL;   /* kmalloc'd on clipboard_init */
static uint32_t    clip_len  = 0;      /* bytes currently stored      */
static bool        clip_ready = false; /* true once buf is allocated  */

/* ------------------------------------------------------------------ */
/* clipboard_init                                                       */
/* ------------------------------------------------------------------ */

void clipboard_init(void) {
    /* Idempotent: if already initialised, do nothing. */
    if (clip_ready) {
        return;
    }

    spin_lock_init_named(&clip_lock, "clipboard");

    clip_buf = (uint8_t *)kmalloc(CLIP_MAX);
    if (!clip_buf) {
        kprintf("[CLIPBOARD] clipboard_init: kmalloc(%u) failed\n", CLIP_MAX);
        /* Clipboard will remain disabled; clipboard_set/get return -ENOMEM. */
        return;
    }

    memset(clip_buf, 0, CLIP_MAX);
    clip_len   = 0;
    clip_ready = true;

    kprintf("[CLIPBOARD] Clipboard service ready (%u bytes)\n", CLIP_MAX);
}

/* ------------------------------------------------------------------ */
/* clipboard_set                                                        */
/* ------------------------------------------------------------------ */

/*
 * Copy up to CLIP_MAX bytes from user_buf into the kernel clipboard buffer.
 *
 * Returns: number of bytes stored (>= 0), or negative errno:
 *   -ENOMEM (-12) : clipboard not initialised (kmalloc failed at boot)
 *   -EINVAL (-22) : len == 0 or user_buf == NULL
 *   -EFAULT (-14) : copy_from_user reported a bad user pointer
 */
int clipboard_set(const void *user_buf, uint32_t len) {
    if (!clip_ready) {
        return -12; /* -ENOMEM */
    }

    if (!user_buf || len == 0) {
        return -22; /* -EINVAL */
    }

    /* Clamp to maximum capacity. */
    uint32_t to_copy = (len > CLIP_MAX) ? CLIP_MAX : len;

    /*
     * Copy from user space into a temporary kernel buffer OUTSIDE the lock.
     * copy_from_user can fault (no #PF->EFAULT fixup yet); faulting while
     * holding an IRQ-off spinlock would deadlock once a real fault handler or
     * SMP exists. kmalloc here is also outside clip_lock, avoiding a
     * clip_lock -> heap_lock nesting.
     */
    uint8_t *tmp = (uint8_t *)kmalloc(to_copy);
    if (!tmp) {
        return -12; /* -ENOMEM */
    }
    int rc = copy_from_user(tmp, user_buf, (size_t)to_copy);
    if (rc != COPY_SUCCESS) {
        kfree(tmp);
        return -14; /* -EFAULT */
    }

    uint64_t flags;
    spin_lock_irqsave(&clip_lock, &flags);
    memcpy(clip_buf, tmp, (size_t)to_copy);
    clip_len = to_copy;
    spin_unlock_irqrestore(&clip_lock, flags);

    kfree(tmp);
    return (int)to_copy;
}

/* ------------------------------------------------------------------ */
/* clipboard_get                                                        */
/* ------------------------------------------------------------------ */

/*
 * Copy min(clip_len, max) bytes from the kernel clipboard buffer to user_buf.
 *
 * Size-query shortcut: if user_buf == NULL and max == 0, returns the current
 * clipboard length without touching user memory.  Use this to learn the right
 * buffer size before allocating and calling again.
 *
 * Returns: bytes copied (>= 0), or negative errno:
 *   -ENOMEM (-12) : clipboard not initialised
 *   -EFAULT (-14) : copy_to_user reported a bad user pointer
 */
int clipboard_get(void *user_buf, uint32_t max) {
    if (!clip_ready) {
        return -12; /* -ENOMEM */
    }

    /* Size-query shortcut needs no user copy. */
    if (user_buf == NULL && max == 0) {
        uint64_t flags;
        spin_lock_irqsave(&clip_lock, &flags);
        uint32_t current_len = clip_len;
        spin_unlock_irqrestore(&clip_lock, flags);
        return (int)current_len;
    }

    /*
     * Pre-allocate a temp buffer (worst case = clamp of max) BEFORE taking the
     * lock, snapshot under the lock, then copy_to_user OUTSIDE the lock. This
     * keeps the potentially-faulting user copy off the IRQ-off critical section
     * and avoids a clip_lock -> heap_lock nesting.
     */
    uint32_t cap = (max > CLIP_MAX) ? CLIP_MAX : max;
    if (cap == 0) {
        return 0;
    }
    uint8_t *tmp = (uint8_t *)kmalloc(cap);
    if (!tmp) {
        return -12; /* -ENOMEM */
    }

    uint64_t flags;
    spin_lock_irqsave(&clip_lock, &flags);
    uint32_t to_copy = (clip_len < cap) ? clip_len : cap;
    if (to_copy) {
        memcpy(tmp, clip_buf, (size_t)to_copy);
    }
    spin_unlock_irqrestore(&clip_lock, flags);

    int rc = COPY_SUCCESS;
    if (to_copy) {
        rc = copy_to_user(user_buf, tmp, (size_t)to_copy);
    }
    kfree(tmp);

    if (rc != COPY_SUCCESS) {
        return -14; /* -EFAULT */
    }
    return (int)to_copy;
}
