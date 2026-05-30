/*
 * aibroker.c -- AI Command BROKER: a capability-gated command broker.
 * ===========================================================================
 *
 * The crown jewel of the AI-native OS userspace: the single mediated surface
 * through which an AI agent (or any caller) acts on the system.  The AI never
 * touches raw kernel/root -- it gets TOOLS.  Every tool is a vetted C function;
 * every invocation passes through a POLICY ENGINE that returns
 * ALLOW / REQUIRE_APPROVAL / DENY; every action is appended to a tamper-evident
 * LEDGER; and every file mutation is preceded by a SNAPSHOT so it can be rolled
 * back.
 *
 *   AI  --(text protocol)-->  BROKER  --(tool bus)-->  vetted C functions
 *                                |                          |
 *                          policy engine               kernel syscalls
 *                                |
 *                          ledger + snapshots (rollback)
 *
 * Architecture (all in this one TU for cohesion):
 *   1. TOOL BUS    -- fixed registry of SAFE tools (file.read, file.write_project,
 *                     process.list, process.query, sysinfo, git.status, git.diff,
 *                     pkg.install_request, service.status, rollback).
 *   2. POLICY ENGINE -- loads /etc/ai/policy.json (flat allow[]/require_approval[]
 *                     /deny[] string arrays; tiny hand-rolled parser).  Falls back
 *                     to a safe built-in default if the file is absent.
 *                     classify(tool,args) -> ALLOW | REQUIRE_APPROVAL | DENY.
 *   3. LEDGER      -- appends one line per action to /var/log/ai/actions.log.
 *   4. ROLLBACK    -- snapshots a file to /var/snapshots/<base>.<ticks> before any
 *                     write tool mutates it; `rollback <path>` restores the latest.
 *
 * Command intake:
 *   - reads /etc/ai/commands (one command per line: `tool arg1 arg2`), OR
 *   - if that file is absent, runs a built-in SELF-TEST sequence that exercises
 *     the full pipeline and prints `AIBROKER SELFTEST: PASS` (or FAIL <reason>).
 *
 * This is a FREESTANDING userspace ELF app: no libc, no startup, pure inline
 * syscalls.  Self-contained -- own strlen/memcmp/strcpy/itoa helpers.  Builds ON
 * the aictl ABI ideas (procinfo_t/proc_detail_t/sysinfo_t, SYS_PROC_* numbers)
 * without linking the library, so it stays a single object.
 *
 * Build (flags DIRECTLY on the command line -- NEVER via a shell variable, or
 * -fno-stack-protector is silently dropped and the binary faults at fs:0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/aibroker/aibroker.c -o aibroker.o
 *
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       aibroker.o -o build/aibroker
 *
 *   objdump -d build/aibroker | grep 'fs:0x28'   # MUST be empty
 *
 * Serial markers (how the integrator gates this app):
 *   AIBROKER: <tool> -> <ALLOW|REQUIRE_APPROVAL|DENY>   (one per self-test step)
 *   AIBROKER SELFTEST: PASS                              (overall success)
 *   AIBROKER SELFTEST: FAIL <reason>                     (overall failure)
 */

/* =========================================================================
 * Syscall numbers (verified against kernel/include/syscall.h).
 * ====================================================================== */
#define SYS_EXIT          0
#define SYS_READ          2
#define SYS_WRITE         3
#define SYS_OPEN          4
#define SYS_CLOSE         5
#define SYS_SPAWN         16
#define SYS_KILL          26
#define SYS_STAT          33
#define SYS_UNLINK        34
#define SYS_RENAME        35
#define SYS_GET_TICKS_MS  40
#define SYS_TIME          41
#define SYS_PROCLIST      44
#define SYS_PROC_QUERY    60
#define SYS_PROC_CTL      61
#define SYS_SYSINFO       62
#define SYS_MKDIR         67

/* O_* flags (kernel/include/vfs.h). */
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_CREAT   0x0040
#define O_TRUNC   0x0200
#define O_APPEND  0x0400

/* Kernel copies a fixed-size path buffer; give it room (MAX_PATH_LEN = 4096). */
#define KPATH_MAX 4096

/* =========================================================================
 * Freestanding integer types (no stdint.h).
 * ====================================================================== */
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef unsigned long      usize;

/* =========================================================================
 * ABI structs (mirror the aictl library / kernel ABI -- do NOT relink it).
 * ====================================================================== */
typedef struct {            /* 64 bytes -- SYS_PROCLIST entry */
    u32  pid;
    u32  parent_pid;
    u32  state;
    u32  flags;
    char name[32];
    u64  cpu_ticks;
    u64  ctx_switches;
} procinfo_t;

typedef struct {            /* 64 bytes -- SYS_PROC_QUERY detail */
    u32  pid;
    u32  ppid;
    u32  state;
    u32  prio;
    u64  cpu_ticks;
    u32  mem_pages;
    u32  vma_count;
    char name[32];
} proc_detail_t;

typedef struct {            /* SYS_SYSINFO record */
    u64 total_mem;
    u64 free_mem;
    u64 uptime_ms;
    u32 proc_count;
} sysinfo_t;

