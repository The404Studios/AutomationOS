/* dll_main -- the exe-like main module: imports from dll_lib_a (by name, by ordinal, a forwarder) and kernel32.
 * start() returns a bitmask; 63 == everything linked and callable.
 */
typedef unsigned long DWORD;
__declspec(dllimport) void __stdcall SetLastError(DWORD code);
__declspec(dllimport) int add(int a, int b);
__declspec(dllimport) int mul(int a, int b);
__declspec(dllimport) int ordfn(int a);                             /* import by ordinal (NONAME) */
__declspec(dllimport) int fwd_add(int a, int b);                    /* forwarder -> dll_lib_b.sub */
__declspec(dllimport) int a_sub_twice(int a, int b);
__declspec(dllimport) int get_via_ptr(void);

unsigned start(void)
{
    unsigned r = 0;
    SetLastError(0xC00u | 0xFFu);                                   /* marks "main entry reached" in the log */
    if (add(2, 3) == 5) r |= 1;
    if (mul(4, 5) == 20) r |= 2;
    if (ordfn(5) == 16) r |= 4;
    if (fwd_add(10, 3) == 7) r |= 8;
    if (a_sub_twice(10, 2) == 6) r |= 16;
    if (get_via_ptr() == 77) r |= 32;
    return r;
}

/* An absolute pointer in .data gives the image a DIR64 base relocation: without ANY relocation entry the loader
 * (rightly) refuses to rebase it, and the exe could only run at its preferred base. */
static int g_anchor_val;
int *volatile g_anchor = &g_anchor_val;
