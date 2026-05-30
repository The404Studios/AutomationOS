// M1 platform smoke test (runs in ring 3 as a normal process).
// Exercises the new graphics-platform syscalls: mmap, fb_acquire, get_ticks_ms.
// Output goes to serial via SYS_WRITE(fd=1). Proof points:
//   - mmap write/read-back OK  -> anonymous mmap works
//   - fb_acquire returns 0 + correct geometry, and the full-screen fill loop
//     completes without faulting -> framebuffer is mapped WRITABLE into this
//     process (validates the per-process page-table-targeting fix)
//   - ticks advance -> monotonic clock works

typedef unsigned long       size_t;
typedef unsigned long long  uint64_t;
typedef unsigned int        uint32_t;

#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_MMAP          37
#define SYS_FB_ACQUIRE    39
#define SYS_GET_TICKS_MS  40

#define VMM_PROT_READ   0x01
#define VMM_PROT_WRITE  0x02

static inline long syscall(long n, long a1, long a2, long a3) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return ret;
}

static size_t strlen(const char* s){ size_t l=0; while(s[l]) l++; return l; }
static void print(const char* m){ syscall(SYS_WRITE, 1, (long)m, (long)strlen(m)); }
static void print_num(long n){
    char b[24]; int i=0;
    if (n < 0){ print("-"); n = -n; }
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0){ char c = b[--i]; syscall(SYS_WRITE, 1, (long)&c, 1); }
}

typedef struct { uint64_t vaddr; uint32_t width, height, pitch, bpp; } fb_acquire_t;

void _start(void) {
    print("\n[TEST_M1] ===== userspace graphics-platform test =====\n");

    long t0 = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
    print("[TEST_M1] clock t0(ms)="); print_num(t0); print("\n");

    // --- anonymous mmap (3 MB) ---
    long p = syscall(SYS_MMAP, 0, 0x300000, VMM_PROT_READ | VMM_PROT_WRITE);
    print("[TEST_M1] mmap(3MB) -> "); print_num(p); print("\n");
    if (p > 0) {
        volatile uint64_t* mp = (volatile uint64_t*)p;
        mp[0] = 0xCAFEBABEDEADBEEFULL;
        mp[100000] = 0x1234567890ABCDEFULL;   // ~780KB in, forces multi-page
        if (mp[0] == 0xCAFEBABEDEADBEEFULL && mp[100000] == 0x1234567890ABCDEFULL)
            print("[TEST_M1] mmap write/read-back OK\n");
        else
            print("[TEST_M1] mmap MISMATCH\n");
    } else {
        print("[TEST_M1] mmap FAILED\n");
    }

    // --- framebuffer acquire + fill ---
    fb_acquire_t fb;
    long r = syscall(SYS_FB_ACQUIRE, (long)&fb, 0, 0);
    if (r == 0) {
        print("[TEST_M1] fb_acquire OK  w="); print_num(fb.width);
        print(" h="); print_num(fb.height);
        print(" pitch="); print_num(fb.pitch);
        print(" bpp="); print_num(fb.bpp);
        print(" vaddr="); print_num((long)fb.vaddr); print("\n");

        volatile uint32_t* px = (volatile uint32_t*)fb.vaddr;
        uint32_t stride = fb.pitch / 4;
        // Solid teal background.
        for (uint32_t y = 0; y < fb.height; y++)
            for (uint32_t x = 0; x < fb.width; x++)
                px[y * stride + x] = 0x00208080u;
        // White diagonal so it's obviously "ours".
        for (uint32_t y = 0; y < fb.height; y++) {
            uint32_t x = (y * fb.width) / fb.height;
            px[y * stride + x] = 0x00FFFFFFu;
        }
        print("[TEST_M1] framebuffer fill complete (teal + white diagonal)\n");
    } else {
        print("[TEST_M1] fb_acquire FAILED r="); print_num(r); print("\n");
    }

    long t1 = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
    print("[TEST_M1] clock t1(ms)="); print_num(t1);
    print(" elapsed="); print_num(t1 - t0); print("\n");
    print("[TEST_M1] ===== DONE (all syscalls returned) =====\n");

    for (;;) syscall(SYS_YIELD, 0, 0, 0);
}
