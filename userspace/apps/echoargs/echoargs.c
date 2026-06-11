/* echoargs -- AGENT-RPC-0 P6d proof tool. Prints its argv, ONE entry per line,
 * to fd1, so the agent (reading via the P6c stdout capability) can verify the
 * argv VECTOR arrived intact: a multi-word arg stays on one line, and shell
 * metacharacters are literal bytes (no shell, no whitespace split). crt0-linked
 * for a real argc/argv. */
#define SYS_WRITE 3

static unsigned slen(const char* s){ unsigned n=0; while(s&&s[n]) n++; return n; }
static void out(const char* s, unsigned n){
    long r;
    __asm__ volatile("syscall":"=a"(r):"a"((long)SYS_WRITE),"D"(1L),"S"((long)s),"d"((long)n):"rcx","r11","memory");
    (void)r;
}

int main(int argc, char** argv) {
    for (int i = 0; i < argc; i++) { out(argv[i], slen(argv[i])); out("\n", 1); }
    return 0;
}
