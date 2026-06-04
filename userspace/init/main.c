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
#define SYS_WAITPID 6
#define SYS_YIELD   15

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

static size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static void print(const char* msg) {
    syscall(SYS_WRITE, 1, (long)msg, strlen(msg));
}

static void print_num(long n) {
    char buf[20];
    int i = 0;
    if (n < 0) { print("-"); n = -n; }
    do { buf[i++] = '0' + (n % 10); n /= 10; } while (n > 0);
    while (i > 0) { char c = buf[--i]; syscall(SYS_WRITE, 1, (long)&c, 1); }
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

static void yield(void) {
    syscall(SYS_YIELD, 0, 0, 0);
}

static void sleep(int ms) {
    syscall(SYS_SLEEP, ms, 0, 0);
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

    print("[INIT] Spawning compositor...\n");
    int compositor_pid = spawn("sbin/compositor");
    if (compositor_pid > 0) {
        print("[INIT] Compositor started (PID ");
        print_num(compositor_pid);
        print(")\n");
    } else {
        print("[INIT] ERROR: Failed to spawn compositor!\n");
    }
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

    // NOTE: the right-side dock IS the launcher now, so we no longer auto-open
    // the Applications grid window (it duplicated the dock + covered the desktop).
    // DECLUTTER: dateapp ("Date & Time") is no longer auto-opened either -- it
    // duplicated the panel clock + the Clock+ app and just added a boot window to
    // the cascade. It remains launchable from the dock.
    // spawn("sbin/dateapp");

    // fork/CoW correctness probe: runs once, prints FORKTEST RESULT to serial,
    // exits. Verifies fork address-space isolation (eager copy today, CoW next).
    print("[INIT] Spawning forktest...\n");
    spawn("sbin/forktest");

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

    // cpu1offload: userspace -> CPU1 matmul offload probe. On the SMP kernel it
    // offloads an int matmul to CPU1 (the trusted coprocessor) via SYS_CPU1_OFFLOAD
    // and prints "CPU1OFFLOAD: PASS ... by_apic=1"; on the DEFAULT (single-core)
    // kernel the syscall is unregistered, so it prints "CPU1OFFLOAD: SKIP" and
    // exits cleanly -- harmless in the default boot.
    print("[INIT] Spawning cpu1offload...\n");
    spawn("sbin/cpu1offload");

    // wget self-test (URL parse + dotted-quad DNS, no network -> exits).
    spawn("bin/wget");

    // Crypto/HTTPS KATs (SHA/AES/HMAC/RSA/X.509/TLS-PRF) + misc lib KATs
    // (JSON/DHCP/image codecs). Deterministic, no network; each exits.
    print("[INIT] Self-testing crypto + libs...\n");
    spawn("sbin/cryptotest");
    spawn("sbin/libtest");

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

    // Network manager + web browser GUIs (open windows; user-facing net apps).
    print("[INIT] Spawning netman + browser...\n");
    spawn("sbin/netman");
    spawn("sbin/browser");

    // Browser wave (22-agent): per-layer selftests + the new DOM-rendering
    // browser2. Each app prints "<NAME>: PASS" or "<NAME>: FAIL <which>" and
    // exits, so smoke can gate the entire web pipeline.
    print("[INIT] Self-testing browser pipeline (DOM/HTML/CSS/layout/JS-bridge)...\n");
    spawn("sbin/domtest");
    spawn("sbin/htmltest");
    spawn("sbin/csstest");
    spawn("sbin/layouttest");
    spawn("sbin/webtest");
    spawn("sbin/browser2");
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
    spawn("sbin/prioritytest");

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

    print("[INIT] All services started!\n");

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
    print("[INIT] IDE_AUTOSTART: opening sbin/ide...\n");
    spawn("sbin/ide");
#endif

    while (1) {
        int status;
        int pid = (int)syscall(SYS_WAITPID, -1, (long)&status, 0);
        if (pid > 0) {
            print("[INIT] Process ");
            print_num(pid);
            print(" exited with status ");
            print_num(status);
            print("\n");

            if (pid == compositor_pid) {
                print("[INIT] Restarting compositor...\n");
                compositor_pid = spawn("sbin/compositor");
            }
        } else {
            yield();
        }
    }
}
