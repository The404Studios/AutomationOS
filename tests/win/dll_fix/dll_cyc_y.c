typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
__declspec(dllimport) void __stdcall SetLastError(DWORD code);
__declspec(dllimport) int x_val(void);
int y_val(void) { return 2; }
int y_calls_x(void) { return x_val() + 200; }
BOOL __stdcall DllMain(HANDLE h, DWORD reason, void *res) { (void)h; (void)res; SetLastError(0x1100u | reason); return 1; }

/* An absolute pointer in .data gives the image a DIR64 base relocation: without ANY relocation entry the loader
 * (rightly) refuses to rebase it, and the exe could only run at its preferred base. */
static int g_anchor_val;
int *volatile g_anchor = &g_anchor_val;
