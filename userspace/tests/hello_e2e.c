/*
 * hello_e2e.c -- the /bin/hello.elf target for the FS end-to-end ELF-loader
 * proof. A minimal freestanding ring-3 program: write a distinctive marker to
 * fd 1 (serial/console) and exit 0. crt0 provides _start and calls main();
 * no libc, no headers. Packaged (E2E build only) as BOTH /bin/hello.elf and
 * /bin/hello, so the direct-spawn ELF test AND the shell test (tool_shell
 * resolves the bare name "hello" -> bin/hello) can each run it.
 */
#define SYS_WRITE 3

static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static unsigned slen(const char* s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void out(const char* s) { sc(SYS_WRITE, 1, (long)s, (long)slen(s)); }

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    out("E2E-HELLO: hello from /bin/hello.elf -- ring-3 ELF loaded and running\n");
    return 0;
}
