// negshmdt -- KERNEL-SYSCALL-ROBUST-0 regression proof for the sys_shmdt
// cross-process use-after-free HIGH (kernel/ipc/shm.c). Self-contained (no
// libs, own _start, direct syscalls). Prints to fd 1 (serial). The smoke greps
// "NEGSHMDT: PASS".
//
// BUG (pre-fix, kernel/ipc/shm.c sys_shmdt; parent commit 0a7756a): shmdt
// resolved the target segment purely from the user-supplied virtual address
// (id = (va - SHM_VA_BASE) / SHM_VA_STRIDE reverse math), then decremented the
// SHARED seg->attach_count and could run the deferred-destroy free path WITHOUT
// checking that the caller ever attached the segment -- shm_attach_remove()'s
// false return (caller holds no attach record) was IGNORED. A process that
// never attached could therefore drive attach_count to 0 and free (via
// pmm_free_page on the pending-destroy path) physical frames another owner
// still maps: a cross-process use-after-free. Even with no co-owner, shmdt of a
// created-but-unattached segment wrongly returned IPC_SUCCESS.
//
// FIX (HEAD): sys_shmdt now calls shm_attach_remove(current, seg->id) FIRST and,
// if it returns false, unlocks and returns IPC_EINVAL before touching any
// shared state (unmap / attach_count / deferred destroy).
//
// DISCRIMINATOR (single process -- no co-owner or fork needed): create a
// segment with SYS_SHMGET but do NOT attach it, then SYS_SHMDT its canonical VA
// (SHM_VA_BASE + id*SHM_VA_STRIDE). The segment IS in id_table (so shmdt's VA
// reverse-lookup resolves it) but this process holds no attach record, so the
// FIXED kernel returns IPC_EINVAL (< 0) while the PRE-FIX kernel returns
// IPC_SUCCESS (0). A positive control (shmget + shmat + shmdt) proves a
// legitimate detach still succeeds (the ownership gate does not over-reject),
// and a trailing getpid proves the denied detach corrupted nothing (no free of
// live frames, no panic).

typedef unsigned long size_t;

#define SYS_EXIT     0
#define SYS_WRITE    3
#define SYS_GETPID   8
#define SYS_SHMGET   18
#define SYS_SHMAT    19
#define SYS_SHMDT    20

#define IPC_PRIVATE   0
#define IPC_CREAT     0x0200

// Must match kernel/ipc/shm.c.
#define SHM_VA_BASE   0x60000000UL
#define SHM_VA_STRIDE (16UL * 1024 * 1024)   // 16 MB per slot

static inline long sc6(long n, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}
#define sc3(n,a,b,c) sc6((n),(a),(b),(c),0,0)
#define sc0(n)       sc6((n),0,0,0,0,0)

static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_WRITE, 1, (long)s, (long)slen(s)); }

void _start(void) {
    int ok = 1;
    out("NEGSHMDT: start\n");

    // --- Positive control: a legitimately attached segment must still detach.
    //     Proves the new ownership gate does not reject a real detach. ---
    long id1 = sc3(SYS_SHMGET, IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    if (id1 > 0) {
        long addr = sc3(SYS_SHMAT, id1, 0, 0);
        if (addr > 0) {
            long rp = sc3(SYS_SHMDT, addr, 0, 0);
            if (rp == 0) {
                out("NEGSHMDT: [1] PASS legit attach+detach ok\n");
            } else {
                out("NEGSHMDT: [1] FAIL legit detach was rejected\n");
                ok = 0;
            }
        } else {
            out("NEGSHMDT: [1] FAIL shmat failed\n");
            ok = 0;
        }
    } else {
        out("NEGSHMDT: [1] FAIL shmget(control) failed\n");
        ok = 0;
    }

    // --- The fix: detach of a created-but-NEVER-ATTACHED segment must be denied.
    //     Pre-fix returned IPC_SUCCESS (0); fixed returns IPC_EINVAL (< 0). ---
    long id2 = sc3(SYS_SHMGET, IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    if (id2 > 0) {
        unsigned long va2 = SHM_VA_BASE + (unsigned long)id2 * SHM_VA_STRIDE;
        long rn = sc3(SYS_SHMDT, (long)va2, 0, 0);
        if (rn < 0) {
            out("NEGSHMDT: [2] PASS unattached detach denied\n");
        } else {
            out("NEGSHMDT: [2] FAIL unattached detach accepted (ownership gate missing)\n");
            ok = 0;
        }
    } else {
        out("NEGSHMDT: [2] FAIL shmget(victim) failed\n");
        ok = 0;
    }

    // --- Kernel still alive: the denied detach freed nothing and did not panic. ---
    long pid = sc0(SYS_GETPID);
    if (pid > 0) {
        out("NEGSHMDT: [3] PASS kernel_alive=1\n");
    } else {
        out("NEGSHMDT: [3] FAIL getpid broken after denied detach\n");
        ok = 0;
    }

    out(ok ? "NEGSHMDT: PASS\n" : "NEGSHMDT: FAIL\n");
    sc3(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {}
}
