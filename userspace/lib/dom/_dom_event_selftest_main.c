/*
 * _dom_event_selftest_main.c -- hosted selftest runner (system libc).
 * This file is NOT part of the freestanding build; it exists only for
 * developer validation under a normal Linux toolchain.
 */
#include <stdio.h>

extern int dom_selftest(void);
extern int dom_event_selftest(void);

int main(void)
{
    int r1 = dom_selftest();
    printf("dom_selftest       : %s (ret=%d)\n", r1 == 0 ? "PASS" : "FAIL", r1);

    int r2 = dom_event_selftest();
    /* dom_event_selftest also calls ev_write() directly via syscall,
       so output appears before this printf. */
    printf("dom_event_selftest : %s (ret=%d)\n", r2 == 0 ? "PASS" : "FAIL", r2);

    return (r1 == 0 && r2 == 0) ? 0 : 1;
}
