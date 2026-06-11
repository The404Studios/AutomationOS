/*
 * bkl.c -- SMP-H1 BKL-LITE: the one owner-recursive outer kernel lock.
 * =====================================================================
 *
 * WHY (the user's framing): once BATCH-class work starts running on CPU1,
 * syscalls become the danger surface -- two CPUs executing kernel subsystem
 * code that was written single-core. The honest Linux-2.0 answer is ONE big
 * outer lock around the marked-unsafe syscall groups: correctness first,
 * granularity later (the "no fine-grained VFS rewrite" hard no).
 *
 * WHAT IS MARKED (the groups, chosen conservatively):
 *   FS    : open/read/write/close, opendir/readdir/closedir, stat, unlink,
 *           rename, mkdir, map_file
 *   IPC   : shm get/at/dt/ctl, msgget/msgsnd/msgctl, clipboard set/get,
 *           notify post/poll, channel create/write/read/close/sendmsg/
 *           recvmsg/grant/accept + spawn_ex
 *   NET   : socket, send, sendto, close_sk, bind, listen, net_info,
 *           net_send, net_recv (poll-style), sock_poll
 *   PROC  : fork, execve, spawn, kill, nice, get/setpriority, proclist,
 *           proc_query, proc_ctl, blk_read/blk_write
 *
 * WHAT IS DELIBERATELY NOT MARKED (the load-bearing exclusion -- READ THIS):
 *   This kernel is COOPERATIVE and syscalls run IF=0. A marked syscall that
 *   BLOCKS (context-switches away) while holding the BKL leaves the lock
 *   owned by a parked task; the other CPU then spins IF=0 forever = a
 *   cross-CPU wedge. Linux 2.0 solved this with release-on-block in
 *   schedule(); BKL-LITE v1 instead REFUSES to mark anything that can
 *   schedule away mid-syscall:
 *     exit(0), waitpid(6), sleep(9), yield(15), read_event(14), msgrcv(24),
 *     ioctl(36, PTY paths), recv/recvfrom(54/57), accept(78), connect(52,
 *     handshake wait), futex(70), thread_exit/join(80/81), ch_wait(99).
 *   ALSO unmarked: getpid/get_ticks/yield (dispatcher fast paths bypass the
 *   table entirely), mmap/munmap + rlimit/rusage (self-contained, per-task),
 *   fb_acquire, time/gettime/random, beep, perf/epoll/batch/sendfile (not in
 *   the storm's blast radius; marked-set growth is a follow-up checkpoint,
 *   not a default).
 *   If a future brick makes a marked path blocking, the WATCHDOG below is
 *   the tripwire: a >2s spin prints a loud [BKL] diagnostic (never silent).
 *
 * RECURSION: owner-recursive by CPU -- if the owning CPU re-enters (a marked
 * handler internally invoking another marked path), depth++ instead of
 * self-deadlock. Owner identity is the CPU, not the task: on this cooperative
 * kernel a task cannot lose its CPU mid-syscall without blocking, and
 * blocking paths are unmarked by construction.
 *
 * LAW 16 interaction: the BKL is an OUTER lock around whole syscall bodies;
 * it must never be held across a TLB-shootdown ack wait. Today no marked
 * handler sends shootdowns (kernel-mapping mutation is not a syscall path);
 * the ipi_tlb_flush_kernel_range IF==1 entry check is the backstop if that
 * ever changes (syscalls run IF=0, so a marked handler calling it gets the
 * loud law-16 refusal, not a deadlock).
 *
 * State lives in this file's .bss -- linked in the SMP block, low addresses,
 * below the 0x200000 strict gate (law 15: touched from both CPUs' syscall
 * contexts under arbitrary CR3).
 */

#include "../../include/types.h"
#include "../../include/kernel.h"
#include "../../include/syscall.h"
#include "../../include/perf.h"          /* rdtsc -- the watchdog bound */

extern uint32_t cpu_id(void);

#define BKL_UNOWNED      0xFFFFFFFFu
#define BKL_WATCHDOG_TSC (2000000ULL * 3000ULL)   /* ~2 s at the 3 GHz estimate */

static volatile uint32_t bkl_word      = 0;            /* the lock bit          */
static volatile uint32_t bkl_owner     = BKL_UNOWNED;  /* owning CPU            */
static volatile uint32_t bkl_depth     = 0;            /* owner recursion depth */

/* stats: per-CPU marked-syscall executions + contended acquisitions. The
 * one-shot "engaged" print is the acceptance's cpu0=1 cpu1=1 evidence: BOTH
 * cpus demonstrably executed marked kernel paths under the wall. */
static volatile uint64_t bkl_acq[2];
static volatile uint64_t bkl_contended[2];
static volatile uint32_t bkl_engaged_logged = 0;
static volatile uint32_t bkl_watchdog_fired = 0;

/* The marked table. Designated initializers; everything not listed is 0 =
 * UNMARKED (runs lock-free, exactly as before this brick). */
