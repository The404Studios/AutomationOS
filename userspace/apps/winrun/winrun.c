/*
 * winrun.c -- WIN-MIN: run a Windows x64 console program (.exe) on AutomationOS, entirely in user space.
 * ======================================================================================================
 *
 *     winrun <path.exe> [args...]
 *
 * The bare minimum for "native Windows compatibility", as a pipeline whose every stage is assertable
 * (tools/proofkit/scenarios/wincompat.json greps these markers in order):
 *
 *   L0 parse     validate the PE32+ image with the hostile-input-safe parser (userspace/lib/pe)
 *   L1 map       allocate SizeOfImage (RWX: there is no mprotect syscall yet), copy headers + sections to their
 *                RVAs, apply base relocations (the image is placed wherever the kernel's mmap puts it)
 *   L2 imports   resolve every import against the shim table (win_shim.c); unresolved ones point at loud stubs
 *   L3 run       call the entry point through the Windows x64 ABI (ms_abi); ExitProcess/return ends the process
 *
 * No kernel changes are needed at this tier: vmm_mmap_anon honours an explicit PROT_EXEC and the PE is
 * position-independent through its relocation table. What this tier does NOT have (and the next bricks are):
 * a TEB (GS base), SEH, threads, and the C runtime's imports (msvcrt/ucrt) -- see docs/WIN_MIN.md.
 */
#include "../../lib/pe/pe.h"
#include "win_shim.h"

