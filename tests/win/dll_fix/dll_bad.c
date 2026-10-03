/* dll_bad -- DllMain returns FALSE on DLL_PROCESS_ATTACH.  Imports dll_lib_b so that b is attached first and
 * must be rolled back (detached) when bad's attach fails.  Logs 0xBD00 | reason. */
typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
__declspec(dllimport) void __stdcall SetLastError(DWORD code);
__declspec(dllimport) int sub(int a, int b);

int bad_fn(int a) { return sub(a, 1); }

BOOL __stdcall DllMain(HANDLE h, DWORD reason, void *res)
{
    (void)h; (void)res;
    SetLastError(0xBD00u | reason);
    return reason == 1 ? 0 : 1;
}

/* An absolute pointer in .data gives the image a DIR64 base relocation: without ANY relocation entry the loader
 * (rightly) refuses to rebase it, and the exe could only run at its preferred base. */
static int g_anchor_val;
int *volatile g_anchor = &g_anchor_val;
