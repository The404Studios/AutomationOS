/* echoproof -- deterministic stdout for the AGENT-RPC-0 P6c capability proof.
 * Writes EXACTLY the 17-byte marker "AGENT-RPC-0-PROOF" to fd1 and exits (no
 * args, no newline), so the agent -- after ACCEPTing the runner's one-shot
 * read-only stdout grant -- can byte-compare the exact output it read. */
#define SYS_WRITE 3

int main(void) {
    const char* m = "AGENT-RPC-0-PROOF";   /* 17 bytes */
    long r;
    __asm__ volatile("syscall":"=a"(r):"a"((long)SYS_WRITE),"D"(1L),"S"((long)m),"d"(17L):"rcx","r11","memory");
    (void)r;
    return 0;
}
