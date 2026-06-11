/*
 * meminfo.c -- Userspace memory reporter (freestanding, ring 3, no libc).
 * ======================================================================
 *
 * A tiny CLI tool that prints a memory report to stdout (fd 1):
 *
 *   1. A system-wide summary from SYS_SYSINFO(62): total / free / used MiB,
 *      uptime, and live process count.
 *   2. A per-process resident-memory table built from SYS_PROCLIST(44) plus
 *      one SYS_PROC_QUERY(60) per pid: pid, name, resident KiB (mem_pages * 4K),
 *      and VMA count.
 *
 * Every syscall degrades gracefully: any negative return is treated as
 * "unavailable" and rendered as "n/a" rather than a hard failure, so the tool
 * still runs before SYS_SYSINFO / SYS_PROC_QUERY are wired. The base
 * SYS_PROCLIST is the fallback enumeration source (always available).
 *
 * SELF-TEST: if the system summary printed with real numbers (SYS_SYSINFO
 * returned >= 0), the tool ends with "MEMINFO SELFTEST: PASS"; otherwise
 * "MEMINFO SELFTEST: FAIL". Either way it exits via SYS_EXIT(0).
 *
 * No libc, no includes: pure inline syscalls + freestanding helpers. ABI types
 * mirror userspace/lib/aictl/aictl.h EXACTLY (procinfo_t = 48 bytes,
 * proc_detail_t, sysinfo_t). Syscall numbers verified against
 * kernel/include/syscall.h.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable):
 *
 *   cd /path/to/Kernel
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/meminfo/meminfo.c -o /tmp/meminfo.o
 *
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/meminfo.o -o /tmp/meminfo.elf
 *
 *   # Verify no stack-canary reference (must produce no output):
 *   objdump -d /tmp/meminfo.elf | grep 'fs:0x28'
 *
 *
 * ============================ REGION MODEL =============================
 *
 * This OS ALREADY has a "region layer" sitting above raw pages -- it just
 * lives in the kernel and is not yet exposed to userspace as a first-class
 * managed object. Understanding what exists vs. what is missing:
 *
 *   WHAT EXISTS (the per-process VMA region manager):
 *     kernel/core/mem/vma_region.c + kernel/include/vma.h define vma_t, a
 *     per-process linked list of regions. Each region is:
 *         { vaddr, length, perm (R/W/X), flags (COW/GROWSDOWN),
 *           backing (ANON zero-fill | FILE initrd-backed),
 *           file_ptr/file_off/file_sz }
 *     The ELF loader installs one VMA per PT_LOAD segment (file-backed) plus
 *     one for the stack (anon). This list is the authoritative per-process
 *     memory map: handle_page_fault() consults vma_find() to tell a legitimate
 *     access apart from a real segfault, and faults the page in from the VMA's
 *     backing (copy from initrd for FILE, demand-zero for ANON), applying the
 *     VMA's W^X permissions to the new PTE. So "regions above pages" is real:
 *     a VMA is a contiguous, page-aligned span with uniform perms + one
 *     backing source, and the page tables are just its cached projection.
 *
 *   WHAT IT SITS ON:
 *     - kernel/core/mem/pmm.c: the physical frame allocator (buddy free-lists +
 *       a contiguous-run bitmap + per-CPU page caches). Every faulted-in region
 *       page comes from pmm_alloc_page(); pmm_get_total/used/free_memory() feed
 *       SYS_SYSINFO. This is the byte-accounting layer this tool reports.
 *     - kernel/core/mem/heap.c: the kernel's own kmalloc/kfree heap (segregated
 *       bins, on-demand growth). vma_add() uses kmalloc to store each vma_t.
 *     - kernel/core/mem/vma.c: a SEPARATE anonymous-mmap bump allocator for
 *       SYS_MMAP (VMM_ANON_VA_BASE window). Note this is NOT the region list --
 *       it is a per-address-space VA cursor, and its own comments call out the
 *       missing piece: "A real implementation would track free VA ranges (a
 *       VMA list/tree) per address space."
 *
 *   WHAT IS MISSING (the proposed user-facing RegionManager):
 *     There is no syscall that lets userspace name, enumerate, or reserve a
 *     region as a managed object. SYS_PROC_QUERY only exposes a vma_count
 *     (the length of the list), not the regions themselves. A future
 *     RegionManager / region-mmap syscall would add, on top of the existing
 *     vma_t plumbing, a richer descriptor:
 *         region { base, size, perms, owner (pid), purpose/tag }
 *     and operations to create / resize / query / hand-off regions by name or
 *     handle (e.g. a SYS_REGION_MMAP that records purpose + owner, and a
 *     SYS_REGION_QUERY that returns the per-region table this tool would then
 *     print instead of just a count). That turns the kernel's internal VMA
 *     bookkeeping into the explicit "above pages" abstraction the project
 *     wants: regions become observable, attributable, and shareable rather
 *     than an opaque count. The mechanism is already there; only the user ABI
 *     (the named/owned/purpose-tagged descriptor + its syscalls) is absent.
 *
 * Serial/stdout output on start: a formatted report, ending with the
 * "MEMINFO SELFTEST:" line.
 * ====================================================================== */

