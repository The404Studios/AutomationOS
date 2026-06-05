/* selfheal.h — SELFHEAL desktop self-heal contract (heartbeat shared page).
 *
 * One 4 KiB SysV SHM page the compositor stamps once per frame-loop iteration so
 * a tiny userspace supervisor (sbin/cwatchdog) can distinguish a LIVE desktop
 * from a FROZEN one with zero kernel cooperation.  Shared, verbatim, by:
 *   - init        (userspace/init/main.c)            — CREATES + OWNS the segment
 *   - compositor  (userspace/compositor/compositor_m8.c) — WRITER (per frame)
 *   - cwatchdog   (userspace/apps/cwatchdog/cwatchdog.c)  — READER (poller)
 *
 * ============================ OWNERSHIP (load-bearing) =======================
 * Verified against kernel/ipc/shm.c:shm_cleanup_process (lines 955-988): when a
 * process dies, the kernel treats EVERY segment it *owns* (owner_pid == pid) as
 * an implicit IPC_RMID — it calls key_table_remove(seg), pulling the key from the
 * lookup table.  So if the COMPOSITOR owned this page, killing it for recovery
 * would tombstone the key; the respawned compositor's shmget(KEY) would then miss
 * it and create a brand-new segment, leaving the watchdog attached to the dead
 * one → it never sees the resumed heartbeat → endless kill/respawn (a storm).
 *
 *   >>> Therefore INIT (PID 1, immortal) creates+owns this segment <<<
 *
 * init also ZEROES the page once (sys_shmget does NOT zero page contents — only
 * the segment struct), so `magic` starts 0 on a fresh boot.  The compositor and
 * watchdog attach LOOKUP-ONLY — shmget(KEY, SELFHEAL_SHM_SIZE, 0), i.e. NO
 * IPC_CREAT (a real size is required because this kernel rejects size==0; with
 * the key already present it returns the existing segment without creating) — so
 * neither can ever accidentally become the owner.  A compositor death is then a
 * NON-owner detach: attach_count is decremented, the segment is NOT tombstoned
 * (owner init is alive), and the respawned compositor re-attaches the SAME page —
 * exactly one recovery, no segment churn, no storm.
 *
 * =============================== ONE-SHOT LATCH ==============================
 * For the forced-freeze PROOF (-DFREEZE_TEST), `magic` doubles as a "this segment
 * was already initialised by a prior instance" latch: the FIRST compositor sees
 * magic != SELFHEAL_MAGIC (init zeroed it) → it arms the one-shot freeze, then
 * writes magic.  A RESPAWNED compositor sees magic == SELFHEAL_MAGIC (persisted
 * across the kill because init owns the page) → it does NOT re-freeze.  So the
 * recovery proof is exactly ONE freeze → ONE recovery → a clean PASS, never a
 * self-inflicted circuit-breaker storm.
 *
 * DISCIPLINE: the per-frame write does NO allocation, NO logging, NO IPC — just a
 * couple of plain volatile stores — so it can never itself block or stall a frame.
 *
 * SELF-CONTAINED: uses ONLY primitive C types (no stdint, no typedefs of its own
 * besides the struct tag), so each includer — even the -ffreestanding -nostdlib
 * compositor with its own uint32_t/uint64_t typedefs — includes it without any
 * collision, and the struct layout is identical everywhere (no field drift).
 *
 * GATING: every includer guards its use behind -DSELFHEAL, so the DEFAULT build
 * compiles none of this and is byte-for-byte unchanged.
 */
#ifndef SELFHEAL_H
#define SELFHEAL_H

/* Well-known SysV key (clear of window-surface 0x100000+, font 0x200000, cursor
 * 0x300000, clipboard 0x400000, and the inbox MSG key 0x434F4D50). */
#define SELFHEAL_SHM_KEY   0x53480001   /* 'SH' + 0001                            */
#define SELFHEAL_SHM_SIZE  4096u        /* exactly one page                       */
#define SELFHEAL_MAGIC     0x53484254u  /* 'SHBT' — set once the page is live     */
#define SELFHEAL_VERSION   1u

/* Informational compositor state (the liveness decision is purely "did
 * frame_counter advance", not this field). */
#define SH_STATE_INIT      0u
#define SH_STATE_RUNNING   1u
#define SH_STATE_STOPPING  2u

/* The shared page.  All fields volatile: single-writer (compositor) /
 * single-reader (watchdog), no lock — a torn read just delays detection by one
 * poll, which is benign.  Primitive types keep the layout identical across all
 * three includers regardless of their local typedefs. */
typedef struct sh_heartbeat {
    volatile unsigned int       magic;           /* SELFHEAL_MAGIC once init'd     */
    volatile unsigned int       version;         /* SELFHEAL_VERSION               */
    volatile unsigned int       state;           /* SH_STATE_*                     */
    volatile unsigned int       compositor_pid;  /* live compositor PID (to kill)  */
    volatile unsigned long long frame_counter;   /* ++ once per compositor loop    */
    volatile unsigned long long last_frame_ms;   /* SYS_GET_TICKS_MS at last beat  */
    volatile unsigned long long reserved[4];     /* future use; keeps struct 64 B  */
} sh_heartbeat_t;

#endif /* SELFHEAL_H */
