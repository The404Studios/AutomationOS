/*
 * hello.c -- write a line of text, then exit cleanly.
 *
 * sys_write(fd, buf, len) is a toolchain builtin that emits a
 * SYS_WRITE syscall; the string literal is placed in .data and
 * goes to the kernel console/serial. Exits with code 0.
 */
int main(void) {
    sys_write(1, "hello from a natively-compiled program!\n", 40);
    return 0;
}