/* -----------------------------------------------------------------------
 * Freestanding integer types (no stdint.h). Match aictl.h.
 * --------------------------------------------------------------------- */
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef int                i32;

/* -----------------------------------------------------------------------
 * Syscall numbers (verified against kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_EXIT        0
#define SYS_WRITE       3
#define SYS_PROCLIST    44   /* sc(44, buf, max, 0)      -> count or -errno   */
#define SYS_PROC_QUERY  60   /* sc(60, pid, &detail, 0)  -> 0 or -errno       */
#define SYS_SYSINFO     62   /* sc(62, &info, 0, 0)      -> 0 or -errno       */

/* fd for normal output. */
#define FD_STDOUT       1

/* Upper bound on processes we enumerate / print. */
#define MAX_PROCS       64

/* -----------------------------------------------------------------------
 * ABI structs -- byte-for-byte mirrors of userspace/lib/aictl/aictl.h.
 * --------------------------------------------------------------------- */

/* 64-byte shallow process entry from SYS_PROCLIST. */
typedef struct {
    u32  pid;          /* offset  0 */
    u32  parent_pid;   /* offset  4 */
    u32  state;        /* offset  8 */
    u32  flags;        /* offset 12 */
    char name[32];     /* offset 16 .. 47 */
    u64  cpu_ticks;    /* offset 48  timer ticks observed while running */
    u64  ctx_switches; /* offset 56  number of times dispatched */
} procinfo_t;          /* total: 64 bytes */

/* Compile-time layout assertion (matches aictl.h). */
typedef char _procinfo_size_assert[sizeof(procinfo_t) == 64 ? 1 : -1];

/* Rich per-process record from SYS_PROC_QUERY. */
typedef struct {
    u32  pid;
    u32  ppid;
    u32  state;
    u32  prio;
    u64  cpu_ticks;
    u32  mem_pages;    /* resident physical pages */
    u32  vma_count;    /* number of virtual memory areas */
    char name[32];
} proc_detail_t;

/* System-wide statistics from SYS_SYSINFO (must match kernel procapi.h: 32 bytes). */
typedef struct {
    u64 total_mem;     /* total physical memory in bytes    */
    u64 free_mem;      /* free physical memory in bytes     */
    u64 uptime_ms;     /* milliseconds since boot           */
    u32 proc_count;    /* total live processes              */
    u32 heap_used_kb;  /* kernel heap usage in KiB (0=old)  */
} sysinfo_t;

/* -----------------------------------------------------------------------
 * Inline syscall helper (up to 3 args -- sufficient for our usage).
 * Returns the raw syscall return in rax (signed: < 0 means -errno).
 * --------------------------------------------------------------------- */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* -----------------------------------------------------------------------
 * Freestanding helpers: strings, integer formatting, output buffer.
 * --------------------------------------------------------------------- */

static unsigned long m_strlen(const char *s)
{
    unsigned long n = 0;
    while (s && s[n]) n++;
    return n;
}

/* Append NUL-terminated src to dst at *pos, never exceeding cap-1; keeps dst
 * NUL-terminated. Advances *pos. */
static void buf_puts(char *dst, unsigned long *pos, unsigned long cap, const char *src)
{
    while (src && *src && *pos + 1 < cap) {
        dst[(*pos)++] = *src++;
    }
    dst[*pos] = '\0';
}

/* Append a single char. */
static void buf_putc(char *dst, unsigned long *pos, unsigned long cap, char c)
{
    if (*pos + 1 < cap) {
        dst[(*pos)++] = c;
    }
    dst[*pos] = '\0';
}

