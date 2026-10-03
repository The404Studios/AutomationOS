/* dll_lib_b -- the leaf of the dependency graph (dll_lib_a imports from it, and forwards one export to it).
 *
 *   build: see tests/win/run_dll_tests.sh   (freestanding: no CRT, entry = DllMain, no stack protector)
 *
 * DllMain logs every call through KERNEL32!SetLastError (the test host's builtin shim records the argument):
 *   value = 0xB00 | reason     (reason: 1 attach, 0 detach)
 */
typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
__declspec(dllimport) void __stdcall SetLastError(DWORD code);

int sub(int a, int b) { return a - b; }
int b_id(void) { return 0xB; }

BOOL __stdcall DllMain(HANDLE h, DWORD reason, void *res)
{
    (void)h; (void)res;
    SetLastError(0xB00u | reason);
    return 1;
}

/* An absolute pointer in .data gives the image a DIR64 base relocation: without ANY relocation entry the loader
 * (rightly) refuses to rebase it, and the exe could only run at its preferred base. */
static int g_anchor_val;
int *volatile g_anchor = &g_anchor_val;