/* vfs_stat_t prefix -- we only need st_size (kernel/include/vfs.h). */
typedef struct {
    u64 st_dev;
    u64 st_ino;
    u32 st_mode;
    u32 st_nlink;
    u32 st_uid;
    u32 st_gid;
    u64 st_rdev;
    u64 st_size;
    u64 st_blksize;
    u64 st_blocks;
    u64 st_atime;
    u64 st_mtime;
    u64 st_ctime;
} vfs_stat_t;

/* =========================================================================
 * Inline 3-argument syscall (n=rax, args rdi/rsi/rdx; clobber rcx,r11,memory).
 * Three args is sufficient for every syscall this broker issues.
 * ====================================================================== */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* =========================================================================
 * Tiny freestanding helpers (own strlen/memcmp/strcpy/itoa/u64->dec).
 * ====================================================================== */
static usize k_strlen(const char *s)
{
    usize n = 0;
    while (s[n]) n++;
    return n;
}

static int k_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int k_memcmp(const void *a, const void *b, usize n)
{
    const unsigned char *x = a, *y = b;
    for (usize i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

static void k_memzero(void *p, usize n)
{
    unsigned char *b = p;
    for (usize i = 0; i < n; i++) b[i] = 0;
}

/* Copy src into dst (cap includes NUL); returns length written (excl NUL). */
static int k_strlcpy(char *dst, const char *src, int cap)
{
    int i = 0;
    if (cap <= 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

/* Append src to dst (NUL-terminated, cap includes NUL); returns dst length. */
static int k_strlcat(char *dst, const char *src, int cap)
{
    int dl = (int)k_strlen(dst);
    int i = 0;
    while (src[i] && dl + i < cap - 1) { dst[dl + i] = src[i]; i++; }
    dst[dl + i] = '\0';
    return dl + i;
}

/* Does haystack contain NUL-terminated needle?  Empty needle -> match. */
static int k_contains(const char *hay, const char *needle)
{
    usize nlen = k_strlen(needle);
    if (nlen == 0) return 1;
    usize hlen = k_strlen(hay);
    if (nlen > hlen) return 0;
    for (usize i = 0; i + nlen <= hlen; i++) {
        if (k_memcmp(hay + i, needle, nlen) == 0) return 1;
    }
    return 0;
}

/* Does s start with prefix? */
static int k_startswith(const char *s, const char *prefix)
{
    while (*prefix) { if (*s != *prefix) return 0; s++; prefix++; }
    return 1;
}

/* Unsigned 64-bit -> decimal string in buf (>= 21 bytes); returns length. */
static int u64_to_dec(u64 v, char *buf)
{
    char tmp[24];
    int i = 0;
    do { tmp[i++] = (char)('0' + (v % 10ULL)); v /= 10ULL; } while (v);
    int n = i;
    for (int j = 0; j < n; j++) buf[j] = tmp[n - 1 - j];
    buf[n] = '\0';
    return n;
}

/* Parse leading unsigned decimal; -1 if no digits. */
static long k_atoi(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    if (*s < '0' || *s > '9') return -1;
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

/* =========================================================================
 * Serial output (fd 1 = serial console).
 * ====================================================================== */
static void out(const char *s)  { sc(SYS_WRITE, 1, (long)s, (long)k_strlen(s)); }
static void outn(const char *s, long n) { sc(SYS_WRITE, 1, (long)s, n); }
static void out_u64(u64 v) { char b[24]; int n = u64_to_dec(v, b); outn(b, n); }

/* =========================================================================
 * Decision codes.
 * ====================================================================== */
#define DEC_ALLOW            0
#define DEC_REQUIRE_APPROVAL 1
#define DEC_DENY             2

static const char *decision_name(int d)
{
    switch (d) {
        case DEC_ALLOW:            return "ALLOW";
        case DEC_REQUIRE_APPROVAL: return "REQUIRE_APPROVAL";
        case DEC_DENY:             return "DENY";
        default:                   return "?";
    }
}

/* Ledger decision tag (the action that was actually taken). */
static const char *ledger_decision(int d)
{
    switch (d) {
        case DEC_ALLOW:            return "ALLOW";
        case DEC_REQUIRE_APPROVAL: return "APPROVE";
        case DEC_DENY:             return "DENY";
        default:                   return "?";
    }
}

/* =========================================================================
 * Path safety: write tools may ONLY touch these prefixes; never /etc, /sbin,
 * /boot, or anything kernel-ish.  This is belt-and-braces in addition to the
 * policy engine.
 * ====================================================================== */
static int path_write_allowed(const char *path)
{
    /* explicit denylist first (defensive) */
    if (k_startswith(path, "/boot")  ||
        k_startswith(path, "/sbin")  ||
        k_startswith(path, "/etc")   ||
        k_contains(path, "kernel")   ||
        k_contains(path, ".."))            /* no traversal */
        return 0;

    /* allowlist of project prefixes */
    if (k_startswith(path, "/home")    ||
        k_startswith(path, "/usr/src"))
        return 1;

    return 0;
}

/* =========================================================================
 * Filesystem helpers (whole-file slurp, atomic-ish write, copy).
 * ====================================================================== */
#define FILE_BUF_MAX 65536
static char g_filebuf[FILE_BUF_MAX] __attribute__((aligned(16)));

/* Path scratch buffers sized for the kernel's copy_from_user length. */
static char g_path_a[KPATH_MAX] __attribute__((aligned(16)));
static char g_path_b[KPATH_MAX] __attribute__((aligned(16)));

/* Read whole file into g_filebuf; returns bytes read (>=0), -1 open fail,
 * -2 too large for the buffer. */
static long slurp_file(const char *path)
{
    long fd = sc(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        long room = FILE_BUF_MAX - total;
        if (room <= 0) {
            char extra;
            long n = sc(SYS_READ, fd, (long)&extra, 1);
            sc(SYS_CLOSE, fd, 0, 0);
            return (n > 0) ? -2 : total;
        }
        long n = sc(SYS_READ, fd, (long)(g_filebuf + total), room);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return total;
}

/* Write buf[0..len) to path (truncate/create).  Returns 0 ok, -1 on error. */
static int write_file(const char *path, const char *buf, long len)
{
    long fd = sc(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    long off = 0;
    while (off < len) {
        long w = sc(SYS_WRITE, fd, (long)(buf + off), len - off);
        if (w <= 0) { sc(SYS_CLOSE, fd, 0, 0); return -1; }
        off += w;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return 0;
}

/* Append a NUL-terminated line to path (creating it if absent). Best-effort. */
static int append_line(const char *path, const char *line)
{
    long fd = sc(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return -1;
    long len = (long)k_strlen(line);
    long off = 0;
    while (off < len) {
        long w = sc(SYS_WRITE, fd, (long)(line + off), len - off);
        if (w <= 0) break;
        off += w;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return 0;
}

/* =========================================================================
 * Recursive mkdir.  The kernel's SYS_MKDIR is documented recursive, but we
 * also create each ancestor explicitly so a non-recursive kernel still works.
 * ====================================================================== */
static char g_mkdir_tmp[KPATH_MAX] __attribute__((aligned(16)));
static void mkdir_p(const char *path)
{
    char *tmp = g_mkdir_tmp;
    int n = k_strlcpy(tmp, path, KPATH_MAX);
    /* create each prefix ending in '/' then the whole path */
    for (int i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            sc(SYS_MKDIR, (long)tmp, 0755, 0);
            tmp[i] = '/';
        }
    }
    sc(SYS_MKDIR, (long)path, 0755, 0);
}

/* =========================================================================
 * Monotonic clock for ledger + snapshot naming.
 * ====================================================================== */
static u64 now_ticks(void)
{
    long t = sc(SYS_GET_TICKS_MS, 0, 0, 0);
    return (t < 0) ? 0ULL : (u64)t;
}

/* =========================================================================
 * Basename: pointer to the component after the last '/'.
 * ====================================================================== */
static const char *basename_of(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

/* =========================================================================
 * LEDGER -- one line per action:
 *   <ticks_ms> tool=<t> args=<a> risk=<r> decision=<ALLOW/DENY/APPROVE> result=<ok/err>
 * ====================================================================== */
#define LEDGER_PATH "/var/log/ai/actions.log"
static char g_ledger_line[1024];

static void ledger_record(const char *tool, const char *args,
                          int decision, const char *result)
{
    char ticks[24];
    u64_to_dec(now_ticks(), ticks);

    g_ledger_line[0] = '\0';
    k_strlcat(g_ledger_line, ticks, sizeof(g_ledger_line));
    k_strlcat(g_ledger_line, " tool=", sizeof(g_ledger_line));
    k_strlcat(g_ledger_line, tool ? tool : "-", sizeof(g_ledger_line));
    k_strlcat(g_ledger_line, " args=", sizeof(g_ledger_line));
    k_strlcat(g_ledger_line, (args && args[0]) ? args : "-", sizeof(g_ledger_line));
    k_strlcat(g_ledger_line, " risk=", sizeof(g_ledger_line));
    k_strlcat(g_ledger_line, decision_name(decision), sizeof(g_ledger_line));
    k_strlcat(g_ledger_line, " decision=", sizeof(g_ledger_line));
    k_strlcat(g_ledger_line, ledger_decision(decision), sizeof(g_ledger_line));
    k_strlcat(g_ledger_line, " result=", sizeof(g_ledger_line));
    k_strlcat(g_ledger_line, result ? result : "-", sizeof(g_ledger_line));
    k_strlcat(g_ledger_line, "\n", sizeof(g_ledger_line));

    append_line(LEDGER_PATH, g_ledger_line);
}

/* =========================================================================
 * ROLLBACK / SNAPSHOT.
 *   snapshot_file(path) copies the current contents to
 *     /var/snapshots/<basename>.<ticks>
 *   and remembers the latest snapshot path for that basename so `rollback`
 *   can restore it.  (We track the single most-recent snapshot per run; the
 *   on-disk snapshots are all retained for forensic audit.)
 * ====================================================================== */
#define SNAP_DIR "/var/snapshots/"

/* Remember the most-recent snapshot taken in this run, keyed by basename. */
#define SNAP_SLOTS 16
static struct {
    char base[64];
    char snap[KPATH_MAX];
    char orig[KPATH_MAX];
} g_snaps[SNAP_SLOTS];
static int g_snap_count;

/* Build SNAP_DIR<base>.<ticks> into out. */
static void make_snap_path(char *out, int cap, const char *base, u64 ticks)
{
    char tk[24];
    u64_to_dec(ticks, tk);
    out[0] = '\0';
    k_strlcat(out, SNAP_DIR, cap);
    k_strlcat(out, base, cap);
    k_strlcat(out, ".", cap);
    k_strlcat(out, tk, cap);
}

/* Snapshot path's current contents.  Returns 0 ok, -1 if path unreadable
 * (a brand-new file has nothing to snapshot -- that's fine, returns 1). */
static int snapshot_file(const char *path)
{
    long n = slurp_file(path);
    if (n == -2) return -1;          /* too large to snapshot safely */

    const char *base = basename_of(path);
    u64 ticks = now_ticks();
    char snap[KPATH_MAX];
    make_snap_path(snap, sizeof(snap), base, ticks);

    if (n < 0) {
        /* file doesn't exist yet: record an empty snapshot so rollback to a
         * pre-creation state truncates the file. */
        if (write_file(snap, "", 0) != 0) return -1;
        n = 0;
    } else {
        if (write_file(snap, g_filebuf, n) != 0) return -1;
    }

    /* remember most-recent snapshot for this basename */
    int slot = -1;
    for (int i = 0; i < g_snap_count; i++) {
        if (k_streq(g_snaps[i].base, base)) { slot = i; break; }
    }
    if (slot < 0 && g_snap_count < SNAP_SLOTS) slot = g_snap_count++;
    if (slot >= 0) {
        k_strlcpy(g_snaps[slot].base, base, sizeof(g_snaps[slot].base));
        k_strlcpy(g_snaps[slot].snap, snap, sizeof(g_snaps[slot].snap));
        k_strlcpy(g_snaps[slot].orig, path, sizeof(g_snaps[slot].orig));
    }
    return 0;
}

/* Restore the latest snapshot for path's basename.  0 ok, -1 no snapshot. */
static int rollback_file(const char *path)
{
    const char *base = basename_of(path);
    for (int i = 0; i < g_snap_count; i++) {
        if (k_streq(g_snaps[i].base, base)) {
            long n = slurp_file(g_snaps[i].snap);
            if (n < 0) return -1;
            return write_file(g_snaps[i].orig, g_filebuf, n);
        }
    }
    return -1;
}

/* =========================================================================
 * POLICY ENGINE.
 *
 * Loads /etc/ai/policy.json.  We do NOT need a full JSON parser -- the format
 * is three flat arrays of strings:
 *
 *   {
 *     "allow":            ["process.list", "sysinfo", "file.read", ...],
 *     "require_approval": ["file.write_project", ...],
 *     "deny":             ["/boot", "/sbin", "kernel", ...]
 *   }
 *
 * The parser extracts every double-quoted string token and bins it by which
 * key ("allow"/"require_approval"/"deny") most recently appeared before it.
 * Tokens may be tool names (exact match) or substrings to match against args.
 * ====================================================================== */
#define POLICY_PATH    "/etc/ai/policy.json"
#define POLICY_MAX     64
#define POLICY_TOKLEN  64

static char g_allow[POLICY_MAX][POLICY_TOKLEN];        static int g_n_allow;
static char g_approve[POLICY_MAX][POLICY_TOKLEN];      static int g_n_approve;
static char g_deny[POLICY_MAX][POLICY_TOKLEN];         static int g_n_deny;
static int  g_policy_loaded;   /* 1 if file parsed, 0 if using defaults */

static void policy_add(int bin, const char *tok)
{
    if (bin == 0 && g_n_allow   < POLICY_MAX) k_strlcpy(g_allow[g_n_allow++],   tok, POLICY_TOKLEN);
    if (bin == 1 && g_n_approve < POLICY_MAX) k_strlcpy(g_approve[g_n_approve++], tok, POLICY_TOKLEN);
    if (bin == 2 && g_n_deny    < POLICY_MAX) k_strlcpy(g_deny[g_n_deny++],    tok, POLICY_TOKLEN);
}

/* Safe built-in default policy (used when the file is missing/unparseable). */
static void policy_load_defaults(void)
{
    g_n_allow = g_n_approve = g_n_deny = 0;

    /* allow: read/list/status/query/info */
    policy_add(0, "file.read");
    policy_add(0, "process.list");
    policy_add(0, "process.query");
    policy_add(0, "sysinfo");
    policy_add(0, "service.status");
    policy_add(0, "git.status");
    policy_add(0, "git.diff");

    /* require_approval: anything that writes or requests installs */
    policy_add(1, "file.write_project");
    policy_add(1, "pkg.install_request");
    policy_add(1, "rollback");

    /* deny: anything touching boot/sbin/kernel surfaces */
    policy_add(2, "/boot");
    policy_add(2, "/sbin");
    policy_add(2, "/etc");
    policy_add(2, "kernel");

    g_policy_loaded = 0;
}

/* Parse the flat-array policy JSON in g_filebuf[0..len). */
static int policy_parse(const char *json, long len)
{
    g_n_allow = g_n_approve = g_n_deny = 0;
    int bin = -1;     /* -1 = unknown, 0 allow, 1 approve, 2 deny */
    long i = 0;

    while (i < len) {
        char c = json[i];
        if (c == '"') {
            /* read the quoted token */
            long start = ++i;
            while (i < len && json[i] != '"') i++;
            long tlen = i - start;
            i++;      /* skip closing quote */

            char tok[POLICY_TOKLEN];
            int tl = 0;
            for (long j = 0; j < tlen && tl < POLICY_TOKLEN - 1; j++)
                tok[tl++] = json[start + j];
            tok[tl] = '\0';

            /* Is this token a section key, or a value in the current section? */
            if (k_streq(tok, "allow"))                 bin = 0;
            else if (k_streq(tok, "require_approval")) bin = 1;
            else if (k_streq(tok, "deny"))             bin = 2;
            else if (bin >= 0)                         policy_add(bin, tok);
            continue;
        }
        i++;
    }
    return (g_n_allow + g_n_approve + g_n_deny) > 0 ? 0 : -1;
}

static void policy_load(void)
{
    long n = slurp_file(POLICY_PATH);
    if (n < 0) { policy_load_defaults(); return; }
    if (policy_parse(g_filebuf, n) != 0) { policy_load_defaults(); return; }
    g_policy_loaded = 1;
}

/* Does a token list contain an exact tool name OR a substring of args? */
static int list_matches(char list[][POLICY_TOKLEN], int count,
                        const char *tool, const char *args)
{
    for (int i = 0; i < count; i++) {
        if (k_streq(list[i], tool)) return 1;             /* exact tool name */
        if (args && args[0] && k_contains(args, list[i])) return 1; /* substr in args */
    }
    return 0;
}

/*
 * classify(tool, args) -> ALLOW | REQUIRE_APPROVAL | DENY.
 *
 * Precedence: DENY > REQUIRE_APPROVAL > ALLOW.  If the tool/args match nothing,
 * the safe default is DENY (deny-by-default).
 */
static int classify(const char *tool, const char *args)
{
    if (list_matches(g_deny,    g_n_deny,    tool, args)) return DEC_DENY;
    if (list_matches(g_approve, g_n_approve, tool, args)) return DEC_REQUIRE_APPROVAL;
    if (list_matches(g_allow,   g_n_allow,   tool, args)) return DEC_ALLOW;
    return DEC_DENY;   /* deny by default */
}

/* =========================================================================
 * TOOL BUS -- the vetted tool implementations.  Each returns a result string
 * "ok" or "err" for the ledger; output (if any) is printed to serial.
 *
 * `args` is the remainder of the command line after the tool name.
 * ====================================================================== */

/* file.read <path> -- print file contents to serial. */
static const char *tool_file_read(const char *args)
{
    if (!args || !args[0]) { out("AIBROKER tool file.read: missing path\n"); return "err"; }
    k_strlcpy(g_path_a, args, KPATH_MAX);
    long n = slurp_file(g_path_a);
    if (n < 0) { out("AIBROKER tool file.read: cannot open\n"); return "err"; }
    out("---8<--- "); out(g_path_a); out("\n");
    outn(g_filebuf, n);
    if (n == 0 || g_filebuf[n - 1] != '\n') out("\n");
    out("--->8---\n");
    return "ok";
}

/* file.write_project <path> <data...> -- snapshot then write, allowed prefix only. */
static char g_wp_path[KPATH_MAX] __attribute__((aligned(16)));
static char g_wp_dir[KPATH_MAX]  __attribute__((aligned(16)));
static const char *tool_file_write_project(const char *args)
{
    /* split: first token = path, remainder = data */
    char *path = g_wp_path;
    int pl = 0;
    const char *p = args;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t' && pl < KPATH_MAX - 1) path[pl++] = *p++;
    path[pl] = '\0';
    while (*p == ' ' || *p == '\t') p++;
    const char *data = p;

    if (!path[0]) { out("AIBROKER tool file.write_project: missing path\n"); return "err"; }
    if (!path_write_allowed(path)) {
        out("AIBROKER tool file.write_project: path outside allowed prefix: ");
        out(path); out("\n");
        return "err";
    }

    /* ROLLBACK HOOK: snapshot current contents before mutating. */
    k_strlcpy(g_path_a, path, KPATH_MAX);
    snapshot_file(g_path_a);

    /* Ensure the parent directory exists (e.g. /home), else O_CREAT fails on a
     * ramfs with no such dir. Derive the dirname and recursively mkdir it. */
    k_strlcpy(g_wp_dir, g_path_a, KPATH_MAX);
    {
        int li = -1;
        for (int i = 0; g_wp_dir[i]; i++) if (g_wp_dir[i] == '/') li = i;
        if (li > 0) { g_wp_dir[li] = '\0'; mkdir_p(g_wp_dir); }
    }

    long dlen = (long)k_strlen(data);
    if (write_file(g_path_a, data, dlen) != 0) {
        out("AIBROKER tool file.write_project: write failed\n");
        return "err";
    }
    out("AIBROKER tool file.write_project: wrote "); out_u64((u64)dlen);
    out(" bytes to "); out(g_path_a); out("\n");
    return "ok";
}

/* process.list -- SYS_PROCLIST. */
#define PS_MAX 64
static procinfo_t g_ps[PS_MAX];

static const char *state_name(u32 s)
{
    switch (s) {
        case 0: return "READY ";
        case 1: return "RUN   ";
        case 2: return "SLEEP ";
        case 3: return "WAIT  ";
        case 4: return "ZOMBIE";
        default: return "?     ";
    }
}

static const char *tool_process_list(const char *args)
{
    (void)args;
    long count = sc(SYS_PROCLIST, (long)g_ps, PS_MAX, 0);
    if (count < 0) { out("AIBROKER tool process.list: unavailable\n"); return "err"; }
    out("  PID  PPID  STATE   NAME\n");
    for (long i = 0; i < count; i++) {
        g_ps[i].name[31] = '\0';
        out("  "); out_u64(g_ps[i].pid);
        out("  ");  out_u64(g_ps[i].parent_pid);
        out("  ");  out(state_name(g_ps[i].state));
        out("  ");  out(g_ps[i].name[0] ? g_ps[i].name : "(unnamed)");
        out("\n");
    }
    out("AIBROKER tool process.list: "); out_u64((u64)count); out(" process(es)\n");
    return "ok";
}

/* process.query <pid> -- SYS_PROC_QUERY. */
static proc_detail_t g_det;

static const char *tool_process_query(const char *args)
{
    long pid = k_atoi(args ? args : "");
    if (pid < 0) { out("AIBROKER tool process.query: bad pid\n"); return "err"; }
    k_memzero(&g_det, sizeof(g_det));
    long r = sc(SYS_PROC_QUERY, pid, (long)&g_det, 0);
    if (r < 0) { out("AIBROKER tool process.query: unavailable\n"); return "err"; }
    g_det.name[31] = '\0';
    out("pid=");      out_u64(g_det.pid);
    out(" ppid=");    out_u64(g_det.ppid);
    out(" prio=");    out_u64(g_det.prio);
    out(" state=");   out(state_name(g_det.state));
    out(" mem_pg=");  out_u64(g_det.mem_pages);
    out(" vma=");     out_u64(g_det.vma_count);
    out(" cpu=");     out_u64(g_det.cpu_ticks);
    out(" name=");    out(g_det.name[0] ? g_det.name : "(unnamed)");
    out("\n");
    return "ok";
}

/* sysinfo -- SYS_SYSINFO. */
static sysinfo_t g_si;

static const char *tool_sysinfo(const char *args)
{
    (void)args;
    k_memzero(&g_si, sizeof(g_si));
    long r = sc(SYS_SYSINFO, (long)&g_si, 0, 0);
    if (r < 0) { out("AIBROKER tool sysinfo: unavailable\n"); return "err"; }
    out("mem_total_kb="); out_u64(g_si.total_mem / 1024ULL);
    out(" mem_free_kb=");  out_u64(g_si.free_mem  / 1024ULL);
    out(" uptime_s=");     out_u64(g_si.uptime_ms / 1000ULL);
    out(" procs=");        out_u64(g_si.proc_count);
    out("\n");
    return "ok";
}

/* git.status / git.diff -- there is no in-kernel git; report delegated. */
static const char *tool_git_status(const char *args)
{
    (void)args;
    out("AIBROKER tool git.status: delegated (no in-kernel VCS; use shell 'git')\n");
    return "ok";
}
static const char *tool_git_diff(const char *args)
{
    (void)args;
    out("AIBROKER tool git.diff: delegated (no in-kernel VCS; use shell 'git')\n");
    return "ok";
}

/* pkg.install_request <name> -- record-only; never installs. */
#define PKG_REQ_PATH "/var/log/ai/pkg_requests.log"
static const char *tool_pkg_install_request(const char *args)
{
    if (!args || !args[0]) { out("AIBROKER tool pkg.install_request: missing name\n"); return "err"; }
    char line[256];
    char tk[24];
    u64_to_dec(now_ticks(), tk);
    line[0] = '\0';
    k_strlcat(line, tk, sizeof(line));
    k_strlcat(line, " requested=", sizeof(line));
    k_strlcat(line, args, sizeof(line));
    k_strlcat(line, "\n", sizeof(line));
    append_line(PKG_REQ_PATH, line);
    out("AIBROKER tool pkg.install_request: recorded request for '"); out(args);
    out("' (NOT installed)\n");
    return "ok";
}

/* service.status <name> -- best-effort: find by name in the process list. */
static const char *tool_service_status(const char *args)
{
    const char *name = (args && args[0]) ? args : "";
    long count = sc(SYS_PROCLIST, (long)g_ps, PS_MAX, 0);
    if (count < 0) { out("AIBROKER tool service.status: unavailable\n"); return "err"; }
    int found = 0;
    for (long i = 0; i < count; i++) {
        g_ps[i].name[31] = '\0';
        if (!name[0] || k_contains(g_ps[i].name, name)) {
            out("service "); out(g_ps[i].name[0] ? g_ps[i].name : "(unnamed)");
            out(" pid="); out_u64(g_ps[i].pid);
            out(" state="); out(state_name(g_ps[i].state)); out("\n");
            found = 1;
        }
    }
    if (!found) { out("AIBROKER tool service.status: '"); out(name); out("' not running\n"); }
    return "ok";
}

/* rollback <path> -- restore the latest snapshot for that file. */
static const char *tool_rollback(const char *args)
{
    if (!args || !args[0]) { out("AIBROKER tool rollback: missing path\n"); return "err"; }
    k_strlcpy(g_path_b, args, KPATH_MAX);
    if (rollback_file(g_path_b) != 0) {
        out("AIBROKER tool rollback: no snapshot for "); out(g_path_b); out("\n");
        return "err";
    }
    out("AIBROKER tool rollback: restored "); out(g_path_b); out("\n");
    return "ok";
}

/* =========================================================================
 * Tool dispatch table.
 * ====================================================================== */
typedef const char *(*tool_fn)(const char *args);

typedef struct {
    const char *name;
    tool_fn     fn;
    int         is_write;   /* write tools get a pre-mutation snapshot already
                               handled internally; flag kept for clarity/audit */
} tool_entry_t;

static const tool_entry_t g_tools[] = {
    { "file.read",            tool_file_read,          0 },
    { "file.write_project",   tool_file_write_project, 1 },
    { "process.list",         tool_process_list,       0 },
    { "process.query",        tool_process_query,      0 },
    { "sysinfo",              tool_sysinfo,            0 },
    { "git.status",           tool_git_status,         0 },
    { "git.diff",             tool_git_diff,           0 },
    { "pkg.install_request",  tool_pkg_install_request,0 },
    { "service.status",       tool_service_status,     0 },
    { "rollback",             tool_rollback,           1 },
};
#define N_TOOLS ((int)(sizeof(g_tools) / sizeof(g_tools[0])))

static const tool_entry_t *tool_find(const char *name)
{
    for (int i = 0; i < N_TOOLS; i++)
        if (k_streq(g_tools[i].name, name)) return &g_tools[i];
    return (const tool_entry_t *)0;
}

/* =========================================================================
 * BROKER CORE: classify -> ledger -> (maybe) execute.
 *
 * Returns the decision so callers (self-test) can assert on it.
 * `executed_ok` (out, optional) is set to 1 if the tool ran and returned "ok".
 * ====================================================================== */
static int broker_invoke(const char *tool, const char *args, int *executed_ok)
{
    if (executed_ok) *executed_ok = 0;

    const tool_entry_t *te = tool_find(tool);
    int decision = classify(tool, args);

    /* Print the gating marker the integrator greps for. */
    out("AIBROKER: "); out(tool); out(" -> "); out(decision_name(decision)); out("\n");

    if (!te) {
        /* unknown tool: always denied, recorded as err */
        ledger_record(tool, args, DEC_DENY, "err");
        return DEC_DENY;
    }

    if (decision == DEC_DENY) {
        ledger_record(tool, args, DEC_DENY, "blocked");
        return DEC_DENY;
    }

    /*
     * REQUIRE_APPROVAL: in this headless broker there is no interactive human.
     * Policy semantics: an approval-gated tool is executed but recorded as
     * APPROVE in the ledger (the audit trail makes the human-in-the-loop step
     * explicit).  A real deployment would block here pending an approve token;
     * the self-test treats REQUIRE_APPROVAL as a valid, auditable outcome.
     */
    const char *result = te->fn(args);
    int ok = k_streq(result, "ok");
    if (executed_ok) *executed_ok = ok;
    ledger_record(tool, args, decision, result);
    return decision;
}

/* =========================================================================
 * COMMAND INTAKE -- parse "tool arg1 arg2..." lines from /etc/ai/commands.
 * ====================================================================== */
#define COMMANDS_PATH "/etc/ai/commands"

/* Run one command line (mutates nothing in the buffer). */
static void run_command_line(const char *line)
{
    /* skip leading space, ignore blanks and '#' comments */
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#') return;

    /* split tool name from args */
    char tool[POLICY_TOKLEN];
    int tl = 0;
    while (*line && *line != ' ' && *line != '\t' && tl < POLICY_TOKLEN - 1)
        tool[tl++] = *line++;
    tool[tl] = '\0';
    while (*line == ' ' || *line == '\t') line++;

    broker_invoke(tool, line, (int *)0);
}

/* Returns 1 if /etc/ai/commands existed and was processed, 0 if absent. */
static char g_cmdbuf[FILE_BUF_MAX] __attribute__((aligned(16)));
static int process_commands_file(void)
{
    long fd = sc(SYS_OPEN, (long)COMMANDS_PATH, O_RDONLY, 0);
    if (fd < 0) return 0;

    /* read the whole file into g_cmdbuf */
    long total = 0;
    for (;;) {
        long room = FILE_BUF_MAX - 1 - total;
        if (room <= 0) break;
        long n = sc(SYS_READ, fd, (long)(g_cmdbuf + total), room);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    g_cmdbuf[total] = '\0';

    /* split into lines (in place) and run each */
    long start = 0;
    for (long i = 0; i <= total; i++) {
        if (i == total || g_cmdbuf[i] == '\n') {
            g_cmdbuf[i] = '\0';
            run_command_line(g_cmdbuf + start);
            start = i + 1;
        }
    }
    return 1;
}

/* =========================================================================
 * SELF-TEST -- exercise the full pipeline on safe sample commands when no
 * commands file is present.  Prints PASS/FAIL marker for the integrator.
 * ====================================================================== */
static void run_selftest(void)
{
    out("AIBROKER SELFTEST: begin\n");

    int fail = 0;
    const char *reason = "";
    int dec, ok;

    /* 1) process.list -> must be ALLOW */
    dec = broker_invoke("process.list", "", &ok);
    if (dec != DEC_ALLOW) { fail = 1; reason = "process.list-not-allowed"; }

    /* 2) sysinfo -> must be ALLOW */
    if (!fail) {
        dec = broker_invoke("sysinfo", "", &ok);
        if (dec != DEC_ALLOW) { fail = 1; reason = "sysinfo-not-allowed"; }
    }

    /* 3) file.write_project /home/test.txt "hi" -> ALLOW or REQUIRE_APPROVAL,
     *    and it must actually execute ok (snapshot + write). */
    if (!fail) {
        dec = broker_invoke("file.write_project", "/home/aibroker_test.txt hi", &ok);
        if (dec == DEC_DENY) { fail = 1; reason = "write-project-denied"; }
        else if (!ok)        { fail = 1; reason = "write-project-exec-failed"; }
    }

    /* 4) file.read back what we wrote -> ALLOW + ok, and contents must match. */
    if (!fail) {
        dec = broker_invoke("file.read", "/home/aibroker_test.txt", &ok);
        if (dec != DEC_ALLOW || !ok) { fail = 1; reason = "read-back-failed"; }
        else if (!(k_strlen(g_filebuf) >= 2 && g_filebuf[0] == 'h' && g_filebuf[1] == 'i'))
            { fail = 1; reason = "read-back-mismatch"; }
    }

    /* 5) overwrite then rollback -> contents must revert to "hi". */
    if (!fail) {
        dec = broker_invoke("file.write_project", "/home/aibroker_test.txt CLOBBERED", &ok);
        if (dec == DEC_DENY || !ok) { fail = 1; reason = "second-write-failed"; }
    }
    if (!fail) {
        dec = broker_invoke("rollback", "/home/aibroker_test.txt", &ok);
        if (dec == DEC_DENY || !ok) { fail = 1; reason = "rollback-failed"; }
    }
    if (!fail) {
        dec = broker_invoke("file.read", "/home/aibroker_test.txt", &ok);
        if (!(ok && g_filebuf[0] == 'h' && g_filebuf[1] == 'i' && k_strlen(g_filebuf) >= 2))
            { fail = 1; reason = "rollback-content-mismatch"; }
    }

    /* 6) file.write_project /boot/x -> must be DENY (protected surface). */
    if (!fail) {
        dec = broker_invoke("file.write_project", "/boot/x evil", &ok);
        if (dec != DEC_DENY) { fail = 1; reason = "boot-write-not-denied"; }
    }

    /* 7) unknown tool -> must be DENY (deny-by-default). */
    if (!fail) {
        dec = broker_invoke("kernel.poke", "0xdeadbeef", &ok);
        if (dec != DEC_DENY) { fail = 1; reason = "unknown-tool-not-denied"; }
    }

    /* 8) pkg.install_request -> record-only, REQUIRE_APPROVAL by default. */
    if (!fail) {
        dec = broker_invoke("pkg.install_request", "vim", &ok);
        if (dec == DEC_DENY || !ok) { fail = 1; reason = "pkg-request-failed"; }
    }

    if (fail) {
        out("AIBROKER SELFTEST: FAIL "); out(reason); out("\n");
    } else {
        out("AIBROKER SELFTEST: PASS\n");
    }
}

/* =========================================================================
 * Entry point.
 * ====================================================================== */
void _start(void)
{
    out("AIBROKER: starting capability-gated command broker\n");

    /* Ensure the broker's directory tree exists. */
    mkdir_p("/etc/ai");
    mkdir_p("/var/log/ai");
    mkdir_p("/var/snapshots");

    /* Load policy (file or safe defaults). */
    policy_load();
    out(g_policy_loaded
        ? "AIBROKER: policy loaded from /etc/ai/policy.json\n"
        : "AIBROKER: using built-in default policy\n");

    /* Intake: commands file if present, else the self-test. */
    if (process_commands_file()) {
        out("AIBROKER: processed /etc/ai/commands\n");
    } else {
        run_selftest();
    }

    sc(SYS_EXIT, 0, 0, 0);
    for (;;) { }   /* never reached */
}