/* Append an unsigned 64-bit value in decimal. */
static void buf_putu(char *dst, unsigned long *pos, unsigned long cap, u64 val)
{
    char tmp[24];
    int  i = 0;
    if (val == 0) {
        buf_putc(dst, pos, cap, '0');
        return;
    }
    while (val > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (int)(val % 10ULL));
        val /= 10ULL;
    }
    while (i-- > 0) {
        buf_putc(dst, pos, cap, tmp[i]);
    }
}

/* Append val right-justified in a field of `width` (space padded). */
static void buf_putu_pad(char *dst, unsigned long *pos, unsigned long cap,
                         u64 val, int width)
{
    /* Count digits. */
    int   digits = 1;
    u64   v = val;
    while (v >= 10ULL) { v /= 10ULL; digits++; }
    for (int i = digits; i < width; i++) {
        buf_putc(dst, pos, cap, ' ');
    }
    buf_putu(dst, pos, cap, val);
}

/* Append a string left-justified in a field of `width` (space padded). */
static void buf_puts_pad(char *dst, unsigned long *pos, unsigned long cap,
                         const char *src, int width)
{
    int n = (int)m_strlen(src);
    buf_puts(dst, pos, cap, src);
    for (int i = n; i < width; i++) {
        buf_putc(dst, pos, cap, ' ');
    }
}

/* Flush a NUL-terminated buffer to stdout. */
static void write_out(const char *s)
{
    sc(SYS_WRITE, FD_STDOUT, (long)s, (long)m_strlen(s));
}

/* -----------------------------------------------------------------------
 * Report assembly.
 * --------------------------------------------------------------------- */

#define REPORT_CAP 8192
static char g_report[REPORT_CAP];

