// Init Process (PID 1) for AutomationOS
// Spawns and manages system services using SYS_SPAWN (no fork required)

typedef unsigned long size_t;

// Syscall numbers matching kernel/include/syscall.h
#define SYS_EXIT    0
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_GETPID  8
#define SYS_SLEEP   9
#define SYS_SPAWN   16
#define SYS_SPAWN_EX_ARGV 106
#define SYS_WAITPID 6
#define SYS_YIELD   15
#define SYS_SHMGET  18
#define SYS_SHMAT   19
#define SYS_KILL    26
#define SYS_GET_TICKS_MS 40
#define SYS_TIME    41
#define SYS_RECOVERY_OVERLAY 88

#define WNOHANG 1
#define SIGKILL 9

#define COMP_POLL_MS             50
#define COMP_START_TIMEOUT_MS 10000
#define COMP_STALL_MS          2500
#define COMP_STABLE_RESET_MS  30000
#define COMP_CIRCUIT_FAILS         5
#define COMP_CIRCUIT_COOLDOWN_MS 30000

#ifdef SELFHEAL
/* SELFHEAL: init creates+owns the compositor heartbeat SHM page. selfheal.h is
 * self-contained (primitive types only), so including it here is collision-free. */
#include "../include/selfheal.h"
#endif

