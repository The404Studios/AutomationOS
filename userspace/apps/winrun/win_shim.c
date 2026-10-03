/*
 * win_shim.c -- the Windows side of WIN-MIN: kernel32 (+ a few ntdll/msvcrt basics) implemented on this OS's
 * syscalls, compiled with the WINDOWS x64 CALLING CONVENTION (ms_abi: args in RCX/RDX/R8/R9, 32-byte shadow
 * space, 16-byte aligned stack) so a PE image can call them through its import table.
 *
 * Freestanding ring 3, linked into sbin/winrun. Single-threaded (tier 1): critical sections are no-ops, TLS
 * slots are one global array, there is no TEB (GS base is not set; code that reads gs: will fault -- listed as the
 * next kernel brick in docs/WIN_MIN.md).
 *
 * Coverage rule: anything NOT implemented resolves to a numbered stub that prints
 *     WINRUN: FATAL: unimplemented import KERNEL32!Name
 * and exits 127 *when called* (not at load) -- so rarely-used imports never block a program, and a missing one
 * is named loudly instead of crashing mysteriously. winrun also lists every unresolved import at load time; the
 * proofkit scenario `wincompat` asserts "unresolved=0" for each fixture, which makes the surface measurable.
 */
#include "win_shim.h"

#define MS __attribute__((ms_abi))