void _start(void)
{
    unsigned long pos = 0;
    g_report[0] = '\0';

    int sysinfo_ok = 0;   /* drives the self-test verdict */

    /* ---- Header ---- */
    buf_puts(g_report, &pos, REPORT_CAP,
             "==== meminfo: memory report ====\n");

    /* ---- 1. System summary (SYS_SYSINFO) ---- */
    sysinfo_t info;
    /* Zero it so a partial/failed call shows clean "n/a" values. */
    {
        char *p = (char *)&info;
        for (unsigned long i = 0; i < sizeof(info); i++) p[i] = 0;
    }

    long si_rc = sc(SYS_SYSINFO, (long)&info, 0, 0);
    if (si_rc >= 0) {
        sysinfo_ok = 1;

        u64 total_mb = info.total_mem / (1024ULL * 1024ULL);
        u64 free_mb  = info.free_mem  / (1024ULL * 1024ULL);
        u64 used_mb  = (info.total_mem >= info.free_mem)
                         ? (info.total_mem - info.free_mem) / (1024ULL * 1024ULL)
                         : 0;
        u64 up_s     = info.uptime_ms / 1000ULL;
        u64 up_h     = up_s / 3600ULL;
        u64 up_m     = (up_s % 3600ULL) / 60ULL;
        u64 up_ss    = up_s % 60ULL;

        buf_puts(g_report, &pos, REPORT_CAP, "Memory total : ");
        buf_putu(g_report, &pos, REPORT_CAP, total_mb);
        buf_puts(g_report, &pos, REPORT_CAP, " MiB\n");

        buf_puts(g_report, &pos, REPORT_CAP, "Memory used  : ");
        buf_putu(g_report, &pos, REPORT_CAP, used_mb);
        buf_puts(g_report, &pos, REPORT_CAP, " MiB\n");

        buf_puts(g_report, &pos, REPORT_CAP, "Memory free  : ");
        buf_putu(g_report, &pos, REPORT_CAP, free_mb);
        buf_puts(g_report, &pos, REPORT_CAP, " MiB\n");

        buf_puts(g_report, &pos, REPORT_CAP, "Uptime       : ");
        buf_putu_pad(g_report, &pos, REPORT_CAP, up_h, 2);
        buf_putc(g_report, &pos, REPORT_CAP, ':');
        buf_putu_pad(g_report, &pos, REPORT_CAP, up_m, 2);
        buf_putc(g_report, &pos, REPORT_CAP, ':');
        buf_putu_pad(g_report, &pos, REPORT_CAP, up_ss, 2);
        buf_putc(g_report, &pos, REPORT_CAP, '\n');

        buf_puts(g_report, &pos, REPORT_CAP, "Processes    : ");
        buf_putu(g_report, &pos, REPORT_CAP, (u64)info.proc_count);
        buf_putc(g_report, &pos, REPORT_CAP, '\n');

        /* Kernel heap usage (available when heap_used_kb > 0; older kernels
         * return 0 here because the field was reserved padding). */
        if (info.heap_used_kb > 0) {
            buf_puts(g_report, &pos, REPORT_CAP, "Heap used    : ");
            buf_putu(g_report, &pos, REPORT_CAP, (u64)info.heap_used_kb);
            buf_puts(g_report, &pos, REPORT_CAP, " KiB\n");
        }
    } else {
        /* Degrade gracefully -- SYS_SYSINFO not wired or errored. */
        buf_puts(g_report, &pos, REPORT_CAP, "Memory total : n/a\n");
        buf_puts(g_report, &pos, REPORT_CAP, "Memory used  : n/a\n");
        buf_puts(g_report, &pos, REPORT_CAP, "Memory free  : n/a\n");
        buf_puts(g_report, &pos, REPORT_CAP, "Uptime       : n/a\n");
        buf_puts(g_report, &pos, REPORT_CAP, "Processes    : n/a\n");
    }

    /* ---- 2. Per-process resident-memory table ---- */
    buf_puts(g_report, &pos, REPORT_CAP,
             "\n  PID  NAME                RES(KiB)   VMAs\n");
    buf_puts(g_report, &pos, REPORT_CAP,
             "  ---  ------------------  --------   ----\n");

    static procinfo_t procs[MAX_PROCS];
    long n = sc(SYS_PROCLIST, (long)procs, MAX_PROCS, 0);

    if (n < 0) {
        buf_puts(g_report, &pos, REPORT_CAP,
                 "  (process list unavailable)\n");
    } else {
        if (n > MAX_PROCS) n = MAX_PROCS;   /* defensive clamp */
        for (long i = 0; i < n; i++) {
            /* pid (3-wide) */
            buf_puts(g_report, &pos, REPORT_CAP, "  ");
            buf_putu_pad(g_report, &pos, REPORT_CAP, (u64)procs[i].pid, 3);
            buf_puts(g_report, &pos, REPORT_CAP, "  ");

            /* name (18-wide, force NUL-termination of the fixed field) */
            char name[33];
            int  j;
            for (j = 0; j < 32; j++) name[j] = procs[i].name[j];
            name[32] = '\0';
            buf_puts_pad(g_report, &pos, REPORT_CAP, name, 18);
            buf_puts(g_report, &pos, REPORT_CAP, "  ");

            /* Query rich detail for resident pages + VMA count. */
            proc_detail_t d;
            {
                char *p = (char *)&d;
                for (unsigned long k = 0; k < sizeof(d); k++) p[k] = 0;
            }
            long q = sc(SYS_PROC_QUERY, (long)procs[i].pid, (long)&d, 0);
            if (q >= 0) {
                /* resident KiB = mem_pages * 4KiB = mem_pages * 4 */
                u64 res_kib = (u64)d.mem_pages * 4ULL;
                buf_putu_pad(g_report, &pos, REPORT_CAP, res_kib, 8);
                buf_puts(g_report, &pos, REPORT_CAP, "   ");
                buf_putu_pad(g_report, &pos, REPORT_CAP, (u64)d.vma_count, 4);
            } else {
                /* Degrade gracefully per row. */
                buf_puts_pad(g_report, &pos, REPORT_CAP, "n/a", 8);
                buf_puts(g_report, &pos, REPORT_CAP, "   ");
                buf_puts_pad(g_report, &pos, REPORT_CAP, "n/a", 4);
            }
            buf_putc(g_report, &pos, REPORT_CAP, '\n');
        }
        if (n == 0) {
            buf_puts(g_report, &pos, REPORT_CAP, "  (no processes)\n");
        }
    }

    /* ---- Self-test verdict ---- */
    buf_puts(g_report, &pos, REPORT_CAP, "\n");
    if (sysinfo_ok) {
        buf_puts(g_report, &pos, REPORT_CAP, "MEMINFO SELFTEST: PASS\n");
    } else {
        buf_puts(g_report, &pos, REPORT_CAP, "MEMINFO SELFTEST: FAIL\n");
    }

    /* ---- Emit the whole report and exit. ---- */
    write_out(g_report);
    sc(SYS_EXIT, 0, 0, 0);

    /* Should never reach here; spin defensively if SYS_EXIT ever returns. */
    for (;;) {
        asm volatile("hlt");
    }
}
