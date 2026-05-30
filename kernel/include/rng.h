/*
 * rng.h -- Kernel Random Number Generator API
 * ============================================
 *
 * Provides hardware-backed (RDRAND) and software-fallback (xorshift128+)
 * random number generation for the kernel.
 *
 * Usage
 * -----
 *   rng_init();               // call once during kernel init
 *   uint64_t v = rng_u64();   // get 64 random bits
 *   rng_bytes(buf, n);        // fill buffer
 *   rng_has_hardware();       // 1 if RDRAND available, 0 if PRNG fallback
 */

#ifndef RNG_H
#define RNG_H

#include "types.h"

/*
 * Initialize the RNG subsystem.
 *
 * Probes CPUID leaf 1 ECX[30] for RDRAND support.  If absent, seeds
 * xorshift128+ from rdtsc mixed with RTC time registers.
 * Must be called before any rng_u64() / rng_bytes() call.
 */
void rng_init(void);

/*
 * Return 64 random bits.
 *
 * Uses RDRAND (with carry-flag retry loop, up to RNG_RDRAND_RETRIES
 * attempts) when available; falls back to xorshift128+ otherwise.
 */
uint64_t rng_u64(void);

/*
 * Fill buf[0..n-1] with random bytes.
 */
void rng_bytes(void *buf, size_t n);

/*
 * Returns 1 if RDRAND hardware is available and in use, 0 for PRNG fallback.
 */
int rng_has_hardware(void);

/* Maximum retry count for a single RDRAND attempt before giving up and
 * mixing in the PRNG fallback for that word. */
#define RNG_RDRAND_RETRIES  10

/*
 * Proposed kernel syscall number for userspace random bytes.
 *
 * SYS_RANDOM(buf, len) -> ssize_t
 *   Fills user buffer <buf> with <len> random bytes.
 *   Returns number of bytes written on success, negative error on failure.
 *   Suggested number: 41  (next free slot after SYS_GET_TICKS_MS = 40).
 *
 * Handler prototype (for kernel/core/syscall/handlers.c):
 *
 *   int64_t sys_random(uint64_t buf, uint64_t len, uint64_t arg3,
 *                      uint64_t arg4, uint64_t arg5, uint64_t arg6);
 *
 * Wiring (kernel/core/syscall/syscall.c):
 *   syscall_table[SYS_RANDOM] = sys_random;
 *
 * /dev/random VFS node wiring (report only; do not edit vfs.c):
 *   - After vfs_mkdir("/dev", ...) succeeds, call rng_register_devnode().
 *   - rng_register_devnode() creates an inode with rng_fops (read returns
 *     random bytes, write is ignored), then calls vfs_link_node("/dev/random").
 *   - Kernel open("/dev/random") goes through vfs_open -> rng_fops.read.
 */
#define SYS_RANDOM  41

/* Optional: register /dev/random VFS device node.
 * Reports the intent; actual wiring requires caller to use vfs_* APIs.
 * Returns 0 on success, negative on failure. */
int rng_register_devnode(void);

#endif /* RNG_H */