/* ---- this OS's syscalls (kernel/include/syscall.h) ---- */
#define SYS_EXIT 0
#define SYS_READ 2
#define SYS_WRITE 3
#define SYS_GETPID 8
#define SYS_SLEEP 9
#define SYS_MMAP 37
#define SYS_MUNMAP 38
#define SYS_GET_TICKS_MS 40

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

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef int BOOL;
typedef u32 DWORD;
typedef void *HANDLE;
typedef unsigned long long SIZE_T;

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void put(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)slen(s)); }
static void put_u(u64 v) {
    char b[24]; int i = 23; b[i] = 0; if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    put(&b[i]);
}
static int ieq(const char *a, const char *b) {          /* case-insensitive equality */
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static int starts_i(const char *s, const char *pre) {
    for (; *pre; s++, pre++) {
        char x = *s, y = *pre;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}
static void *mem_set(void *d, int c, u64 n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
static void *mem_cpy(void *d, const void *s, u64 n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }

/* ============================================================== process-wide state */
static u64 g_image_base;
static char g_cmdline[512];
static char g_module_path[256];
static u32 g_last_error;
void win_shim_init(u64 image_base, const char *module_path, const char *cmdline) {
    g_image_base = image_base;
    unsigned i;
    for (i = 0; module_path && module_path[i] && i < sizeof(g_module_path) - 1; i++) g_module_path[i] = module_path[i];
    g_module_path[i] = 0;
    for (i = 0; cmdline && cmdline[i] && i < sizeof(g_cmdline) - 1; i++) g_cmdline[i] = cmdline[i];
    g_cmdline[i] = 0;
}

/* ---- virtual memory (every Windows allocation is RWX here: there is no mprotect syscall yet) ---- */
#define PROT_RWX 7
static void *vm_alloc(u64 size) {
    long r = sc5(SYS_MMAP, 0, (long)size, PROT_RWX, 0, 0);
    return ((u64)r < 4096u) ? 0 : (void *)r;
}
#define MAX_VALLOC 128
static struct { void *p; u64 n; } g_valloc[MAX_VALLOC];

/* ============================================================== KERNEL32 */
static MS void ExitProcess(unsigned int code) {
    put("WINRUN: exit code="); put_u(code); put("\n");
    sc(SYS_EXIT, (long)code, 0, 0);
    for (;;) {}
}
static MS BOOL TerminateProcess(HANDLE h, unsigned int code) { (void)h; ExitProcess(code); return 0; }

#define H_STDIN ((HANDLE)0x100)
#define H_STDOUT ((HANDLE)0x101)
#define H_STDERR ((HANDLE)0x102)
static int handle_fd(HANDLE h) {
    if (h == H_STDIN) return 0;
    if (h == H_STDOUT) return 1;
    if (h == H_STDERR) return 2;
    return -1;
}
static MS HANDLE GetStdHandle(DWORD n) {
    if (n == (DWORD)-10) return H_STDIN;
    if (n == (DWORD)-11) return H_STDOUT;
    if (n == (DWORD)-12) return H_STDERR;
    return (HANDLE)(long long)-1;
}
static MS BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov) {
    (void)ov;
    int fd = handle_fd(h);
    if (fd < 1) { g_last_error = 6; if (written) *written = 0; return 0; }   /* ERROR_INVALID_HANDLE */
    long r = sc(SYS_WRITE, fd, (long)buf, (long)n);
    if (written) *written = r > 0 ? (DWORD)r : 0;
    return r >= 0;
}
static MS BOOL WriteConsoleA(HANDLE h, const void *buf, DWORD n, DWORD *written, void *res) { (void)res; return WriteFile(h, buf, n, written, 0); }
static MS BOOL WriteConsoleW(HANDLE h, const u16 *buf, DWORD n, DWORD *written, void *res) {
    (void)res;
    char tmp[256];
    DWORD done = 0;
    while (done < n) {                                   /* best effort: UTF-16 -> '?'-safe 7-bit in chunks */
        DWORD k = 0;
        while (done + k < n && k < sizeof(tmp)) { u16 c = buf[done + k]; tmp[k] = (c < 0x80) ? (char)c : '?'; k++; }
        DWORD w = 0;
        if (!WriteFile(h, tmp, k, &w, 0)) break;
        done += k;
    }
    if (written) *written = done;
    return 1;
}
static MS BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *read, void *ov) {
    (void)ov;
    int fd = handle_fd(h);
    if (fd != 0) { g_last_error = 6; if (read) *read = 0; return 0; }
    long r = sc(SYS_READ, 0, (long)buf, (long)n);
    if (read) *read = r > 0 ? (DWORD)r : 0;
    return 1;                                            /* EOF on stdin is success with 0 bytes */
}
static MS BOOL GetConsoleMode(HANDLE h, DWORD *mode) { (void)h; if (mode) *mode = 0; return 0; }
static MS BOOL SetConsoleMode(HANDLE h, DWORD mode) { (void)h; (void)mode; return 1; }
static MS BOOL SetConsoleCtrlHandler(void *fn, BOOL add) { (void)fn; (void)add; return 1; }
static MS DWORD GetFileType(HANDLE h) { return handle_fd(h) >= 0 ? 2u /* FILE_TYPE_CHAR */ : 0u; }
static MS BOOL CloseHandle(HANDLE h) { (void)h; return 1; }

static MS const char *GetCommandLineA(void) { return g_cmdline; }
static MS HANDLE GetModuleHandleA(const char *name) { return (!name || !*name) ? (HANDLE)g_image_base : 0; }
static MS HANDLE GetModuleHandleW(const u16 *name) { return (!name || !*name) ? (HANDLE)g_image_base : 0; }
static MS DWORD GetModuleFileNameA(HANDLE m, char *buf, DWORD cap) {
    (void)m;
    DWORD n = (DWORD)slen(g_module_path);
    if (n >= cap) n = cap ? cap - 1 : 0;
    mem_cpy(buf, g_module_path, n);
    if (cap) buf[n] = 0;
    return n;
}
static MS DWORD GetLastError(void) { return g_last_error; }
static MS void SetLastError(DWORD e) { g_last_error = e; }
static MS HANDLE GetCurrentProcess(void) { return (HANDLE)(long long)-1; }
static MS DWORD GetCurrentProcessId(void) { return (DWORD)sc(SYS_GETPID, 0, 0, 0); }
static MS DWORD GetCurrentThreadId(void) { return (DWORD)sc(SYS_GETPID, 0, 0, 0); }
static MS BOOL IsDebuggerPresent(void) { return 0; }
static MS void *SetUnhandledExceptionFilter(void *f) { (void)f; return 0; }
static MS DWORD GetACP(void) { return 1252; }
static MS DWORD GetEnvironmentVariableA(const char *n, char *b, DWORD c) { (void)n; (void)b; (void)c; g_last_error = 203; return 0; }
static MS void GetStartupInfoA(void *si) { mem_set(si, 0, 104); *(DWORD *)si = 104; }

/* ---- time ---- */
static MS DWORD GetTickCount(void) { return (DWORD)sc(SYS_GET_TICKS_MS, 0, 0, 0); }
static MS u64 GetTickCount64(void) { return (u64)sc(SYS_GET_TICKS_MS, 0, 0, 0); }
static MS void Sleep(DWORD ms) { sc(SYS_SLEEP, (long)ms, 0, 0); }
static MS BOOL QueryPerformanceFrequency(long long *f) { if (f) *f = 1000000; return 1; }
static MS BOOL QueryPerformanceCounter(long long *c) { if (c) *c = (long long)sc(SYS_GET_TICKS_MS, 0, 0, 0) * 1000; return 1; }

/* ---- memory: VirtualAlloc/Free + a simple process heap ---- */
static MS void *VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD prot) {
    (void)type; (void)prot;
    if (addr || !size) { g_last_error = 87; return 0; }  /* fixed-address reservations are not supported */
    void *p = vm_alloc(size);
    if (!p) { g_last_error = 8; return 0; }
    for (int i = 0; i < MAX_VALLOC; i++) if (!g_valloc[i].p) { g_valloc[i].p = p; g_valloc[i].n = size; break; }
    return p;
}
static MS BOOL VirtualFree(void *addr, SIZE_T size, DWORD type) {
    (void)size;
    if (!(type & 0x8000)) return 1;                      /* MEM_DECOMMIT: nothing to do */
    for (int i = 0; i < MAX_VALLOC; i++)
        if (g_valloc[i].p == addr) { sc(SYS_MUNMAP, (long)addr, (long)g_valloc[i].n, 0); g_valloc[i].p = 0; return 1; }
    return 0;
}
static MS BOOL VirtualProtect(void *a, SIZE_T n, DWORD prot, DWORD *old) { (void)a; (void)n; (void)prot; if (old) *old = 0x40; return 1; }
static MS SIZE_T VirtualQuery(const void *a, void *mbi, SIZE_T len) {
    if (len < 48) return 0;
    u64 *w = mbi;
    mem_set(mbi, 0, 48);
    w[0] = (u64)a & ~0xFFFull;                           /* BaseAddress */
    w[1] = w[0];                                          /* AllocationBase */
    ((DWORD *)mbi)[4] = 0x40;                             /* AllocationProtect  PAGE_EXECUTE_READWRITE */
    w[3] = 0x1000;                                        /* RegionSize */
    ((DWORD *)mbi)[8] = 0x1000;                           /* State  MEM_COMMIT */
    ((DWORD *)mbi)[9] = 0x40;                             /* Protect */
    return 48;
}

