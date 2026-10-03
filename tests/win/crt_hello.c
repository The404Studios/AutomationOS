/* WINFIX crt_hello -- an ORDINARY MinGW program (the C runtime is linked in). Tier-2 target:
 * it needs the CRT's own imports (msvcrt + a slice of kernel32) on top of the tier-1 surface.
 *
 *   x86_64-w64-mingw32-gcc -O2 -o crt_hello.exe crt_hello.c
 *
 * Expected output: "WINFIX: crt hello 7 argc=1" and exit code 3.
 */
#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argv;
    printf("WINFIX: crt hello %d argc=%d\n", 7, argc);
    return 3;
}
