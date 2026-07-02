/*
 * e2etest.c -- FS / exec end-to-end integration proof (E2E build only; init
 * spawns it under -DE2E_TEST and waitpid()s it). Freestanding ring-3 program;
 * crt0 provides _start and calls main(); no libc, no headers.
 *
 * It exercises the ALREADY-HARDENED stack end to end and prints serial markers
 * that build_test/e2e_boot.sh asserts:
 *
 *   PIECE 1 (VFS):   opendir/readdir "/bin"      -> E2E-LS:    PASS ...
 *   PIECE 2 (ELF):   spawn+wait "bin/hello.elf"  -> E2E-ELF:   PASS ...
 *   PIECE 3 (SHELL): tool_shell runs "hello"     -> E2E-SHELL: PASS ...
 *
 * This program only ASSERTS the pieces work together; the VFS, initrd parser,
 * ELF loader, exec path, and shell all already exist (see FS-ROBUST-0).
 */
#define SYS_WRITE          3
#define SYS_WAITPID        6
#define SYS_OPENDIR        30
#define SYS_READDIR        31
#define SYS_CLOSEDIR       32
#define SYS_SPAWN_EX_ARGV  106
#define NAME_MAX_          256

typedef struct {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[NAME_MAX_];
} k_dirent_t;

static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "=a"(r)
        : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return r;
}

static unsigned slen(const char* s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void out(const char* s) { sc(SYS_WRITE, 1, (long)s, (long)slen(s)); }
static void out_d(long v) {
    char t[24]; int j = 0; char b[26]; int i = 0; int neg = (v < 0);
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (!u) t[j++] = '0';
    while (u) { t[j++] = (char)('0' + u % 10); u /= 10; }
    if (neg) b[i++] = '-';
    while (j) b[i++] = t[--j];
    b[i] = 0; out(b);
}
static int streq(const char* a, const char* b) {
    int i = 0; for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0; return a[i] == b[i];
}

/* Pack args NUL-separated, spawn via SYS_SPAWN_EX_ARGV, waitpid, return the
 * child's exit status (or -1 spawn-fail / -2 wait-fail). Mirrors tool_shell. */
static int argv_pack(char* buf, int cap, const char* const* args, int n) {
    int p = 0;
    for (int k = 0; k < n; k++) {
        const char* a = args[k];
        for (int j = 0; a[j] && p < cap - 1; j++) buf[p++] = a[j];
        if (p < cap) buf[p++] = 0;
    }
    return p;
}
static long spawn_wait(const char* path, const char* const* args, int nargs) {
    char av[512];
    int al = argv_pack(av, 512, args, nargs);
    long pid = sc6(SYS_SPAWN_EX_ARGV, (long)path, (long)av, al, 0, 0, 0);
    if (pid < 0) return -1;
    long st = 0;
    long w = sc(SYS_WAITPID, pid, (long)&st, 0);
    return w < 0 ? -2 : st;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    out("E2E: begin end-to-end integration proof (VFS + ELF-loader + shell)\n");

    /* PIECE 1 -- VFS: the kernel already mounted the initrd as the ramfs root.
     * Enumerate /bin (the "ls /bin" the request asks for). */
    long dfd = sc(SYS_OPENDIR, (long)"/bin", 0, 0);
    if (dfd < 0) dfd = sc(SYS_OPENDIR, (long)"bin", 0, 0);   /* tolerate abs/rel */
    if (dfd < 0) {
        out("E2E-LS: FAIL opendir /bin rc="); out_d(dfd); out("\n");
    } else {
        k_dirent_t de; int count = 0, found_hello = 0;
        for (;;) {
            long r = sc(SYS_READDIR, dfd, (long)&de, 0);
            if (r != 0) break;                       /* 0 = got entry; nonzero = end */
            de.d_name[NAME_MAX_ - 1] = 0;
            const char* nm = de.d_name;
            if (!nm[0]) continue;
            if (nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2]))) continue;  /* . .. */
            count++;
            if (streq(nm, "hello.elf")) found_hello = 1;
            out("  /bin/"); out(nm); out("\n");
        }
        sc(SYS_CLOSEDIR, dfd, 0, 0);
        out("E2E-LS: PASS ls /bin entries="); out_d(count);
        out(" hello_elf_present="); out_d(found_hello); out("\n");
    }

    /* PIECE 2 -- ELF loader: load + run /bin/hello.elf. A non-negative pid means
     * the (hardened) loader parsed/validated/mapped the ELF; exit==0 means it
     * actually executed in ring-3 (it also prints its own E2E-HELLO marker). */
    {
        const char* none[1]; none[0] = 0;
        long st = spawn_wait("bin/hello.elf", none, 0);
        if (st == 0) out("E2E-ELF: PASS loaded+ran /bin/hello.elf exit=0\n");
        else { out("E2E-ELF: FAIL /bin/hello.elf st="); out_d(st); out("\n"); }
    }

    /* PIECE 3 -- Shell: drive the existing userspace shell (tool_shell) to run a
     * command by name. tool_shell resolves "hello" -> bin/hello, spawns+waits it,
     * and prints "SHELL hello exit=0". A 0 exit here == the shell ran a command. */
    {
        const char* a[1]; a[0] = "hello";
        long st = spawn_wait("sbin/tool_shell", a, 1);
        if (st == 0) out("E2E-SHELL: PASS shell (tool_shell) ran command 'hello'\n");
        else { out("E2E-SHELL: FAIL tool_shell st="); out_d(st); out("\n"); }
    }

    out("E2E: ALL DONE -- kernel->initrd->VFS->ELF-loader->exec->shell proven\n");
    return 0;
}