/* heap: bump arenas + exact-size-class free lists (16-byte granules, powers of two up to 64 KiB; bigger = own mapping) */
#define HEAP_HDR 16
#define HEAP_CLASSES 13                                   /* 16 B .. 64 KiB */
static u8 *g_arena, *g_arena_end;
static void *g_free[HEAP_CLASSES];
static int cls_of(u64 n) { int c = 0; u64 s = 16; while (s < n + HEAP_HDR && c < HEAP_CLASSES) { s <<= 1; c++; } return c; }
static void *heap_alloc(u64 n) {
    int c = cls_of(n);
    if (c >= HEAP_CLASSES) {                              /* large: dedicated mapping, class marker 0xFF */
        u8 *p = vm_alloc(n + HEAP_HDR);
        if (!p) return 0;
        *(u64 *)p = n + HEAP_HDR;
        *(u64 *)(p + 8) = 0xFFull;
        return p + HEAP_HDR;
    }
    if (g_free[c]) { u8 *p = g_free[c]; g_free[c] = *(void **)(p + HEAP_HDR); return p + HEAP_HDR; }
    u64 sz = 16ull << c;
    if (!g_arena || g_arena + sz > g_arena_end) {
        u64 ar = 1ull << 20;
        g_arena = vm_alloc(ar);
        if (!g_arena) return 0;
        g_arena_end = g_arena + ar;
    }
    u8 *p = g_arena;
    g_arena += sz;
    *(u64 *)p = sz;
    *(u64 *)(p + 8) = (u64)c;
    return p + HEAP_HDR;
}
static void heap_free(void *q) {
    if (!q) return;
    u8 *p = (u8 *)q - HEAP_HDR;
    u64 c = *(u64 *)(p + 8);
    if (c == 0xFFull) { sc(SYS_MUNMAP, (long)p, (long)*(u64 *)p, 0); return; }
    *(void **)(p + HEAP_HDR) = g_free[c];
    g_free[c] = p;
}
static u64 heap_size(void *q) { u8 *p = (u8 *)q - HEAP_HDR; return *(u64 *)p - HEAP_HDR; }
#define H_PROCHEAP ((HANDLE)0x200)
static MS HANDLE GetProcessHeap(void) { return H_PROCHEAP; }
static MS HANDLE HeapCreate(DWORD o, SIZE_T i, SIZE_T m) { (void)o; (void)i; (void)m; return H_PROCHEAP; }
static MS BOOL HeapDestroy(HANDLE h) { (void)h; return 1; }
static MS void *HeapAlloc(HANDLE h, DWORD flags, SIZE_T n) {
    (void)h;
    void *p = heap_alloc(n);
    if (p && (flags & 8)) mem_set(p, 0, n);              /* HEAP_ZERO_MEMORY */
    if (!p) g_last_error = 8;
    return p;
}
static MS BOOL HeapFree(HANDLE h, DWORD f, void *p) { (void)h; (void)f; heap_free(p); return 1; }
static MS void *HeapReAlloc(HANDLE h, DWORD flags, void *p, SIZE_T n) {
    if (!p) return HeapAlloc(h, flags, n);
    u64 old = heap_size(p);
    if (n <= old) return p;
    void *q = HeapAlloc(h, flags, n);
    if (!q) return 0;
    mem_cpy(q, p, old);
    heap_free(p);
    return q;
}
static MS SIZE_T HeapSize(HANDLE h, DWORD f, const void *p) { (void)h; (void)f; return p ? heap_size((void *)p) : (SIZE_T)-1; }

