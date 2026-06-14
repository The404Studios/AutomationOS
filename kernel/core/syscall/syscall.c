#include "../../include/syscall.h"
#include "../../include/kernel.h"
#include "../../include/perf.h"
#include "../../include/seccomp.h"
#include "../../include/sched.h"
#include "../../include/net.h"   // sys_net_info / sys_net_send / sys_net_recv prototypes
#include "../../include/netif.h" // sys_net_config prototype
#include "../../include/socket.h" // sys_sock_* (BSD socket) prototypes
#include "../../include/audio.h" // audio_beep (SYS_BEEP target)
#include "../../include/acpi.h"  // power_off / power_reboot (SYS_POWEROFF/REBOOT)
#include "../../include/pci.h"   // sys_pci_list (SYS_PCI_LIST=92)
// NOTE: do NOT include compat/errno.h here. It defines EINVAL/ENOTSUP as
// POSITIVE values, which (being included last) would override syscall.h's
// canonical NEGATIVE errno — making syscall_dispatch return +22 / +95 to
// userspace for bad/unimplemented syscalls, indistinguishable from a 22- or
// 95-byte success. syscall.h already provides the negative EINVAL/ENOTSUP used
// below.

// Syscall handler table
static syscall_handler_t syscall_table[MAX_SYSCALLS];

/**
 * SYS_BEEP handler: play a tone via the HDA audio driver.
 * arg1 = frequency in Hz (0 defaults to 440 Hz)
 * arg2 = duration in milliseconds (0 defaults to 200 ms)
 * Returns 0 on success, negative on error (no HDA hardware).
 */
static int64_t sys_beep(uint64_t freq_hz, uint64_t duration_ms,
                        uint64_t arg3, uint64_t arg4,
                        uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    return (int64_t)audio_beep((uint32_t)freq_hz, (uint32_t)duration_ms);
}

/* sys_poweroff / sys_reboot live in handlers.c (canonical definitions).
 * Removed duplicates that were here -- the syscall_table entries below
 * reference the handlers.c versions via the header declarations. */