#define SYS_EXIT 0
#define SYS_READ 2
#define SYS_WRITE 3
#define SYS_OPEN 4
#define SYS_CLOSE 5
#define SYS_MMAP 37

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static long sc5(long n, long a, long b, long c, long d, long e) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
    return r;
}
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void put(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)slen(s)); }
static void put_u(u64 v) {
    char b[24]; int i = 23; b[i] = 0; if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    put(&b[i]);
}
static void put_x(u64 v) {
    char b[19]; int i = 18; b[i] = 0;
    do { int d = (int)(v & 15); b[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; } while (v);
    b[--i] = 'x'; b[--i] = '0';
    put(&b[i]);
}
static void die(const char *why, int code) {
    put("WINRUN: REJECT "); put(why); put("\n");
    sc(SYS_EXIT, code, 0, 0);
    for (;;) {}
}

#define MAX_EXE (16u * 1024u * 1024u)
#define READ_CHUNK (256u * 1024u)
static void *map_rwx(u64 n) {
    long r = sc5(SYS_MMAP, 0, (long)n, 7, 0, 0);
    return ((u64)r < 4096u) ? 0 : (void *)r;
}
static void zero(void *p, u64 n) { u8 *q = p; while (n--) *q++ = 0; }

static pe_info_t g_info;                                  /* ~3.7 KB: keep it off the stack */

typedef __attribute__((ms_abi)) unsigned (*win_entry_t)(void);

int main(int argc, char **argv) {
    if (argc < 2) { put("usage: winrun <program.exe> [args...]\n"); return 2; }
    const char *path = argv[1];

    /* ---- read the file ---- */
    long fd = sc(SYS_OPEN, (long)path, 0, 0);
    if (fd < 0) { put("WINRUN: REJECT cannot open "); put(path); put("\n"); return 2; }
    u8 *file = map_rwx(MAX_EXE);
    if (!file) die("out of memory for the file buffer", 2);
    u64 len = 0;
    for (;;) {
        /* sys_read rejects count > MAX_READ_SIZE (1 MiB) with EINVAL, so read in bounded chunks. */
        u64 want = MAX_EXE - len;
        if (want > READ_CHUNK) want = READ_CHUNK;
        long r = sc(SYS_READ, fd, (long)(file + len), (long)want);
        if (r < 0) die("read error on the file", 2);      /* an I/O error is not a truncated PE: say so */
        if (r == 0) break;
        len += (u64)r;
        if (len >= MAX_EXE) die("file larger than 16 MiB", 2);
    }
    sc(SYS_CLOSE, fd, 0, 0);

    /* ---- L0: parse ---- */
    int r = pe_parse(file, len, &g_info);
    if (r != PE_OK) {
        put("WINRUN: REJECT "); put(pe_strerror(r)); put(" (pe code="); put_u((u64)(-r)); put(") file="); put(path); put("\n");
        return 2;
    }
    put("WINRUN: L0 parse OK file="); put(path);
    put(" size="); put_u(len);
    put(" machine=x86_64 subsystem="); put(g_info.subsystem == 3 ? "console" : g_info.subsystem == 2 ? "gui" : "other");
    put(" image_base="); put_x(g_info.image_base);
    put(" size_of_image="); put_x(g_info.size_of_image);
    put(" sections="); put_u(g_info.n_sections);
    put(" entry_rva="); put_x(g_info.entry_rva); put("\n");
    if (g_info.is_dll) die("a DLL cannot be run directly", 2);

    /* ---- L1: map + relocate ---- */
    u8 *img = map_rwx(g_info.size_of_image);
    if (!img) die("out of memory for the image", 2);
    zero(img, g_info.size_of_image);                       /* pe_map requires a zeroed image */
    r = pe_map(file, len, &g_info, img);
    if (r != PE_OK) { put("WINRUN: REJECT map: "); put(pe_strerror(r)); put("\n"); return 2; }
    u64 base = (u64)img;
    r = pe_relocate(&g_info, img, base);
    if (r != PE_OK) { put("WINRUN: REJECT relocate: "); put(pe_strerror(r)); put("\n"); return 2; }
    put("WINRUN: L1 mapped at "); put_x(base); put(" preferred="); put_x(g_info.image_base);
    put(" delta="); put(base == g_info.image_base ? "0" : "nonzero"); put(" relocs=applied\n");

    u64 tls_s = 0, tls_e = 0, tls_i = 0, tls_cb = 0;
    if (pe_tls_info(&g_info, img, base, &tls_s, &tls_e, &tls_i, &tls_cb) == PE_OK)
        put("WINRUN: note: image has a TLS directory -- thread-locals are not supported at this tier (no GS/TEB)\n");

    /* ---- L2: imports ---- */
    char cmdline[512];
    unsigned k = 0;
    cmdline[k++] = '"';
    for (unsigned i = 0; path[i] && k < sizeof(cmdline) - 4; i++) cmdline[k++] = path[i];
    cmdline[k++] = '"';
    for (int a = 2; a < argc && k < sizeof(cmdline) - 2; a++) {
        cmdline[k++] = ' ';
        for (unsigned i = 0; argv[a][i] && k < sizeof(cmdline) - 2; i++) cmdline[k++] = argv[a][i];
    }
    cmdline[k] = 0;
    win_shim_init(base, path, cmdline);

    unsigned n_total = 0, n_unres = 0;
    r = pe_resolve_imports(&g_info, img, win_shim_resolve, 0, &n_total, &n_unres);
    if (r != PE_OK) { put("WINRUN: REJECT imports: "); put(pe_strerror(r)); put("\n"); return 2; }
    put("WINRUN: L2 imports total="); put_u(n_total); put(" unresolved="); put_u(n_unres); put("\n");
    for (unsigned i = 0; i < win_shim_unresolved(); i++) {
        put("WINRUN:   unresolved "); put(win_shim_unresolved_dll(i)); put("!"); put(win_shim_unresolved_name(i)); put("\n");
    }

    /* ---- L3: run ---- */
    win_entry_t entry = (win_entry_t)(img + g_info.entry_rva);
    put("WINRUN: L3 entry "); put_x((u64)entry); put(" -> calling\n");
    unsigned code = entry();                               /* normally never returns: the program calls ExitProcess */
    put("WINRUN: L3 returned code="); put_u(code); put("\n");
    return (int)code;
}