static const uint8_t bkl_marked[MAX_SYSCALLS] = {
    /* ---- FS ---- */
    [SYS_READ] = 1, [SYS_WRITE] = 1, [SYS_OPEN] = 1, [SYS_CLOSE] = 1,
    [SYS_OPENDIR] = 1, [SYS_READDIR] = 1, [SYS_CLOSEDIR] = 1,
    [SYS_STAT] = 1, [SYS_UNLINK] = 1, [SYS_RENAME] = 1, [SYS_MKDIR] = 1,
    [SYS_MAP_FILE] = 1,
    /* ---- IPC ---- */
    [SYS_SHMGET] = 1, [SYS_SHMAT] = 1, [SYS_SHMDT] = 1, [SYS_SHMCTL] = 1,
    [SYS_MSGGET] = 1, [SYS_MSGSND] = 1, [SYS_MSGCTL] = 1,
    [SYS_CLIP_SET] = 1, [SYS_CLIP_GET] = 1,
    [SYS_NOTIFY] = 1, [SYS_NOTIFY_POLL] = 1,
    [SYS_CH_CREATE] = 1, [SYS_CH_WRITE] = 1, [SYS_CH_READ] = 1,
    [SYS_CH_CLOSE] = 1, [SYS_SPAWN_EX] = 1,
    [SYS_CH_SENDMSG] = 1, [SYS_CH_RECVMSG] = 1,
    [SYS_CH_GRANT] = 1, [SYS_CH_ACCEPT] = 1,
    /* ---- NET (non-blocking subset) ---- */
    [SYS_SOCKET] = 1, [SYS_SEND] = 1, [SYS_SENDTO] = 1, [SYS_CLOSE_SK] = 1,
    [SYS_BIND] = 1, [SYS_LISTEN] = 1, [SYS_NET_INFO] = 1,
    [SYS_NET_SEND] = 1, [SYS_NET_RECV] = 1, [SYS_SOCK_POLL] = 1,
    /* ---- PROC management (non-blocking subset) ---- */
    [SYS_FORK] = 1, [SYS_EXECVE] = 1, [SYS_SPAWN] = 1, [SYS_KILL] = 1,
    [SYS_NICE] = 1, [SYS_GETPRIORITY] = 1, [SYS_SETPRIORITY] = 1,
    [SYS_PROCLIST] = 1, [SYS_PROC_QUERY] = 1, [SYS_PROC_CTL] = 1,
    [SYS_BLK_READ] = 1, [SYS_BLK_WRITE] = 1,
};

int bkl_is_marked(uint64_t syscall_num) {
    if (syscall_num >= MAX_SYSCALLS) return 0;
    return bkl_marked[syscall_num];
}

void bkl_acquire(void) {
    uint32_t me = cpu_id();

    /* owner-recursive fast path: WE already hold it. Safe to read owner
     * without the lock: only the owning CPU stores `me` there, and we ARE
     * that CPU if it matches. */
    if (bkl_word && __atomic_load_n(&bkl_owner, __ATOMIC_ACQUIRE) == me) {
        bkl_depth++;
        return;
    }

    int contended = 0;
    uint64_t s = rdtsc();
    while (__atomic_test_and_set(&bkl_word, __ATOMIC_ACQUIRE)) {
        contended = 1;
        if (!bkl_watchdog_fired && (rdtsc() - s) >= BKL_WATCHDOG_TSC) {
            /* The tripwire for the "marked path blocked while holding the
             * BKL" design violation. LOUD diagnostic, then keep waiting --
             * proceeding unlocked would trade a visible wedge for silent
             * corruption (never), and panicking on a possible false positive
             * violates the F3-0 validator discipline. One-shot. */
            bkl_watchdog_fired = 1;
            kprintf("[BKL] possible deadlock: cpu%u spinning >2s (owner=cpu%u "
                    "depth=%u) -- a marked syscall blocked while holding the "
                    "BKL?\n", me,
                    __atomic_load_n(&bkl_owner, __ATOMIC_ACQUIRE),
                    bkl_depth);
        }
        __asm__ volatile("pause" ::: "memory");
    }
    __atomic_store_n(&bkl_owner, me, __ATOMIC_RELEASE);
    bkl_depth = 1;

    if (me < 2) {
        bkl_acq[me]++;
        if (contended) bkl_contended[me]++;
        /* acceptance evidence: the first moment BOTH cpus have executed
         * marked syscalls under the wall. One-shot; serial-safe because the
         * printer HOLDS the BKL (the other CPU's marked path is excluded; an
         * unmarked CPU1 print racing this is the pre-existing kprintf-race
         * exposure, unchanged by this brick). */
        if (!bkl_engaged_logged && bkl_acq[0] > 0 && bkl_acq[1] > 0) {
            bkl_engaged_logged = 1;
            kprintf("[BKL] engaged: cpu0_acq=%lu cpu1_acq=%lu contended=%lu+%lu "
                    "(cross-cpu syscall serialization live)\n",
                    (unsigned long)bkl_acq[0], (unsigned long)bkl_acq[1],
                    (unsigned long)bkl_contended[0], (unsigned long)bkl_contended[1]);
        }
    }
}

void bkl_release(void) {
    uint32_t me = cpu_id();
    if (__atomic_load_n(&bkl_owner, __ATOMIC_ACQUIRE) != me || bkl_depth == 0) {
        /* Releasing a lock we don't hold = a dispatcher wrap bug. Loud, no
         * state change (corrupting owner/depth would be worse). */
        kprintf("[BKL] BUG: cpu%u released a BKL it does not own (owner=%u)\n",
                me, __atomic_load_n(&bkl_owner, __ATOMIC_ACQUIRE));
        return;
    }
    if (--bkl_depth == 0) {
        __atomic_store_n(&bkl_owner, BKL_UNOWNED, __ATOMIC_RELEASE);
        __atomic_clear(&bkl_word, __ATOMIC_RELEASE);
    }
}