/* ---- critical sections (single-threaded tier: no-ops) + TLS slots (one global array) ---- */
static MS void InitializeCriticalSection(void *cs) { (void)cs; }
static MS BOOL InitializeCriticalSectionAndSpinCount(void *cs, DWORD c) { (void)cs; (void)c; return 1; }
static MS void EnterCriticalSection(void *cs) { (void)cs; }
static MS void LeaveCriticalSection(void *cs) { (void)cs; }
static MS void DeleteCriticalSection(void *cs) { (void)cs; }
#define TLS_SLOTS 64
static void *g_tls[TLS_SLOTS];
static u8 g_tls_used[TLS_SLOTS];
static MS DWORD TlsAlloc(void) { for (DWORD i = 0; i < TLS_SLOTS; i++) if (!g_tls_used[i]) { g_tls_used[i] = 1; g_tls[i] = 0; return i; } return (DWORD)-1; }
static MS BOOL TlsFree(DWORD i) { if (i >= TLS_SLOTS) return 0; g_tls_used[i] = 0; return 1; }
static MS void *TlsGetValue(DWORD i) { return i < TLS_SLOTS ? g_tls[i] : 0; }
static MS BOOL TlsSetValue(DWORD i, void *v) { if (i >= TLS_SLOTS) return 0; g_tls[i] = v; return 1; }