void syscall_init(void) {
    kprintf("[SYSCALL] Initializing system call interface...\n");

    // Clear syscall table
    for (int i = 0; i < MAX_SYSCALLS; i++) {
        syscall_table[i] = NULL;
    }

    // Register syscall handlers
    syscall_table[SYS_EXIT] = sys_exit;
    syscall_table[SYS_FORK] = sys_fork;
    syscall_table[SYS_READ] = sys_read;
    syscall_table[SYS_WRITE] = sys_write;
    syscall_table[SYS_OPEN] = sys_open;
    syscall_table[SYS_CLOSE] = sys_close;
    syscall_table[SYS_WAITPID] = sys_waitpid;
    syscall_table[SYS_EXECVE] = sys_execve;
    syscall_table[SYS_GETPID] = sys_getpid;
    syscall_table[SYS_SLEEP] = sys_sleep;
    syscall_table[SYS_READ_EVENT] = sys_read_event;
    syscall_table[SYS_YIELD] = sys_yield;
    syscall_table[SYS_SPAWN] = sys_spawn;
    syscall_table[SYS_MAP_FILE] = sys_map_file;

    // IPC syscalls
    syscall_table[SYS_SHMGET] = sys_shmget;
    syscall_table[SYS_SHMAT] = sys_shmat;
    syscall_table[SYS_SHMDT] = sys_shmdt;
    syscall_table[SYS_SHMCTL] = sys_shmctl;
    syscall_table[SYS_MSGGET] = sys_msgget;
    syscall_table[SYS_MSGSND] = sys_msgsnd;
    syscall_table[SYS_MSGRCV] = sys_msgrcv;
    syscall_table[SYS_MSGCTL] = sys_msgctl;

    // Signal syscalls
    syscall_table[SYS_KILL] = sys_kill;
    // SIG-FULL-0 (B8): handler delivery / masks / sigreturn
    syscall_table[SYS_RT_SIGACTION]   = sys_rt_sigaction;
    syscall_table[SYS_RT_SIGPROCMASK] = sys_rt_sigprocmask;
    syscall_table[SYS_RT_SIGRETURN]   = sys_rt_sigreturn;
    syscall_table[SYS_SIGPENDING]     = sys_sigpending;

    // Priority syscalls
    syscall_table[SYS_NICE] = sys_nice;
    syscall_table[SYS_GETPRIORITY] = sys_getpriority;
    syscall_table[SYS_SETPRIORITY] = sys_setpriority;

    // Resource limit syscalls -- handlers exist in kernel/core/rlimit/syscall.c
    // but the rlimit subsystem has unresolved deps and is not yet compiled.
    // TODO: integrate rlimit subsystem into the build.
    // syscall_table[SYS_SETRLIMIT] = sys_setrlimit;
    // syscall_table[SYS_GETRLIMIT] = sys_getrlimit;
    // syscall_table[SYS_GETRUSAGE] = sys_getrusage;
    // syscall_table[SYS_PRLIMIT]   = sys_prlimit;

    // I/O control syscalls
    syscall_table[SYS_IOCTL] = sys_ioctl;

    // Memory / framebuffer / time syscalls (M1 graphics platform)
    syscall_table[SYS_MMAP] = sys_mmap;
    syscall_table[SYS_MUNMAP] = sys_munmap;
    syscall_table[SYS_FB_ACQUIRE] = sys_fb_acquire;
    syscall_table[SYS_GET_TICKS_MS] = sys_get_ticks_ms;

    // RTC / wall-clock syscalls
    syscall_table[SYS_TIME] = sys_time;
    syscall_table[SYS_GETTIME] = sys_gettime;

    // Entropy syscall
    syscall_table[SYS_RANDOM] = sys_random;

    // Audio beep (HDA tone playback)
    syscall_table[SYS_BEEP] = sys_beep;

    // Power management: ACPI poweroff (S5) and reboot
    syscall_table[SYS_POWEROFF] = sys_poweroff;
    syscall_table[SYS_REBOOT]   = sys_reboot;

    // Security: seccomp -- handler in kernel/security/seccomp/seccomp_syscall.c
    // but seccomp subsystem not yet compiled. TODO: integrate.
    // syscall_table[SYS_SECCOMP] = sys_seccomp;

    // Process enumeration
    syscall_table[SYS_PROCLIST] = sys_proclist;

    // Process API (AI introspect + control), clipboard, notifications
    syscall_table[SYS_PROC_QUERY]  = sys_proc_query;
    syscall_table[SYS_PROC_CTL]    = sys_proc_ctl;
    syscall_table[SYS_SYSINFO]     = sys_sysinfo;
    syscall_table[SYS_CLIP_SET]    = sys_clip_set;
    syscall_table[SYS_CLIP_GET]    = sys_clip_get;
    syscall_table[SYS_NOTIFY]      = sys_notify_post;
    syscall_table[SYS_NOTIFY_POLL] = sys_notify_poll;

    // Directory syscalls
    syscall_table[SYS_OPENDIR] = sys_opendir;
    syscall_table[SYS_READDIR] = sys_readdir;
    syscall_table[SYS_CLOSEDIR] = sys_closedir;
    syscall_table[SYS_STAT] = sys_stat;
    syscall_table[SYS_UNLINK] = sys_unlink;
    syscall_table[SYS_RENAME] = sys_rename;
    syscall_table[SYS_MKDIR] = sys_mkdir;
    syscall_table[SYS_TRUNCATE] = sys_truncate;
    syscall_table[SYS_FTRUNCATE] = sys_ftruncate;
    syscall_table[SYS_FSYNC] = sys_fsync;
    syscall_table[SYS_SYNC] = sys_sync;
    syscall_table[SYS_RECOVERY_OVERLAY] = sys_recovery_overlay;

    // Block device syscalls (AHCI/SATA persistence)
    syscall_table[SYS_BLK_READ] = sys_blk_read;
    syscall_table[SYS_BLK_WRITE] = sys_blk_write;

    // Persistent named-file syscalls (diskfs flat files; gate-safe, no-op w/o disk)
    syscall_table[SYS_PERSIST_READ]  = sys_persist_read;
    syscall_table[SYS_PERSIST_WRITE] = sys_persist_write;

    // Networking syscalls (e1000 NIC: info/MAC + raw frame TX/RX)
    syscall_table[SYS_NET_INFO] = (syscall_handler_t)sys_net_info;
    syscall_table[SYS_NET_SEND] = (syscall_handler_t)sys_net_send;
    syscall_table[SYS_NET_RECV] = (syscall_handler_t)sys_net_recv;

    // BSD-ish socket syscalls (UDP + TCP over the e1000 stack)
    syscall_table[SYS_SOCKET]    = (syscall_handler_t)sys_sock_socket;
    syscall_table[SYS_CONNECT]   = (syscall_handler_t)sys_sock_connect;
    syscall_table[SYS_SEND]      = (syscall_handler_t)sys_sock_send;
    syscall_table[SYS_RECV]      = (syscall_handler_t)sys_sock_recv;
    syscall_table[SYS_CLOSE_SK]  = (syscall_handler_t)sys_sock_close;
    syscall_table[SYS_SENDTO]    = (syscall_handler_t)sys_sock_sendto;
    syscall_table[SYS_RECVFROM]  = (syscall_handler_t)sys_sock_recvfrom;
    syscall_table[SYS_SOCK_POLL] = (syscall_handler_t)sys_sock_poll;
    syscall_table[SYS_BIND]      = (syscall_handler_t)sys_sock_bind;
    syscall_table[SYS_LISTEN]    = (syscall_handler_t)sys_sock_listen;
    syscall_table[SYS_ACCEPT]    = (syscall_handler_t)sys_sock_accept;

    // Network configuration (DHCP lease apply, routing, ARP table query)
    syscall_table[SYS_NET_CONFIG]  = (syscall_handler_t)sys_net_config;
    syscall_table[SYS_ROUTE_TABLE] = (syscall_handler_t)sys_route_table;
    syscall_table[SYS_ARP_TABLE]   = (syscall_handler_t)sys_arp_table;

    // PCI device list (lspci userspace tool)
    syscall_table[SYS_PCI_LIST]    = (syscall_handler_t)sys_pci_list;

    // Battery status (EC embedded controller -- T410 / laptops)
    syscall_table[SYS_BATTERY]     = sys_battery;

    // Futex (fast userspace mutex)
    syscall_table[SYS_FUTEX] = sys_futex;

    // Threads (real threads sharing the caller's address space)
    syscall_table[SYS_THREAD_CREATE] = sys_thread_create;
    syscall_table[SYS_THREAD_EXIT]   = sys_thread_exit;
    syscall_table[SYS_THREAD_JOIN]   = sys_thread_join;

    // Zero-copy file transfer
    syscall_table[SYS_SENDFILE] = sys_sendfile;

    // Performance monitoring
    syscall_table[SYS_PERF_REPORT] = sys_perf_report;

    // Batched syscall interface (io_uring-style)
    syscall_table[SYS_BATCH_SUBMIT] = sys_batch_submit;

    // VMA testing syscall (benchmarking red-black tree)
    syscall_table[SYS_VMA_TEST] = sys_vma_test;

    // Epoll syscalls (event-driven I/O multiplexing)
    syscall_table[SYS_EPOLL_CREATE] = sys_epoll_create;
    syscall_table[SYS_EPOLL_CTL]    = sys_epoll_ctl;
    syscall_table[SYS_EPOLL_WAIT]   = sys_epoll_wait;

    // CHANNEL-0: capability-backed shared-ring channels (kernel/ipc/channel.c).
    // Additive -- every existing program is byte-for-byte unchanged (unregistered
    // numbers return ENOTSUP; nothing binds a channel until a process opts in).
    {
        extern int64_t sys_ch_create(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
        extern int64_t sys_ch_write (uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
        extern int64_t sys_ch_read  (uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
        extern int64_t sys_ch_wait  (uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
        extern int64_t sys_ch_close (uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
        syscall_table[SYS_CH_CREATE] = sys_ch_create;
        syscall_table[SYS_CH_WRITE]  = sys_ch_write;
        syscall_table[SYS_CH_READ]   = sys_ch_read;
        syscall_table[SYS_CH_WAIT]   = sys_ch_wait;
        syscall_table[SYS_CH_CLOSE]  = sys_ch_close;
        extern void channel_selftest(void);
        channel_selftest();
        // P2: additive spawn that binds a child's stdio to channel handles.
        extern int64_t sys_spawn_ex(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
        syscall_table[SYS_SPAWN_EX] = sys_spawn_ex;
        extern void channel_selftest_p2(void);
        channel_selftest_p2();
        // P5: typed CH_MSG message framing (substrate only; no syscall/agent yet).
        extern void channel_selftest_p5(void);
        channel_selftest_p5();
        // P5b: explicit CH_MSG packet syscalls (byte channels keep stream semantics).
        extern int64_t sys_ch_sendmsg(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
        extern int64_t sys_ch_recvmsg(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
        syscall_table[SYS_CH_SENDMSG] = sys_ch_sendmsg;
        syscall_table[SYS_CH_RECVMSG] = sys_ch_recvmsg;
        // P6c: one-shot read-only CH_BYTE capability grant/accept (AGENT-RPC-0 stdout transfer).
        extern int64_t sys_ch_grant (uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
        extern int64_t sys_ch_accept(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
        syscall_table[SYS_CH_GRANT]  = sys_ch_grant;
        syscall_table[SYS_CH_ACCEPT] = sys_ch_accept;
        // P6d: explicit argv-vector spawn (separate from the command-line SYS_SPAWN_EX).
        extern int64_t sys_spawn_ex_argv(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
        syscall_table[SYS_SPAWN_EX_ARGV] = sys_spawn_ex_argv;
    }

#if defined(SMP_FOUNDATION) && !defined(SMP_SCHED_DISPATCH)
    // SMP coprocessor offload (the userspace -> CPU1 bridge). Registered ONLY for
    // the coprocessor SMP build; the DEFAULT kernel leaves this slot NULL, so the
    // syscall returns ENOTSUP and the binary is byte-for-byte unchanged. The handler
    // does all user-pointer validation + copy-in/out and dispatches a trusted kernel
    // matmul to CPU1 (handlers.c, also SMP_FOUNDATION-gated).
    //
    // NOT registered under SMP_SCHED_DISPATCH: in SCHEDULER mode CPU1 runs
    // ap_scheduler_loop, NOT the cpu1_job worker loop, so an offload would publish a
    // job nobody services and time out silently. Leaving the slot NULL makes the
    // syscall return ENOTSUP immediately (the cpu1offload userspace test then SKIPs
    // cleanly instead of timing out / FAILing). Mode separation, per the SMP audit.
    syscall_table[SYS_CPU1_OFFLOAD] = sys_cpu1_offload;
#endif

    // Initialize epoll subsystem
    epoll_init();

    kprintf("[SYSCALL] Registered %d syscalls\n", 50);
    kprintf("[SYSCALL] System call interface initialized\n");
}

int64_t syscall_dispatch(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6) {
#ifdef SCHED_DEBUG
    // DIAGNOSTIC: paint a GREEN marker the first time ANY syscall arrives. If this
    // appears on the T410, ring-3 was reached and /sbin/init executed at least up
    // to its first syscall — i.e. the freeze (if any) is AFTER ring-3 entry, not in
    // the IRETQ. If it never appears but the yellow "enter_usermode" marker does,
    // the fault is in the IRETQ itself or init's first (pre-syscall) instructions.
    {
        extern void framebuffer_puts_scaled(const char*, uint32_t, uint32_t,
                                            uint32_t, uint32_t);
        static volatile int _first_syscall_seen = 0;
        static volatile int _spawn_seen = 0;
        static volatile int _fb_seen = 0;
        if (!_first_syscall_seen) {
            _first_syscall_seen = 1;
            framebuffer_puts_scaled("ring3 OK: first syscall", 40, 150, 0x0000FF00u, 2);
        }
        // SYS_SPAWN (16): init launching a child (the compositor / services).
        if (syscall_num == 16 && !_spawn_seen) {
            _spawn_seen = 1;
            framebuffer_puts_scaled("init: SYS_SPAWN ok", 40, 172, 0x0000FF00u, 2);
        }
        // First syscall from a process that is NOT init (pid 1) — proves the
        // spawned compositor actually got SCHEDULED and executed ring-3 code.
        // If this NEVER appears, the compositor was spawned but never ran (a
        // scheduler/runqueue or yield bug). If it DOES appear but FB_ACQUIRE
        // does not, the compositor runs but hangs early in its own startup.
        {
            static volatile int _noninit_seen = 0;
            process_t* cur = process_get_current();
            if (!_noninit_seen && cur && cur->pid != 1) {
                _noninit_seen = 1;
                framebuffer_puts_scaled("non-init proc RAN (compositor)", 40, 326, 0x0000FF00u, 2);
            }
        }
        // SYS_WAITPID(6)/SYS_YIELD(15) from init(pid1): proves init FINISHED its
        // spawn sequence and reached its blocking point — so it WILL invoke the
        // scheduler. If this never appears, init is stuck in ring-3 before it ever
        // blocks (so the scheduler is never asked to run a child).
        {
            static volatile int _initblock_seen = 0;
            process_t* c2 = process_get_current();
            if (!_initblock_seen && c2 && c2->pid == 1 &&
                (syscall_num == 6 || syscall_num == 15)) {
                _initblock_seen = 1;
                framebuffer_puts_scaled("init reached WAITPID/YIELD", 40, 238, 0x00FFFF00u, 2);
            }
        }
        // SYS_FB_ACQUIRE (39): the compositor mapping the framebuffer = it started.
        if (syscall_num == 39 && !_fb_seen) {
            _fb_seen = 1;
            framebuffer_puts_scaled("compositor: FB_ACQUIRE ok", 40, 348, 0x0000FF00u, 2);
        }
    }
#endif
    // Wave-7 perf: PERF_START/END removed from the hot path. The RDTSC pair
    // cost ~40 cycles per syscall (~20% of a fast getpid). Individual handlers
    // that need profiling can instrument themselves; the dispatch wrapper is too
    // hot. SYS_PERF_REPORT still works (it measures handler-level, not wrapper).

    // ═══════════════════════════════════════════════════════════════════════════
    // FAST PATH: Inline common read-only syscalls (40-56% latency reduction)
    // ═══════════════════════════════════════════════════════════════════════════
    // Branch prediction: getpid is ~10-15% of all syscalls (benchmarked)
    // Inlining saves handler dispatch overhead (~20-30 cycles)
    if (__builtin_expect(syscall_num == SYS_GETPID, 0)) {
        process_t* current = process_get_current();
        if (__builtin_expect(current != NULL, 1)) {
            return (int64_t)current->pid;
        }
        return ESRCH;
    }

    // Fast path: gettimeofday (read-only, high-frequency)
    if (__builtin_expect(syscall_num == SYS_GET_TICKS_MS, 0)) {
        extern uint64_t timer_get_ticks_ms(void);
        return (int64_t)timer_get_ticks_ms();
    }

    // Wave-7 perf: sys_yield fast path — avoid handler table lookup for the
    // most latency-sensitive scheduling syscall (~5-8% of all syscalls).
    if (__builtin_expect(syscall_num == SYS_YIELD, 0)) {
        return sys_yield(arg1, arg2, arg3, arg4, arg5, arg6);
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // VALIDATION: Bounds check and handler lookup
    // ═══════════════════════════════════════════════════════════════════════════
    // Branch hint: syscall_num is valid (reduces misprediction in hot path)
    if (__builtin_expect(syscall_num >= MAX_SYSCALLS, 0)) {
        // Cold path: invalid syscall (rare, ~0.1% of calls)
#ifndef SYSCALL_QUIET
        kprintf("[SYSCALL] Invalid syscall number: %u\n", (uint32_t)syscall_num);
#endif
        return EINVAL;
    }

    syscall_handler_t handler = syscall_table[syscall_num];
    if (__builtin_expect(handler == NULL, 0)) {
        // Cold path: unimplemented syscall (rare, ~0.5% of calls)
#ifndef SYSCALL_QUIET
        kprintf("[SYSCALL] Unimplemented syscall: %u\n", (uint32_t)syscall_num);
#endif
        return ENOTSUP;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // SECCOMP: Security filter enforcement
    // ═══════════════════════════════════════════════════════════════════════════
    // SECCOMP disabled until seccomp.c is compiled
    // TODO: Re-enable when seccomp infrastructure is linked
    // Branch hint: most processes have no seccomp filter (fast path)
    // if (__builtin_expect(current->seccomp_filter != NULL, 0)) {
    //     if (seccomp_enforce(current, syscall_num) != 0) {
    //         return EPERM;
    //     }
    // }

    // ═══════════════════════════════════════════════════════════════════════════
    // DISPATCH: Call handler (optimized: no debug logging in hot path)
    // ═══════════════════════════════════════════════════════════════════════════
#ifndef SYSCALL_QUIET
    kprintf("[SYSCALL] Dispatching syscall %u\n", (uint32_t)syscall_num);
#endif

#ifdef SMP_BKL
    // SMP-H1 BKL-LITE: the marked-unsafe groups (FS/net/IPC/proc -- see the
    // table + the loud blocking-exclusion doc in bkl.c) take the one outer
    // kernel lock around the WHOLE handler body. Unmarked syscalls (and the
    // getpid/ticks/yield fast paths above, which never reach this point) run
    // exactly as before. Owner-recursive, so a marked handler internally
    // re-entering a marked path cannot self-deadlock. Falls through (no early
    // return) so the post-handler diagnostics below stay live for marked
    // syscalls too.
    extern int  bkl_is_marked(uint64_t n);
    extern void bkl_acquire(void);
    extern void bkl_release(void);
    int bkl_held = 0;
    if (bkl_is_marked(syscall_num)) {
        bkl_acquire();
        bkl_held = 1;
    }
#endif

    int64_t result = handler(arg1, arg2, arg3, arg4, arg5, arg6);

#ifdef SMP_BKL
    if (bkl_held) {
        bkl_release();
    }
#endif

#ifdef SCHED_DEBUG
    // DIAGNOSTIC: report the RESULT of init's first SYS_SPAWN. The entry marker
    // above fires before the handler (so it only proves init CALLED spawn); this
    // fires after, proving whether the child actually LOADED. result>0 = pid
    // (success); result<0 = load/exec failure (the spawned binary was rejected).
    if (syscall_num == 16) {
        static volatile int _spawn_ret_seen = 0;
        if (!_spawn_ret_seen) {
            _spawn_ret_seen = 1;
            extern void framebuffer_puts_scaled(const char*, uint32_t, uint32_t,
                                                uint32_t, uint32_t);
            if (result > 0)
                framebuffer_puts_scaled("spawn RESULT: OK (loaded)", 40, 194, 0x0000FF00u, 2);
            else
                framebuffer_puts_scaled("spawn RESULT: FAIL (load rejected)", 40, 194, 0x00FF0000u, 2);
        }
    }
#endif

#ifndef SYSCALL_QUIET
    kprintf("[SYSCALL] Syscall %u returned %d\n", (uint32_t)syscall_num, (int)result);
#endif

    return result;
}
