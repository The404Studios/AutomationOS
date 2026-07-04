/*
 * negdir.c -- NEGDIR negative regression probe (freestanding, ring 3, own
 * _start -- no libs, no crt0). Proves the KERNEL-ROBUST-0 vfs_rename() fix:
 *
 *   (1) renaming a directory ONTO a non-empty destination directory must be
 *       REJECTED (VFS_ERR_INVAL / -22), not silently clobber (and leak) the
 *       destination's children.
 *   (2) renaming a directory INTO ITSELF (dest parent == the dir being moved)
 *       must be REJECTED, not build an orphaned self-referential cycle.
 *
 * On the PRE-FIX kernel, vfs_rename() has neither guard: both renames above
 * return 0 (success). On the FIXED kernel both return a negative errno. This
 * probe also does two follow-up ops after the rejected renames to prove the
 * kernel is still fully functional (no corruption / no wedge): the victim
 * file's content must still be intact, and a brand-new mkdir must succeed.
 *
 * Prints "NEGDIR: PASS" on success, "NEGDIR: FAIL <why>" otherwise. Gated
 * into the FULL=1 self-test boot storm alongside sockettest/sigtest.
 */

#define SYS_EXIT    0
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_OPEN    4
#define SYS_CLOSE   5
#define SYS_RENAME  35
#define SYS_MKDIR   67

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_CREAT     0x0040

static long sc6(long n, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5;
    asm volatile("syscall" : "=a"(ret)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return ret;
}
#define sc3(n,a,b,c) sc6((n),(a),(b),(c),0,0)
#define sc1(n,a)     sc6((n),(a),0,0,0,0)

static unsigned long slen(const char* s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_WRITE, 1, (long)s, (long)slen(s)); }

static int streq(const char* a, const char* b, unsigned long n) {
    for (unsigned long i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

void _start(void) {
    out("[NEGDIR] starting\n");

    /* --- Set up: /tmp/nd_a/f (non-empty dir) and /tmp/nd_b (empty dir) --- */
    long r = sc3(SYS_MKDIR, (long)"/tmp/nd_a", 0755, 0);
    if (r < 0) { out("NEGDIR: FAIL (mkdir /tmp/nd_a)\n"); sc1(SYS_EXIT, 1); }

    long fd = sc3(SYS_OPEN, (long)"/tmp/nd_a/f", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { out("NEGDIR: FAIL (open/create /tmp/nd_a/f)\n"); sc1(SYS_EXIT, 1); }

    long w = sc3(SYS_WRITE, fd, (long)"HELLO", 5);
    if (w != 5) { out("NEGDIR: FAIL (write /tmp/nd_a/f)\n"); sc1(SYS_EXIT, 1); }
    sc1(SYS_CLOSE, fd);

    r = sc3(SYS_MKDIR, (long)"/tmp/nd_b", 0755, 0);
    if (r < 0) { out("NEGDIR: FAIL (mkdir /tmp/nd_b)\n"); sc1(SYS_EXIT, 1); }

    out("[NEGDIR] setup ok: /tmp/nd_a/f (non-empty dir), /tmp/nd_b (empty dir)\n");

    /* --- Case 1: rename empty dir ONTO a non-empty dir must be rejected. ---
     * Pre-fix: vfs_rename() clobbers /tmp/nd_a (frees its dentry array WITHOUT
     * freeing its child "f", leaking the inode/dentry/buffer) and returns 0.
     * Fixed: returns VFS_ERR_INVAL (-22) and leaves both dirs untouched. */
    long r1 = sc3(SYS_RENAME, (long)"/tmp/nd_b", (long)"/tmp/nd_a", 0);
    if (r1 >= 0) {
        out("NEGDIR: FAIL (rename onto non-empty dir SUCCEEDED -- clobber/leak)\n");
        sc1(SYS_EXIT, 1);
    }
    out("[NEGDIR] [1] PASS non_empty_dest_rejected=1\n");

    /* --- Case 2: rename a dir INTO ITSELF (new parent == the dir being
     * moved) must be rejected. Pre-fix: dentry is unlinked from /tmp, renamed
     * to "sub", and re-inserted as a child of ITSELF -- orphaned self-cycle,
     * return 0. Fixed: returns VFS_ERR_INVAL (-22), /tmp/nd_a untouched. */
    long r2 = sc3(SYS_RENAME, (long)"/tmp/nd_a", (long)"/tmp/nd_a/sub", 0);
    if (r2 >= 0) {
        out("NEGDIR: FAIL (rename dir into itself SUCCEEDED -- self-cycle/orphan)\n");
        sc1(SYS_EXIT, 1);
    }
    out("[NEGDIR] [2] PASS self_cycle_rejected=1\n");

    /* --- Follow-up 1: the victim file must still be exactly intact (no
     * clobber actually happened under the covers despite the rejection). --- */
    long fd2 = sc3(SYS_OPEN, (long)"/tmp/nd_a/f", O_RDONLY, 0);
    if (fd2 < 0) { out("NEGDIR: FAIL (post-check: /tmp/nd_a/f missing)\n"); sc1(SYS_EXIT, 1); }
    char buf[8] = {0};
    long rd = sc3(SYS_READ, fd2, (long)buf, 5);
    sc1(SYS_CLOSE, fd2);
    if (rd != 5 || !streq(buf, "HELLO", 5)) {
        out("NEGDIR: FAIL (post-check: /tmp/nd_a/f content corrupted)\n");
        sc1(SYS_EXIT, 1);
    }
    out("[NEGDIR] [3] PASS victim_file_intact=1\n");

    /* --- Follow-up 2: the kernel/VFS must still work normally afterward
     * (no wedge/corruption from the rejected renames). --- */
    long r3 = sc3(SYS_MKDIR, (long)"/tmp/nd_followup", 0755, 0);
    if (r3 < 0) { out("NEGDIR: FAIL (post-check: mkdir after rejected renames failed)\n"); sc1(SYS_EXIT, 1); }
    out("[NEGDIR] [4] PASS kernel_alive_after=1\n");

    out("NEGDIR: PASS\n");
    sc1(SYS_EXIT, 0);
    for (;;) {}
}