/* ============================================================== name -> function table */
static MS void unimpl_dispatch(unsigned idx);
typedef struct { const char *dll; const char *name; void *fn; } shim_t;
#define K "kernel32.dll"
#define E(dll, n, f) { dll, n, (void *)f }
static const shim_t g_table[] = {
    E(K, "ExitProcess", ExitProcess), E(K, "TerminateProcess", TerminateProcess),
    E(K, "GetStdHandle", GetStdHandle), E(K, "WriteFile", WriteFile), E(K, "ReadFile", ReadFile),
    E(K, "WriteConsoleA", WriteConsoleA), E(K, "WriteConsoleW", WriteConsoleW),
    E(K, "GetConsoleMode", GetConsoleMode), E(K, "SetConsoleMode", SetConsoleMode),
    E(K, "SetConsoleCtrlHandler", SetConsoleCtrlHandler), E(K, "GetFileType", GetFileType), E(K, "CloseHandle", CloseHandle),
    E(K, "GetCommandLineA", GetCommandLineA), E(K, "GetModuleHandleA", GetModuleHandleA), E(K, "GetModuleHandleW", GetModuleHandleW),
    E(K, "GetModuleFileNameA", GetModuleFileNameA), E(K, "GetLastError", GetLastError), E(K, "SetLastError", SetLastError),
    E(K, "GetCurrentProcess", GetCurrentProcess), E(K, "GetCurrentProcessId", GetCurrentProcessId),
    E(K, "GetCurrentThreadId", GetCurrentThreadId), E(K, "IsDebuggerPresent", IsDebuggerPresent),
    E(K, "SetUnhandledExceptionFilter", SetUnhandledExceptionFilter), E(K, "GetACP", GetACP),
    E(K, "GetEnvironmentVariableA", GetEnvironmentVariableA), E(K, "GetStartupInfoA", GetStartupInfoA),
    E(K, "GetTickCount", GetTickCount), E(K, "GetTickCount64", GetTickCount64), E(K, "Sleep", Sleep),
    E(K, "QueryPerformanceFrequency", QueryPerformanceFrequency), E(K, "QueryPerformanceCounter", QueryPerformanceCounter),
    E(K, "VirtualAlloc", VirtualAlloc), E(K, "VirtualFree", VirtualFree), E(K, "VirtualProtect", VirtualProtect),
    E(K, "VirtualQuery", VirtualQuery), E(K, "GetProcessHeap", GetProcessHeap), E(K, "HeapCreate", HeapCreate),
    E(K, "HeapDestroy", HeapDestroy), E(K, "HeapAlloc", HeapAlloc), E(K, "HeapFree", HeapFree),
    E(K, "HeapReAlloc", HeapReAlloc), E(K, "HeapSize", HeapSize),
    E(K, "InitializeCriticalSection", InitializeCriticalSection),
    E(K, "InitializeCriticalSectionAndSpinCount", InitializeCriticalSectionAndSpinCount),
    E(K, "EnterCriticalSection", EnterCriticalSection), E(K, "LeaveCriticalSection", LeaveCriticalSection),
    E(K, "DeleteCriticalSection", DeleteCriticalSection),
    E(K, "TlsAlloc", TlsAlloc), E(K, "TlsFree", TlsFree), E(K, "TlsGetValue", TlsGetValue), E(K, "TlsSetValue", TlsSetValue),
    { 0, 0, 0 }
};

/* GetProcAddress/LoadLibrary resolve through the same table, so programs that bind dynamically also work. */
static void *lookup(const char *dll, const char *name) {
    int any_dll = !dll || !*dll;
    for (const shim_t *s = g_table; s->name; s++) {
        if (!ieq(s->name, name)) continue;
        if (any_dll || ieq(s->dll, dll)) return s->fn;
        /* api-ms-win-core-*, kernelbase and ntdll forward the same kernel32-level functions */
        if (starts_i(dll, "api-ms-win-core-") || ieq(dll, "kernelbase.dll") || ieq(dll, "ntdll.dll")) return s->fn;
    }
    return 0;
}
static MS void *GetProcAddress(HANDLE m, const char *name) { (void)m; return lookup(0, name); }
static MS HANDLE LoadLibraryA(const char *name) {
    if (ieq(name, K) || ieq(name, "kernel32") || ieq(name, "kernelbase.dll") || ieq(name, "ntdll.dll")) return (HANDLE)0x300;
    g_last_error = 126;
    return 0;
}
/* registered after the table so the table can stay const */
static const shim_t g_dyn[] = { E(K, "GetProcAddress", GetProcAddress), E(K, "LoadLibraryA", LoadLibraryA), { 0, 0, 0 } };