static inline long syscall(long n, long a1, long a2, long a3) {
    long ret;
    asm volatile (
        "syscall"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* 6-arg syscall (r10/r8/r9 = a4/a5/a6) -- for SYS_SPAWN_EX_ARGV's stdio handles. */
static inline long sc6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5, r9 asm("r9") = a6;
    asm volatile("syscall" : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

static size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

/* Line-buffered console output. Init builds its log lines from several fragments ("[INIT] Process " + pid + " exited"),
 * and on a multi-core kernel another CPU's print can land BETWEEN two fragments and tear the line (found by the multi-core
 * proof: "[INIT] Process 5[SYSCALL] sys_stat..." -- init had reaped the process, the log said otherwise). Collect a whole
 * line and emit it in ONE write; the serial driver already makes a single write atomic. */
static char g_lb[256];
static int  g_ln;
static void print_flush(void) {
    if (g_ln > 0) { syscall(SYS_WRITE, 1, (long)g_lb, g_ln); g_ln = 0; }
}

static void print(const char* msg) {
    for (; *msg; msg++) {
        if (g_ln >= (int)sizeof(g_lb)) print_flush();
        g_lb[g_ln++] = *msg;
        if (*msg == '\n') print_flush();
    }
}

static void print_num(long n) {
    char buf[24];
    int i = 0;
    if (n < 0) { print("-"); n = -n; }
    do { buf[i++] = '0' + (n % 10); n /= 10; } while (n > 0);
    while (i > 0) { char c[2]; c[0] = buf[--i]; c[1] = 0; print(c); }
}

static int spawn(const char* path) {
    int pid = (int)syscall(SYS_SPAWN, (long)path, 0, 0);
    if (pid < 0) {
        print("[INIT] Spawn failed for ");
        print(path);
        print("\n");
    }
    return pid;
}

// Spawn with a space-separated argument string (kernel builds argv = [path,...]).
static int spawn_args(const char* path, const char* args) {
    int pid = (int)syscall(SYS_SPAWN, (long)path, (long)args, 0);
    if (pid < 0) { print("[INIT] Spawn failed for "); print(path); print("\n"); }
    return pid;
}

/* Spawn with a SINGLE argv[1] entry via the explicit-vector ABI (NUL-separated
 * byte buffer + byte length + explicit stdio=0) -- the convention the compositor's
 * project-icon double-click uses, so a path containing a space survives intact. */
static int spawn_ex_argv1(const char* path, const char* arg) {
    char buf[256]; int n = 0;
    for (const char* p = arg; *p && n < (int)sizeof(buf) - 1; ) buf[n++] = *p++;
    buf[n++] = '\0';
    int pid = (int)sc6(SYS_SPAWN_EX_ARGV, (long)path, (long)buf, (long)n, 0, 0, 0);
    if (pid < 0) { print("[INIT] spawn_ex_argv1 failed for "); print(path); print("\n"); }
    return pid;
}

static void yield(void) {
    print_flush();
    syscall(SYS_YIELD, 0, 0, 0);
}

static void sleep(int ms) {
    print_flush();
    syscall(SYS_SLEEP, ms, 0, 0);
}

static long now_ms(void) {
    return syscall(SYS_GET_TICKS_MS, 0, 0, 0);
}

static long compositor_backoff_ms(int failures) {
    long delay = 250;
    int shifts = failures > 1 ? failures - 1 : 0;
    if (shifts > 5) shifts = 5;
    while (shifts-- > 0) delay <<= 1;
    return delay > 8000 ? 8000 : delay;
}

static void print_supervisor(const char* event, long pid, long value) {
    print("[SUP] t="); print_num(now_ms());
    print(" service=compositor event="); print(event);
    if (pid >= 0) { print(" pid="); print_num(pid); }
    if (value >= 0) { print(" value_ms="); print_num(value); }
    print("\n");
}

void _start(void) {
    print("[INIT] ==========================================\n");
    print("[INIT]   AutomationOS Init (PID 1)\n");
    print("[INIT]   Starting userspace services...\n");
    print("[INIT] ==========================================\n");

    int my_pid = (int)syscall(SYS_GETPID, 0, 0, 0);
    if (my_pid != 1) {
        print("[INIT] ERROR: Not PID 1!\n");
        syscall(SYS_EXIT, 1, 0, 0);
    }

#ifdef E2E_TEST
    /* FS-E2E (E2E build only): run the end-to-end integration proof BEFORE the
     * desktop, so it is a clean headless proof. init (itself loaded from the
     * initrd by the ELF loader) spawns e2etest, which does VFS ls /bin, loads +
     * runs /bin/hello.elf, and drives tool_shell to run a command. We waitpid it
     * so build_test/e2e_boot.sh sees its markers deterministically. Boot then
     * continues normally into the desktop. */
    print("[INIT] E2E_TEST: running end-to-end integration proof...\n");
    {
        int e2e_pid = spawn("sbin/e2etest");
        if (e2e_pid > 0) {
            int e2e_st = 0;
            syscall(SYS_WAITPID, e2e_pid, (long)&e2e_st, 0);
        } else {
            print("[INIT] E2E_TEST: FAILED to spawn sbin/e2etest\n");
        }
        print("[INIT] E2E_TEST: integration proof complete\n");
    }
#endif

#ifdef SELFHEAL
    volatile sh_heartbeat_t* heal = (volatile sh_heartbeat_t*)0;
    /* SELFHEAL: init (PID 1, immortal) CREATES + OWNS the compositor heartbeat
     * segment, then zeroes the page (sys_shmget does NOT zero page contents), so
     * the first compositor instance reads magic==0 and detects a fresh segment.
     * init owning it is load-bearing: an owner's death implicitly IPC_RMIDs the
     * segment (kernel/ipc/shm.c:955-988), tombstoning the key and breaking
     * respawn-resume — see userspace/include/selfheal.h. init keeps it attached
     * forever so the page can never be freed while the desktop is up. */
    print("[INIT] SELFHEAL: creating compositor heartbeat segment...\n");
    {
        long hb_id = syscall(SYS_SHMGET, (long)SELFHEAL_SHM_KEY,
                             (long)SELFHEAL_SHM_SIZE, 0x200 /*IPC_CREAT*/ | 0666);
        if (hb_id < 0) {
            print("[INIT] SELFHEAL: heartbeat shmget FAILED\n");
        } else {
            long hb_addr = syscall(SYS_SHMAT, hb_id, 0, 0);
            if (hb_addr > 0) {
                volatile unsigned char* p = (volatile unsigned char*)hb_addr;
                for (unsigned i = 0; i < SELFHEAL_SHM_SIZE; i++) p[i] = 0;
                heal = (volatile sh_heartbeat_t*)hb_addr;
                print("[INIT] SELFHEAL: heartbeat segment ready\n");
            } else {
                print("[INIT] SELFHEAL: heartbeat shmat FAILED\n");
            }
        }
    }
#endif

    print("[INIT] Spawning compositor...\n");
    int compositor_pid = spawn("sbin/compositor");
    if (compositor_pid > 0) {
        print("[INIT] Compositor started (PID ");
        print_num(compositor_pid);
        print(")\n");
    } else {
        print("[INIT] ERROR: Failed to spawn compositor!\n");
    }

#ifdef DZ_MPLIVE
    // DEADZONE-MP-LIVE-0: prove LIVE networked co-op over the kernel's loopback
    // datapath. Spawn the authoritative server, give it a moment to bind/listen
    // and start ticking, then spawn the headless client -- it connects to
    // 127.0.0.1:27015, plays a scripted session, and prints
    //   "DEADZONE: mp LIVE PASS ..."   (verified server applied our inputs).
    // Loopback needs no NIC; this is the first userspace two-process TCP exchange
    // in the OS. Gated -DDZ_MPLIVE => the default boot never spawns either, so it
    // is byte-behaviorally invisible (the smoke gate stays unchanged).
    print("[INIT] DZ_MPLIVE: spawning deadzoned (authoritative server)...\n");
    spawn("sbin/deadzoned");
    sleep(800);
    print("[INIT] DZ_MPLIVE: spawning dzclient (live loopback co-op client)...\n");
    int dz_client_pid = spawn("sbin/dzclient");
    if (dz_client_pid > 0) {
        // Block init on the client's scripted session so it gets CPU promptly --
        // otherwise the ~70-process self-test storm spawned below would starve the
        // cooperative client and it would never finish before the smoke timeout.
        // The server keeps running (init waits ONLY on the client).
        int dz_status = 0;
        syscall(SYS_WAITPID, dz_client_pid, (long)&dz_status, 0);
        print("[INIT] DZ_MPLIVE: dzclient session complete\n");
    }
#endif

#ifdef DZ_MPGUI_DEMO
    /* DZ_MPGUI_DEMO: on-screen (QEMU) 2-window LIVE co-op demo. Spawn the
     * authoritative server, then TWO GUI DeadZone windows that AUTO-JOIN it
     * (deadzone.c auto-connects under -DDZ_MPGUI_DEMO). The compositor shows two
     * game windows, each rendering the OTHER player as a cyan teammate sprite.
     * GUI-visual validation only; gated, so the default boot is unaffected. */
    print("[INIT] DZ_MPGUI_DEMO: spawning deadzoned + 2 DeadZone co-op windows...\n");
    spawn("sbin/deadzoned");
    sleep(1200);                       /* server binds/listens + compositor settles */
    spawn("sbin/deadzone");
    sleep(400);
    spawn("sbin/deadzone");
#endif

    // Auto-DHCP: spawns in the background, sleeps 2s (NIC PHY negotiate),
    // checks link, runs DHCP if up, applies lease, exits. Non-blocking --
    // init does not wait for it. If no NIC or DHCP fails, exits silently.
    print("[INIT] Spawning autodhcp...\n");
    spawn("sbin/autodhcp");

    // M3: spawn a test client that creates a window over the SHM protocol.
    // (No settle delay: both processes are queued and the SysV inbox queue is
    // created with IPC_CREAT by whichever side calls msgget first.)
    print("[INIT] Spawning terminal...\n");
    int term_pid = spawn("sbin/terminal");
    if (term_pid > 0) {
        print("[INIT] terminal started (PID ");
        print_num(term_pid);
        print(")\n");
    } else {
        print("[INIT] ERROR: Failed to spawn terminal!\n");
    }

    print("[INIT] Spawning file manager...\n");
    int fm_pid = spawn("sbin/filemanager");
    if (fm_pid > 0) {
        print("[INIT] filemanager started (PID ");
        print_num(fm_pid);
        print(")\n");
    } else {
        print("[INIT] ERROR: Failed to spawn filemanager!\n");
    }

#ifdef DZ_GAMETEST
    // DZ_GAMETEST (DZ_GAMETEST=1): spawn the DeadZone GUI game so its launch-time
    // headless selftests (systems / mp / loot) print to serial for the loot proof.
    // The compositor is up by here, so wl_connect succeeds; deadzone runs its game
    // loop afterward (harmless headless). Gated => default boot never spawns it.
    print("[INIT] DZ_GAMETEST: spawning deadzone (selftest proof)...\n");
    spawn("sbin/deadzone");
#endif

#ifdef TASKMAN_TEST
    // TASKMAN_TEST (TASKMAN_TEST=1): spawn the real Task Manager so its launch
    // emits [TASKMAN] starting + [TASKMAN] N procs (N = the TRUE live process
    // count via SYS_PROCLIST, now honest -- no longer silently pinned at 12),
    // proving the task manager reads real kernel data. Compositor is up by here,
    // so wl_connect succeeds. Gated => default boot never spawns it.
    print("[INIT] TASKMAN_TEST: spawning taskman (proclist proof)...\n");
    spawn("sbin/taskman");
#endif

#ifdef BLOCKRECV_TEST
    // NET-BLOCK-0 (BLOCKRECV_TEST=1): two sbin/blockrecv instances rendezvous
    // over loopback -- the first binds the port (SERVER: blocking accept, then
    // sleep+send), the second's bind fails (CLIENT: blocking recv). Proves
    // blocking accept + blocking recv wait at zero CPU (the [NETBLOCK] slice
    // markers) and return the data, instead of busy-spinning on EAGAIN. Net is
    // up by here (autodhcp ran above; loopback needs no NIC). Gated => default
    // boot never spawns it.
    print("[INIT] BLOCKRECV_TEST: spawning blockrecv x2 (NET-BLOCK-0 proof)...\n");
    spawn("sbin/blockrecv");
    spawn("sbin/blockrecv");
#endif

#ifdef AUDIO_STREAMTEST
    // AUDIO_STREAMTEST: prove a ring-3 app streams PCM via SYS_AUDIO_STREAM_WRITE
    // (the on_bcis ring consumer drains it into the HDA DMA). Gated => default boot
    // never spawns it. Pair with a kernel built AUDIO_SELFTEST=1 (implies HDA_ENABLE)
    // so the syscall handler + ring exist. Wait on it (it streams ~1-2s) so it gets
    // CPU before the self-test storm.
    print("[INIT] AUDIO_STREAMTEST: spawning streamtest (userspace PCM stream proof)...\n");
    {
        int stt_pid = spawn("sbin/streamtest");
        if (stt_pid > 0) { int stt_st = 0; syscall(SYS_WAITPID, stt_pid, (long)&stt_st, 0); }
    }
#endif

    // NOTE: the right-side dock IS the launcher now, so we no longer auto-open
    // the Applications grid window (it duplicated the dock + covered the desktop).
    // DECLUTTER: dateapp ("Date & Time") is no longer auto-opened either -- it
    // duplicated the panel clock + the Clock+ app and just added a boot window to
    // the cascade. It remains launchable from the dock.
    // spawn("sbin/dateapp");

    // prioritytest's pid (referenced by the reaper loop). Declared here so it is
    // visible in BOTH the full and DESKTOP_MINIMAL builds; -1 means "never spawned".
    int prioritytest_pid = -1;
    // agentd's pid is likewise referenced by the reaper loop (the AGENT-LEDGER
    // re-verify on agentd's reap) in BOTH builds, but agentd is only spawned in the
    // full build. Declare it unconditionally (-1 = "never spawned") so the
    // DESKTOP_MINIMAL build compiles -- it was previously declared inside the
    // #ifndef DESKTOP_MINIMAL storm, which broke the minimal build.
    int agentd_pid = -1;

    // ── DESKTOP_MINIMAL boot trim ───────────────────────────────────────────
    // When built with -DDESKTOP_MINIMAL (the T410 desktop profile), init spawns
    // ONLY the persistent desktop apps (compositor + terminal + filemanager +
    // netman + browser + ide) and SKIPS the ~70 self-test programs below. Those
    // tests are a dev smoke-suite, not part of the desktop: several (cryptotest,
    // matbench, prioritytest) run long no-yield compute blocks that, on the slow
    // cooperative T410, hold the CPU for seconds (the boot "lag"), and the rapid
    // create/destroy churn of ~80 short-lived processes stresses the PCID/address-
    // space teardown path. Trimming them gives a fast, smooth, low-churn boot and
    // stops the SELFHEAL watchdog from false-tripping while the storm starves the
    // compositor. The full self-test boot is still available by omitting the flag.
#ifndef DESKTOP_MINIMAL
    // fork/CoW correctness probe: runs once, prints FORKTEST RESULT to serial,
    // exits. Verifies fork address-space isolation (eager copy today, CoW next).
    print("[INIT] Spawning forktest...\n");
    spawn("sbin/forktest");

    // SIG-FULL-0 (B8) signal-delivery probe: installs a SIGUSR1 handler, raises
    // it to self (handler runs + resumes), proves blocked-signal pending/unblock,
    // default-terminate in a child, and bad-handler fail-safe. Prints SIGTEST
    // RESULT to serial, exits. Gated with the rest of the self-test boot.
    print("[INIT] Spawning sigtest...\n");
    spawn("sbin/sigtest");

    // NEGUPTR-0 regression probe: hands SYS_OPEN/SYS_STAT/SYS_UNLINK a
    // deliberately non-page-aligned, unmapped user pointer as the path arg.
    // On a fixed kernel each call returns a negative EFAULT-class error and
    // the process survives to print "NEGUPTR: PASS"; on the pre-fix kernel
    // the first such call panics the kernel in ring 0 (copy_user_string only
    // checked accessibility on 4K-aligned addresses), so this marker never
    // appears and boot itself dies. Prints NEGUPTR RESULT to serial, exits.
    print("[INIT] Spawning neguptr...\n");
    spawn("sbin/neguptr");

    // NEGCACHAIN-0 regression probe: calls x509_test_cert_is_ca() (the
    // x509_verify.c fix's public test hook) against hand-crafted cA=TRUE /
    // cA=FALSE / no-extension DER fixtures plus every real CA-bundle root.
    // Pre-fix, cert_is_ca()/x509_test_cert_is_ca() do not exist at all, so
    // this app fails to LINK and never ships -- "NEGCACHAIN: PASS" never
    // appears in the serial log. Prints NEGCACHAIN: PASS, exits.
    print("[INIT] Spawning negcachain...\n");
    spawn("sbin/negcachain");

    // NEGRSP/NEGSOCK/NEGDIR-0 regression probes (KERNEL-ROBUST-0 HIGHs):
    // negrsp forks a child that wrecks user_rsp before signal delivery (fixed
    // kernel default-terminates it; a regressed kernel spins with IF=0 and
    // wedges boot -> the outer smoke timeout catches it); negsock forks a
    // child that tries cross-process shutdown/setsockopt/getsockopt on the
    // parent's socket (must come back EBADF); negdir renames onto a non-empty
    // dir and a dir into itself (must be rejected EINVAL). Each prints
    // "<TAG>: PASS"; smoke_boot.sh gates on the markers.
    print("[INIT] Spawning negrsp...\n");
    spawn("sbin/negrsp");
    print("[INIT] Spawning negsock...\n");
    spawn("sbin/negsock");
    print("[INIT] Spawning negdir...\n");
    spawn("sbin/negdir");
    // FWTEST (FW-0/PCAP-0): kernel packet-filter control plane + ENFORCED privilege drops
    // (a child that drops CAP_NET_ADMIN/RAW is refused by the kernel; its child inherits the
    // refusal; the privileged parent is unaffected). Prints "FWTEST: PASS" for smoke_boot.sh.
    print("[INIT] Spawning fwtest...\n");
    spawn("sbin/fwtest");

    // NEGSHMDT (KERNEL-SYSCALL-ROBUST-0 HIGH): shmget-without-attach then shmdt
    // the segment's canonical VA must be DENIED (IPC_EINVAL); a fixed kernel
    // prints "NEGSHMDT: PASS", a regressed one accepts the detach -> would drop
    // another owner's frames (cross-process UAF). smoke_boot.sh gates the marker.
    print("[INIT] Spawning negshmdt...\n");
    spawn("sbin/negshmdt");

    // POLL-SELECT-0 (B10) probe: poll()/select() over real fd readiness (a
    // ready file vs an idle socket, a timeout, a mixed set) + epoll level/edge.
    // Prints POLLSELFTEST RESULT to serial, exits.
    print("[INIT] Spawning pollselftest...\n");
    spawn("sbin/pollselftest");

    // fork-fd-table inheritance probe: parent opens a ramfs file, forks,
    // the child writes through the INHERITED fd, then the parent writes
    // through the same still-live fd after reaping. Prints FORKFDTEST
    // RESULT to serial, exits.
    print("[INIT] Spawning forkfdtest...\n");
    spawn("sbin/forkfdtest");

    // FORK-REGS-INHERIT-0 probe: a fork()ed child must resume with the
    // parent callee-saved registers intact. Prints FORKREGTEST RESULT.
    print("[INIT] Spawning forkregtest...\n");
    spawn("sbin/forkregtest");

    // EXECVE-INPLACE-0 probe: exectest forks + execve("sbin/execchild") (same
    // PID, fresh argv/envp), proves no stray 3rd process + a failed execve leaves
    // the caller alive. Prints EXECTEST RESULT. NOTE: spawn ONLY exectest --
    // execchild must be reached EXCLUSIVELY via execve (a direct spawn would make
    // the C3 "no stray 3rd process" baseline non-zero).
    print("[INIT] Spawning exectest...\n");
    spawn("sbin/exectest");

    // real-threads probe: spawns 4 threads that SHARE this process's address
    // space but have independent stacks + FPU state, joins them, and prints
    // THREADTEST: PASS/FAIL. Proves shared memory, independent stacks, and
    // independent FPU across context switches.
    print("[INIT] Spawning threadtest...\n");
    spawn("sbin/threadtest");

    // argv handoff probe: verifies exec.c's argv frame + crt0 reach userspace
    // (prints ARGVTEST: PASS with argv[0] = its spawn path).
    print("[INIT] Spawning argvtest...\n");
    spawn_args("sbin/argvtest", "hello world");

#ifdef EX_ARGV_PROBE
    /* EX_ARGV_PROBE=1: prove the compositor's project-icon spawn convention --
     * SYS_SPAWN_EX_ARGV delivers a single argv[1] with embedded spaces INTACT (no
     * whitespace split, unlike the legacy spawn_args above). Spawn argvtest with a
     * spaced path + wait; the harness asserts the contiguous "My Game" survives. */
    print("[INIT] EX_ARGV_PROBE: spawn argvtest via SYS_SPAWN_EX_ARGV (spaced argv[1])...\n");
    {
        int pid = spawn_ex_argv1("sbin/argvtest", "/Desktop/Projects/My Game");
        if (pid > 0) { int st = 0; syscall(SYS_WAITPID, pid, (long)&st, 0); }
    }
#endif

    // CHANNEL-0 P5b: userspace CH_MSG send/recv round-trip. The parent creates a
    // CH_MSG channel, proves EAGAIN+EMSGSIZE, self-spawns a bound child, and
    // exchanges a framed packet both ways. Prints MSGTEST: PASS to serial.
    print("[INIT] Spawning msgtest...\n");
    spawn("sbin/msgtest");

    // AGENT-RPC-0 P6a: encode/decode/validate the TOOL_RUN/TOOL_RESULT wire
    // schema (no channels, no dispatch). Prints RPCTEST: PASS to serial.
    print("[INIT] Spawning rpctest...\n");
    spawn("sbin/rpctest");

    // AGENT-RPC-0 P6b: path-only TOOL_RUN runner. agent -> runner -> /bin/free
    // (stdout bound to a byte channel) -> TOOL_RESULT. Prints TOOLRUN/RUNNER PASS.
    print("[INIT] Spawning toolrun...\n");
    spawn("sbin/toolrun");

    // AGENT-HOST-0: the first agent riding the AGENT-RPC-0 rail -- issue TOOL_RUN,
    // read the tool's exact stdout via the capability, render a structured verdict,
    // reject a malformed call. Prints AGENTHOST: PASS.
    print("[INIT] Spawning agenthost...\n");
    spawn("sbin/agenthost");

    // TOOLSET-0: the safe whitelisted tool surface (read_file/list_dir/stat/run)
    // with a host-side path policy. Prints TOOLSET: PASS.
    print("[INIT] Spawning toolset_host...\n");
    spawn("sbin/toolset_host");

    // CHAINLAYER-HOST-0: model-in-the-loop -- a (stub) model chooses a tool as
    // JSON, the host validates + runs it, the model answers. Prints CHAINHOST: PASS.
    print("[INIT] Spawning chainhost...\n");
    spawn("sbin/chainhost");

    // ring-3 float/SSE probe: proves SSE is enabled + context-switched for user
    // tasks (scalar float, a 2x2 float matmul, a reduction). Prints FLOATTEST: PASS.
    print("[INIT] Spawning floattest...\n");
    spawn("sbin/floattest");

    // matbench: SIMD float matmul benchmark (scalar baseline vs hand-vectorized
    // SSE) -- prints MATBENCH with the measured speedup + a correctness PASS.
    print("[INIT] Spawning matbench...\n");
    spawn("sbin/matbench");

    // tensortest: SSE tensor-kernel library self-test (matmul/add/scale/relu/dot
    // each compared against an independent scalar reference). Prints TENSORTEST: PASS.
    print("[INIT] Spawning tensortest...\n");
    spawn("sbin/tensortest");

    // matmuljobs: a matmul run THROUGH the userspace job queue -- the rows are
    // partitioned into jobs, dispatched to worker threads on shared memory,
    // drained, and verified bit-identical to the scalar reference. This proves
    // the compute-COORDINATION machinery (submit/pull/compute/collect) is
    // correct. NOT a speedup: single-core means the workers time-share one cpu,
    // so it is *slower* than inline -- the parallel win waits for SMP. Prints
    // "matmuljobs: PASS result-matches-ref ...".
    print("[INIT] Spawning matmuljobs...\n");
    spawn("sbin/matmuljobs");

    // batchdemo: a tiny single-threaded marked-syscall workload (3 marks +
    // 48 FS reads + exit 7). On a default kernel it is just another quick
    // test app on CPU0. On a DESKTOP-SPLIT kernel this spawn is THE userspace
    // sys_spawn proof: the spawn syscall's BATCH allowlist declares it
    // BATCH+multi-CPU and the scheduler seam routes it to CPU1 -- an ordinary
    // userspace child doing ordinary work on the second core, under typed intent.
    print("[INIT] Spawning batchdemo...\n");
    spawn("sbin/batchdemo");

    // AI-native layer: the capability-gated command broker (crown jewel) runs a
    // self-test of its tool-bus + policy + ledger + rollback pipeline.
    print("[INIT] Spawning aibroker...\n");
    spawn("sbin/aibroker");

    // Standard tools self-verify on boot (each runs its self-test + exits). This
    // turns the smoke boot into a self-test suite for the whole tool layer.
    print("[INIT] Self-testing /bin tools...\n");
    spawn("bin/sed");
    spawn("bin/awk");
    spawn("bin/tar");
    spawn("bin/pkg");
    spawn("bin/make");
    spawn("bin/meminfo");

    // Wave: self-hosting + sysutils self-tests. cc compiles a hello program on
    // device (the self-hosting flagship), gzip round-trips a buffer, and the
    // process/file utilities each verify their core path. Each exits after.
    print("[INIT] Self-testing compiler + sysutils...\n");
    spawn("bin/cc");
    spawn("bin/gzip");
    spawn("bin/ps");
    spawn("bin/kill");
    spawn("bin/free");
    spawn("bin/uptime");
    spawn("bin/find");
    spawn("bin/diff");
    spawn("bin/cmp");
    spawn("bin/tee");
    spawn("bin/wcx");
    spawn("bin/xargs");
    spawn("bin/blk");

    // Networking probe: queries the NIC (MAC/IP via SYS_NET_INFO), TXes a
    // broadcast ARP for the gateway, and polls for the reply.
    print("[INIT] Spawning nettest...\n");
    spawn("sbin/nettest");

    // Socket layer probe: UDP sendto + bounded recv + TCP descriptor alloc.
    print("[INIT] Spawning sockettest...\n");
    spawn("sbin/sockettest");

    // MODEL-BRIDGE-0: the model seam fed by an EXTERNAL endpoint (10.0.2.2:8431).
    // Bounded probes: SKIPs cleanly when networking or the endpoint is absent;
    // with the host-side model stub up it prints MODELBRIDGE: PASS.
    print("[INIT] Spawning modelbridge...\n");
    spawn("sbin/modelbridge");

    // CLAUDE-API-0: claudehost -- send a prompt to the host Claude broker over
    // the slirp seam (10.0.2.2:8432) and print Claude's reply. SKIPs cleanly
    // when net/broker absent. Run `python3 scripts/claude_broker.py` on the
    // host (with ANTHROPIC_API_KEY set, for a real billed call) + boot -netdev.
    print("[INIT] Spawning claudehost (USE Claude over the network)...\n");
    spawn("sbin/claudehost");

    // NEMOTRON-AGENT: agentd -- the OS-side gated agentic loop. Connects to the host
    // model broker (10.0.2.2:8433) and runs a multi-step GOAL/TOOL/RESULT/DONE loop,
    // dispatching only whitelisted tools through the path-traversal gate. SKIPs cleanly
    // (AGENTD: SKIP no_net/no_broker) when no broker is up, so a normal boot is unaffected.
    // Bring it to life with `python3 scripts/nemotron_mock.py` (zero-cost) or
    // `node scripts/nemotron_broker.js` (live NVIDIA/Puter Nemotron) + boot -netdev.
    print("[INIT] Spawning agentd (Nemotron gated OS-automation agent)...\n");
    agentd_pid = spawn("sbin/agentd");

#ifdef COCKPIT_PROOF
    // AGENTCOCKPIT-0 headless seam proof: launch the cockpit in --proof mode so it
    // auto-posts a goal + auto-RUNs with no human (build_test/run_cockpit.sh asserts the
    // cockpit<->agentd markers). Gated behind COCKPIT_PROOF so a normal boot NEVER auto-runs
    // it -- the cockpit otherwise stays launchable from the dock/start menu.
    print("[INIT] COCKPIT_PROOF: launching sbin/cockpit --proof...\n");
    spawn_args("sbin/cockpit", "--proof");
#endif

    // cpu1offload: userspace -> CPU1 matmul offload probe. On the SMP kernel it
    // offloads an int matmul to CPU1 (the trusted coprocessor) via SYS_CPU1_OFFLOAD
    // and prints "CPU1OFFLOAD: PASS ... by_apic=1"; on the DEFAULT (single-core)
    // kernel the syscall is unregistered, so it prints "CPU1OFFLOAD: SKIP" and
    // exits cleanly -- harmless in the default boot.
    print("[INIT] Spawning cpu1offload...\n");
    spawn("sbin/cpu1offload");

    // smpstress: the 2-CPU dispatch STRESS harness. Drives the CPU1 coprocessor
    // mailbox thousands of times and verifies every result -- the proving ground the
    // SMP scaling work is validated against BEFORE any per-CPU scheduler change. On
    // the SMP kernel: "SMPSTRESS: PASS jobs=...". On the DEFAULT kernel SYS_CPU1_OFFLOAD
    // is unregistered, so it prints "SMPSTRESS: SKIP single CPU" and exits cleanly.
    print("[INIT] Spawning smpstress...\n");
    spawn("sbin/smpstress");

    // wget self-test (URL parse + dotted-quad DNS, no network -> exits).
    spawn("bin/wget");

    // Crypto/HTTPS KATs (SHA/AES/HMAC/RSA/X.509/TLS-PRF) + misc lib KATs
    // (JSON/DHCP/image codecs). Deterministic, no network; each exits.
    print("[INIT] Self-testing crypto + libs...\n");
    spawn("sbin/cryptotest");
    spawn("sbin/libtest");
    // MATH-0: fpm (Q16.16 fixed-point) KAT battery -- 308 bit-exact checks
    // against host-libm-derived constants. Deterministic; exits.
    spawn("sbin/mathtest");

    // JavaScript engine self-test (runs the embedded ES5 script battery).
    spawn("bin/js");
    // HTTPS stack tool self-tests (deterministic; real interop is run manually).
    spawn("bin/tlsprobe");
    spawn("bin/certtool");
    spawn("bin/dhcpc");
    spawn("bin/apidemo");

    // Overhaul-syscall verification (futex/epoll/sendfile/perf/batch) -- each
    // exercises the syscall's real ABI with a bounded self-test + exits.
    print("[INIT] Self-testing overhaul syscalls...\n");
    spawn("sbin/futextest");
    spawn("sbin/epolltest");
    spawn("sbin/sendfiletest");
    spawn("sbin/perftest");
    spawn("sbin/batchtest");

    // Networking tools self-test (ping ICMP to gateway, nc socket API).
    spawn("bin/ping");
    spawn("bin/nc");

    // coreutils + sysinfo self-tests (each runs an in-memory check + exits).
    print("[INIT] Self-testing coreutils...\n");
    spawn("bin/grep");  spawn("bin/head");  spawn("bin/tail");  spawn("bin/sort");
    spawn("bin/uniq");  spawn("bin/cut");   spawn("bin/tr");    spawn("bin/nl");
    spawn("bin/du");    spawn("bin/touch");
    spawn("bin/basename"); spawn("bin/dirname");
    spawn("bin/uname"); spawn("bin/hostname"); spawn("bin/whoami"); spawn("bin/date");
    spawn("bin/less");  spawn("bin/hexdump");
    spawn("bin/lspci");

#endif  // !DESKTOP_MINIMAL (self-test storm, part A)

    // Network manager + web browser GUIs (open windows; user-facing net apps).
    // PERSISTENT desktop apps -- spawned in BOTH the full and minimal builds.
    print("[INIT] Spawning netman + browser2...\n");
    spawn("sbin/netman");
    spawn("sbin/soundman");   // Sound Manager GUI (HDA volume/mute/test-tone via SYS_AUDIO_*)
    spawn("sbin/wlanctl");    // WIFI-SYS M1: one-shot SYS_WLAN_SCAN probe (prints the AP list, or ENOTSUP with no wifi); exits
    spawn("sbin/wpasupp");    // WIFI M2: WPA2 4-way handshake KAT selftest -> WPASUPP SELFTEST: PASS; exits
#ifdef WIFI_DEMO
    // Headless connect proof (the Network Manager GUI is the real, user-driven
    // trigger): connect wlan0 to the simulated HomeNet (WPA2) -> 4-way -> set_key
    // -> CONNECTED -> dhcpc run wlan0. Needs a WIFI_SIM kernel.
    spawn_args("sbin/wpasupp", "HomeNet 1 password");
#endif
#ifdef WIFI_DEMO_WPA3
    // Headless WPA3 connect proof: wlan0 -> the simulated SecureMesh (WPA3/SAE
    // dragonfly -> PMK -> 4-way -> CONNECTED -> dhcp).
    spawn_args("sbin/wpasupp", "SecureMesh 2 password");
#endif
#ifdef WIFI_DEMO_FAIL
    // Negative-path proof: a WRONG passphrase must reach WLAN_FAILED -- the sim
    // rejects it and the supplicant must NOT install keys.
    spawn_args("sbin/wpasupp", "HomeNet 1 wrongpass");
#endif
    spawn("sbin/browser2");   // BROWSER-CONSOLIDATE-0: the one real (DOM/CSS/JS/HTTPS) browser

#ifndef DESKTOP_MINIMAL
    // Browser wave (22-agent): per-layer selftests + the new DOM-rendering
    // browser2. Each app prints "<NAME>: PASS" or "<NAME>: FAIL <which>" and
    // exits, so smoke can gate the entire web pipeline.
    print("[INIT] Self-testing browser pipeline (DOM/HTML/CSS/layout/JS-bridge)...\n");
    spawn("sbin/domtest");
    spawn("sbin/htmltest");
    spawn("sbin/csstest");
    spawn("sbin/layouttest");
    spawn("sbin/webtest");
    // BROWSER-DEDUP: these two BOUNDED browser2 self-test runs (--smoke +
    // about:imgtest) exist only to feed the smoke / img / alias verify gates.
    // Each briefly opens a browser window, so on a normal desktop they looked
    // like "double/triple browsers". Gated behind SMOKE_SELFTEST (default OFF)
    // so a normal boot opens exactly ONE browser -- the persistent one above.
    // The img/alias verify scripts build with SMOKE_SELFTEST=1.
#ifdef SMOKE_SELFTEST
    // BROWSER-PERSIST-0: bounded "--smoke" mode (render about:home once, print
    // the verdict, exit) -- gates the web stack without a lingering window.
    spawn_args("sbin/browser2", "--smoke");
    // BROWSER2-IMG-0: bounded run on about:imgtest (PNG/GIF/BMP fixtures + a
    // missing source + a wider-than-viewport image). Prints "BROWSER2-IMG: PASS
    // png=1 gif=1 bmp=1 missing_safe=1 bounded=1" after its first paint, exits.
    spawn_args("sbin/browser2", "about:imgtest");
#endif

    // INITRD-ALIAS-0: the big-image (16 MiB pad, spans VA 16 MiB) + mmap-heavy
    // reader; reads an initrd-backed file on its own shadow-prone CR3,
    // byte-compares against the embedded fixture, spawns the tiny pristine
    // control (sbin/initrdp), and prints "INITRD-ALIAS: PASS pristine_read=1
    // mmapheavy_read=1 same_bytes=1 zero_bug_gone=1".
    print("[INIT] Spawning initrdalias...\n");
    spawn("sbin/initrdalias");
    // webapitest: pure JS web-API selftest (timers/fetch/storage/console/url);
    // prints "WEBAPITEST: PASS" and exits.
    spawn("sbin/webapitest");

    // sleeptest: proves SYS_SLEEP is a real, ms-granularity, BLOCKING sleep (the
    // process goes BLOCKED, accrues ~no CPU, and the timer wakes it at its
    // deadline). It sleeps 50 ms and checks the measured elapsed ms is in a tight
    // window. Spawned LAST (after the boot-time spawn storm has drained) so the
    // measured elapsed reflects the sleep itself, not ready-queue dispatch latency
    // behind dozens of still-spawning boot apps. Works in BOTH builds.
    print("[INIT] Spawning sleeptest...\n");
    spawn("sbin/sleeptest");

    // prioritytest: PROVES scheduler priority classes actually shift CPU share.
    // Forks two CPU-bound children (one SCHED_CLASS_HIGH / nice -10, one
    // SCHED_CLASS_BACKGROUND / nice +10), runs them a fixed ~1.5s window with
    // periodic yields, then compares their per-process cpu_ticks (SYS_PROCLIST)
    // and prints "PRIORITYTEST: PASS" iff HIGH got meaningfully more CPU. Works
    // in BOTH the cooperative and preemptive builds. Spawned LAST (after the
    // boot-time spawn storm has drained) so the two burners get a clean window
    // rather than fighting dozens of still-spawning boot apps for the CPU.
    print("[INIT] Spawning prioritytest...\n");
    // Capture prioritytest's pid: the PID-recycling proof (reaploop) is launched
    // only AFTER prioritytest is reaped (in the loop below), so reaploop's fork load
    // never perturbs the timing-sensitive sleeptest/prioritytest measurements. By
    // the time prioritytest (the last + slowest timing probe) exits, sleeptest has
    // long since finished and the boot storm has drained -- a clean, settled system.
    prioritytest_pid = spawn("sbin/prioritytest");

#ifdef PREEMPT_STRESS
    // ========================================================================
    // PREEMPTIVE-SCHEDULER STRESS WORKLOAD  (compiled ONLY with -DPREEMPT_STRESS,
    // which scripts/build_all.sh adds when STRESS=1; the plain PREEMPT=1 build
    // and every cooperative build leave this block ABSENT). It is placed AFTER
    // all the normal app/desktop spawns above so the compositor + desktop still
    // come up first, THEN we unleash the abuse.
    //
    // cpuburn loops FOREVER without ever yielding/sleeping/blocking. Under the
    // cooperative kernel the first burner would monopolize the CPU and hang the
    // box -- which is exactly why this is gated. Under the preemptive kernel the
    // timer time-slices ring 3, so all six burners (ids 0..5) must make progress
    // (interleaved heartbeats == fairness/no-starvation), and the three extra
    // floattest runs exercise SSE save/restore repeatedly under preemption.
    // ========================================================================
    print("[INIT] PREEMPT_STRESS: launching CPU burners + float load...\n");
    spawn_args("sbin/cpuburn", "0");
    spawn_args("sbin/cpuburn", "1");
    spawn_args("sbin/cpuburn", "2");
    spawn_args("sbin/cpuburn", "3");
    spawn_args("sbin/cpuburn", "4");
    spawn_args("sbin/cpuburn", "5");
    spawn("sbin/floattest");
    spawn("sbin/floattest");
    spawn("sbin/floattest");
    print("[INIT] PREEMPT_STRESS: 6 burners + 3 floattest spawned.\n");
#endif
#endif  // !DESKTOP_MINIMAL (self-test storm, part B)

#ifdef DNS_TEST
    /* DNS-LEASE-0 proof (test builds only): does the resolver follow the DHCP-learned DNS? */
    print("[INIT] Spawning dnstest...\n");
    spawn("sbin/dnstest");
#endif
#ifdef NET_LIVE
    /* LIVE-Internet proof (test builds only): real DNS + HTTP + verified HTTPS. */
    print("[INIT] Spawning livenet...\n");
    spawn("sbin/livenet");
#endif
#ifdef WIN_TEST
    /* WIN-MIN proof (test builds only): run each Windows fixture to completion, one at a time. */
    {
        static const char* const wf[] = { "/usr/share/winfix/nocrt_hello.exe", "/usr/share/winfix/crt_hello.exe",
                                          "/usr/share/winfix/bad_trunc.exe", "/usr/share/winfix/bad_magic.exe",
                                          "/usr/share/winfix/bad_machine.exe", 0 };
        for (int wi = 0; wf[wi]; wi++) {
            int wp = spawn_args("bin/winrun", wf[wi]);
            if (wp > 0) { int wst = 0; syscall(SYS_WAITPID, wp, (long)&wst, 0); }
        }
        print("[INIT] WINTEST: done\n");
    }
#endif

#if defined(AGENTD_SERVE) && defined(DESKTOP_MINIMAL)
    /* AGENTD-SERVE-0: the LEAN desktop profile has no storm, so spawn the OS-side tool host here.
     * /etc/agentd.conf (serve=1) makes it a long-lived MCP-bridge tool host. (In FULL builds it
     * is already spawned by the storm above.) */
    print("[INIT] Spawning agentd (tool-host / serve mode)...\n");
    agentd_pid = spawn("sbin/agentd");
#endif

    print("[INIT] All services started!\n");

    // C4: verify the agent audit ledger's tamper-evident hash-chain (aibroker writes it
    // during its boot self-test). Prints "LEDGER: VERIFIED records=N" / "TAMPERED line=K"
    // / "EMPTY". A quick read-verify-exit; harmless on every boot.
    spawn("sbin/ledgerver");
    // AGENT-LEDGER: also verify the LIVE agent's ledger. On a default (no-broker) boot
    // agentd SKIPs, so /var/log/ai/agent.log is empty here -> "LEDGER: EMPTY" (fine). The
    // authoritative verify is the LATE one keyed to agentd's reap in the reaper loop.
    spawn_args("sbin/ledgerver", "/var/log/ai/agent.log");

#ifdef FAIRTEST
    // SCHED-FAIRNESS-0 proof (build FAIRTEST=1 PREEMPT=1 DESKTOP_MINIMAL=1).
    // Spawn pure non-syscalling ring-3 burners (sbin/pureburn) + a sleeper
    // (sbin/fairwake). With no compositor and init about to block in waitpid on
    // the never-exiting burners, NOTHING provides a cooperative-switch boundary,
    // so fairwake's post-sleep RESUME_CRETURN wake is dispatchable ONLY via the
    // IRQ-path fairness fix. It prints "FAIRWAKE: PASS" iff the fix works; under
    // the old (buggy) scheduler it is starved forever and prints nothing.
    print("[INIT] FAIRTEST: 2 pure burners + 1 sleeper...\n");
    spawn_args("sbin/pureburn", "0");
    spawn_args("sbin/pureburn", "1");
    spawn("sbin/fairwake");
#endif

#ifdef GAMETEST_RUN
    /* Empirical "every app actually runs" harness: spawns each game + key app,
     * lets it run its init+render loop ~2s, checks it survives, prints
     * GAMETEST: <name> PASS/FAIL + a final GAMETEST: PASS/FAIL. Only compiled
     * when init is built with -DGAMETEST_RUN (GAMETEST=1); the normal boot is
     * unaffected. */
    print("[INIT] GAMETEST: spawning game/app survival harness...\n");
    spawn("sbin/gametest");
#endif

#ifdef IDE_AUTOSTART
    /* IDE=1 build: open the Semantic LEGO Map IDE last so it lands on TOP of the
     * default desktop apps (for IDE iteration + screenshots). Normal boot leaves
     * the IDE launchable from the dock/start-menu instead. */
    print("[INIT] IDE_AUTOSTART: opening sbin/ide on the DeadZone project...\n");
    /* AUDIT-9: hand the IDE the DeadZone project root as argv[1] so the boot
     * drives the runtime project-load + prebuilt-gate path and emits the
     * headless proof markers ([IDE] project active / [IDE] prebuilt probe). */
    spawn_args("sbin/ide", "/Desktop/Projects/DeadZone");
#endif

#ifdef DZ_SRVTEST
    /* DZ_SRVTEST=1: run the authoritative server's headless self-test (which now
     * also proves inactive slots serialize as DZ_SLOT_EMPTY -- the MP phantom-
     * teammate fix) and WAIT for it so the boot storm can't starve it. */
    print("[INIT] DZ_SRVTEST: running deadzoned -t...\n");
    {
        int srv = spawn_args("sbin/deadzoned", "-t");
        if (srv > 0) { int st = 0; syscall(SYS_WAITPID, srv, (long)&st, 0); }
    }
#endif

#ifdef SHOWCASE
    /* SHOWCASE=1 build: open a curated set of headline GUI apps cascaded on top
     * of the desktop, so one boot can be screendumped (+ Alt+Tab cycled) to
     * photograph each. Normal boot leaves them launchable from the dock instead. */
    print("[INIT] SHOWCASE: opening headline apps (soundman, cockpit, zombietd, ide)...\n");
    spawn("sbin/soundman");
    spawn("sbin/ide");
    spawn("sbin/zombietd");
    spawn("sbin/cube3d");
    spawn("sbin/cockpit");
    spawn("sbin/browser2"); /* SHOWCASE_TOP picks which spawns LAST (on top) */
#endif

    // PID-recycling proof (#9) is launched from the reaper loop below, exactly once,
    // when prioritytest is reaped -- by then the boot self-test storm has drained
    // and the timing-sensitive probes have finished, so reaploop runs on a settled
    // system (clean PID reuse) without perturbing any measurement.
    int reaploop_spawned = 0;

    /* One authoritative compositor supervisor. It uses monotonic time, bounded
     * backoff and a temporary circuit cooldown; no failure permanently disables
     * the desktop. PID 1 only ever kills its currently tracked child, avoiding
     * the stale-PID race in the former external watchdog. */
    int  comp_failures = 0;
    int  comp_restart_pending = compositor_pid <= 0;
    int  comp_healthy = 0;
    int  comp_hb_seeded = 0;
    int  comp_hb_advances = 0;
    int  comp_kill_sent = 0;
    int  comp_generation = compositor_pid > 0 ? 1 : 0;
    long comp_started_ms = now_ms();
    long comp_healthy_since_ms = 0;
    long comp_hb_changed_ms = comp_started_ms;
    long comp_next_start_ms = comp_restart_pending ? comp_started_ms + 250 : 0;
    long comp_circuit_until_ms = 0;
    unsigned long long comp_hb_last = 0;

    while (1) {
        int status;
        int pid;
        /* Drain every available zombie without blocking so health and restart
         * deadlines continue to run even when no child exits. */
        while ((pid = (int)syscall(SYS_WAITPID, -1, (long)&status, WNOHANG)) > 0) {
            print("[INIT] Process ");
            print_num(pid);
            print(" exited with status ");
            print_num(status);
            print("\n");

            // Launch the PID-recycling proof exactly once, when prioritytest (the
            // last + slowest timing-sensitive boot probe) is reaped. By now the boot
            // storm has drained and the timing probes have finished, so reaploop
            // gets a free pool + clean PID reuse and cannot perturb sleeptest/
            // prioritytest (already done). (#9 zombie/PID-leak proof.)
            if (pid == prioritytest_pid && !reaploop_spawned) {
                reaploop_spawned = 1;
                print("[INIT] Spawning reaploop (PID-recycling proof)...\n");
                spawn("sbin/reaploop");
            }

            // AGENT-LEDGER: once agentd's broker loop finishes and it is reaped, its live
            // ledger /var/log/ai/agent.log is fully written -- verify it NOW. Keying to
            // agentd's exit (not a fixed delay) closes the race where the boot-time
            // ledgerver ran before any agent decision was logged.
            if (pid == agentd_pid && agentd_pid > 0) {
                print("[INIT] AGENT-LEDGER: re-verifying agent.log after agentd reap...\n");
                spawn_args("sbin/ledgerver", "/var/log/ai/agent.log");
            }

            if (pid == compositor_pid) {
                long t = now_ms();
                compositor_pid = -1;
                comp_healthy = 0;
                comp_hb_seeded = 0;
                comp_hb_advances = 0;
                comp_kill_sent = 0;
                comp_failures++;
                comp_restart_pending = 1;

                if (comp_failures >= COMP_CIRCUIT_FAILS) {
                    comp_circuit_until_ms = t + COMP_CIRCUIT_COOLDOWN_MS;
                    comp_next_start_ms = comp_circuit_until_ms;
                    print_supervisor("circuit-open", -1, COMP_CIRCUIT_COOLDOWN_MS);
                } else {
                    long delay = compositor_backoff_ms(comp_failures);
                    comp_next_start_ms = t + delay;
                    print_supervisor("backoff", -1, delay);
                }
            }

            // Yield after each reap so a process woken DURING init's reap burst
            // (e.g. a sleeper hitting its deadline) is dispatched promptly. The #9
            // fix makes each reap do REAL teardown (CR3 destroy, free stack/pages) --
            // previously a no-op (the total leak), so reaps were ~instant. Without
            // this yield the cooperative reaper monopolizes the CPU through the
            // ~80-zombie boot drain and inflates other processes' wakeup-dispatch
            // latency (observed: a 50 ms sleep measured ~480 ms). Spreading the
            // teardown keeps the desktop + timing-sensitive probes responsive.
            yield();
        }

        long t = now_ms();

#ifdef SELFHEAL
        if (compositor_pid > 0 && heal && heal->magic == SELFHEAL_MAGIC &&
            heal->version == SELFHEAL_VERSION &&
            (int)heal->compositor_pid == compositor_pid) {
            unsigned long long fc = heal->frame_counter;
            if (!comp_hb_seeded) {
                comp_hb_seeded = 1;
                comp_hb_last = fc;
                comp_hb_changed_ms = t;
            } else if (fc != comp_hb_last) {
                comp_hb_last = fc;
                comp_hb_changed_ms = t;
                if (comp_hb_advances < 2) comp_hb_advances++;
                if (!comp_healthy && comp_hb_advances >= 2) {
                    comp_healthy = 1;
                    comp_healthy_since_ms = t;
                    print_supervisor("healthy", compositor_pid, -1);
                    if (comp_generation > 1)
                        print("[SUP] SELFHEAL: PASS respawned (two heartbeats)\n");
                }
            }
        }

        if (compositor_pid > 0 && !comp_kill_sent &&
            ((!comp_healthy && t - comp_started_ms >= COMP_START_TIMEOUT_MS) ||
             (comp_healthy && t - comp_hb_changed_ms >= COMP_STALL_MS))) {
            print_supervisor(comp_healthy ? "heartbeat-stalled" : "startup-timeout",
                             compositor_pid,
                             comp_healthy ? t - comp_hb_changed_ms : t - comp_started_ms);
            /* One immediate kill of PID 1's tracked child. There is no delayed
             * second kill that could hit a replacement after PID reuse. */
            syscall(SYS_KILL, compositor_pid, SIGKILL, 0);
            comp_kill_sent = 1;
            syscall(SYS_RECOVERY_OVERLAY, 0, 2000, 0);
            print("[SUP] SELFHEAL: recovery overlay fired\n");
            comp_healthy = 0;
            comp_hb_seeded = 0;
        }
#endif

#ifndef SELFHEAL
        if (compositor_pid > 0 && !comp_healthy &&
            t - comp_started_ms >= COMP_START_TIMEOUT_MS) {
            comp_healthy = 1;
            comp_healthy_since_ms = t;
            print_supervisor("healthy-no-heartbeat", compositor_pid, -1);
        }
#endif

        if (comp_healthy && comp_failures > 0 &&
            t - comp_healthy_since_ms >= COMP_STABLE_RESET_MS) {
            comp_failures = 0;
            comp_circuit_until_ms = 0;
            print_supervisor("stable-reset", compositor_pid, -1);
        }

        if (comp_restart_pending && compositor_pid <= 0 && t >= comp_next_start_ms) {
            if (comp_circuit_until_ms > t) {
                comp_next_start_ms = comp_circuit_until_ms;
            } else {
                print_supervisor("start", -1, -1);
                compositor_pid = spawn("sbin/compositor");
                if (compositor_pid > 0) {
                    comp_generation++;
                    comp_restart_pending = 0;
                    comp_started_ms = t;
                    comp_hb_changed_ms = t;
                    comp_hb_seeded = 0;
                    comp_hb_advances = 0;
                    comp_kill_sent = 0;
                    comp_healthy = 0;
                } else {
                    comp_failures++;
                    if (comp_failures >= COMP_CIRCUIT_FAILS) {
                        comp_circuit_until_ms = t + COMP_CIRCUIT_COOLDOWN_MS;
                        comp_next_start_ms = comp_circuit_until_ms;
                        print_supervisor("circuit-open", -1, COMP_CIRCUIT_COOLDOWN_MS);
                    } else {
                        long delay = compositor_backoff_ms(comp_failures);
                        comp_next_start_ms = t + delay;
                        print_supervisor("spawn-backoff", -1, delay);
                    }
                }
            }
        }

        sleep(COMP_POLL_MS);
    }
}
