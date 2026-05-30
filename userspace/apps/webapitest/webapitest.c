/*
 * webapitest.c -- Boot KAT for the five JS Web API libraries.
 * ============================================================
 *
 * Calls the self-test entry point of each web API library and reports
 * a single PASS/FAIL line so the smoke harness can catch regressions.
 *
 * Libraries under test (each has a *_selftest() that returns 0 = pass):
 *   js_timers_selftest()   -- setTimeout/setInterval/clearTimeout/clearInterval
 *   js_fetch_selftest()    -- fetch() + XMLHttpRequest (offline URL/stub test)
 *   js_storage_selftest()  -- localStorage / sessionStorage key-value store
 *   js_console_selftest()  -- full console.* surface
 *   js_url_selftest()      -- URL parser + encodeURIComponent / decodeURIComponent
 *
 * Freestanding ring-3.  No standard libc.  crt0 (start.asm) calls main().
 *
 * Build flags (NO fs:0x28 canary -- verified by objdump):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/webapitest/webapitest.c -o webapitest.o
 *
 * Output (fd 1 / SYS_WRITE):
 *   WEBAPITEST: PASS
 *   -- or --
 *   WEBAPITEST: FAIL <which>
 *
 * Binary name:   webapitest
 * Boot marker:   WEBAPITEST: PASS
 * Spawned as:    sbin/webapitest
 *
 * SYS_EXIT(0) in all cases so the smoke harness never stalls.
 */

/* Pull in strlen (and friends) from the freestanding libc.
 * The JS headers only include js.h (an opaque handle); they do not
 * transitively bring in string.h, so we include it explicitly here. */
#include "../../libc/string.h"        /* strlen */

#include "../../lib/js/js_timers.h"   /* js_timers_selftest() */
#include "../../lib/js/js_fetch.h"    /* js_fetch_selftest()  */
#include "../../lib/js/js_storage.h"  /* js_storage_selftest()*/
#include "../../lib/js/js_console.h"  /* js_console_selftest()*/
#include "../../lib/js/js_url.h"      /* js_url_selftest()    */

/* =========================================================================
 * Syscall helpers (6-argument SysV x86-64 form, matching domtest/webtest).
 * ========================================================================= */
#define SYS_WRITE  3
#define SYS_EXIT   0

static inline long sc(long n, long a1, long a2, long a3,
                      long a4, long a5, long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* =========================================================================
 * Minimal output helpers (no stdio).
 * strlen is available transitively through js.h -> libc/string.h.
 * ========================================================================= */
static void put(const char *s)
{
    unsigned long n = strlen(s);
    sc(SYS_WRITE, 1, (long)s, (long)n, 0, 0, 0);
}

/* =========================================================================
 * Failure tracking: record first failure label; continue running others
 * so a single run surfaces all broken selftests rather than stopping early.
 * ========================================================================= */
static int         g_failed   = 0;
static const char *g_fail_name = (const char *)0;

static void fail(const char *label)
{
    if (!g_failed) {
        g_fail_name = label;
        g_failed    = 1;
    }
}

/* =========================================================================
 * Entry point (called by crt0 / start.asm).
 * ========================================================================= */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * Run each library's self-test.  We intentionally do NOT short-circuit
     * on first failure so that all five get exercised and any additional
     * failures appear in subsequent runs / logs.  Only the first failure
     * name is reported in the FAIL line (per contract with the harness).
     */

    if (js_timers_selftest()  != 0) { fail("js_timers_selftest");  }
    if (js_fetch_selftest()   != 0) { fail("js_fetch_selftest");   }
    if (js_storage_selftest() != 0) { fail("js_storage_selftest"); }
    if (js_console_selftest() != 0) { fail("js_console_selftest"); }
    if (js_url_selftest()     != 0) { fail("js_url_selftest");     }

    if (!g_failed) {
        put("WEBAPITEST: PASS\n");
    } else {
        put("WEBAPITEST: FAIL ");
        put(g_fail_name);
        put("\n");
    }

    /* crt0 calls SYS_EXIT(return_value).  Always return 0 so the smoke
     * harness does not stall on a non-zero exit code. */
    return 0;
}