/* ============================================================== unresolved imports -> numbered, loud stubs */
#define MAX_STUBS 64
static char g_unres_dll[MAX_STUBS][64];
static char g_unres_name[MAX_STUBS][96];
static unsigned g_n_unres;
static MS void unimpl_dispatch(unsigned idx) {
    put("WINRUN: FATAL: unimplemented import ");
    put(g_unres_dll[idx]); put("!"); put(g_unres_name[idx]); put("\n");
    sc(SYS_EXIT, 127, 0, 0);
    for (;;) {}
}
#define STUB(i) static MS void stub_##i(void) { unimpl_dispatch(i); }
STUB(0) STUB(1) STUB(2) STUB(3) STUB(4) STUB(5) STUB(6) STUB(7) STUB(8) STUB(9) STUB(10) STUB(11) STUB(12) STUB(13) STUB(14) STUB(15)
STUB(16) STUB(17) STUB(18) STUB(19) STUB(20) STUB(21) STUB(22) STUB(23) STUB(24) STUB(25) STUB(26) STUB(27) STUB(28) STUB(29) STUB(30) STUB(31)
STUB(32) STUB(33) STUB(34) STUB(35) STUB(36) STUB(37) STUB(38) STUB(39) STUB(40) STUB(41) STUB(42) STUB(43) STUB(44) STUB(45) STUB(46) STUB(47)
STUB(48) STUB(49) STUB(50) STUB(51) STUB(52) STUB(53) STUB(54) STUB(55) STUB(56) STUB(57) STUB(58) STUB(59) STUB(60) STUB(61) STUB(62) STUB(63)
#define S(i) (void *)stub_##i
static void *const g_stubs[MAX_STUBS] = {
    S(0), S(1), S(2), S(3), S(4), S(5), S(6), S(7), S(8), S(9), S(10), S(11), S(12), S(13), S(14), S(15),
    S(16), S(17), S(18), S(19), S(20), S(21), S(22), S(23), S(24), S(25), S(26), S(27), S(28), S(29), S(30), S(31),
    S(32), S(33), S(34), S(35), S(36), S(37), S(38), S(39), S(40), S(41), S(42), S(43), S(44), S(45), S(46), S(47),
    S(48), S(49), S(50), S(51), S(52), S(53), S(54), S(55), S(56), S(57), S(58), S(59), S(60), S(61), S(62), S(63) };

/* pe_resolve_fn: 0 = resolved; nonzero = unresolved (the slot is pointed at a loud stub) */
int win_shim_resolve(const char *dll, const char *name, unsigned short ordinal, int by_ordinal, unsigned long long *out, void *user) {
    (void)ordinal; (void)user;
    void *f = by_ordinal ? 0 : lookup(dll, name);
    if (!f) for (const shim_t *s = g_dyn; s->name && !f; s++) if (ieq(s->name, name) && (ieq(s->dll, dll) || starts_i(dll, "api-ms-win-core-"))) f = s->fn;
    if (f) { *out = (u64)f; return 0; }
    unsigned i = g_n_unres < MAX_STUBS ? g_n_unres++ : MAX_STUBS - 1;
    unsigned k;
    for (k = 0; dll[k] && k < 63; k++) g_unres_dll[i][k] = dll[k];
    g_unres_dll[i][k] = 0;
    if (by_ordinal) { g_unres_name[i][0] = '#'; g_unres_name[i][1] = 0; }
    else { for (k = 0; name[k] && k < 95; k++) g_unres_name[i][k] = name[k]; g_unres_name[i][k] = 0; }
    *out = (u64)g_stubs[i];
    return 1;
}
unsigned win_shim_unresolved(void) { return g_n_unres; }
const char *win_shim_unresolved_dll(unsigned i) { return g_unres_dll[i]; }
const char *win_shim_unresolved_name(unsigned i) { return g_unres_name[i]; }
