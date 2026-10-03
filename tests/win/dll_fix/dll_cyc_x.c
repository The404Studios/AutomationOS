/* dll_cyc_x / dll_cyc_y -- an import CYCLE: x imports y, y imports x.  Logs 0x1000 | reason (x), 0x1100 | reason (y). */
typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
__declspec(dllimport) void __stdcall SetLastError(DWORD code);
__declspec(dllimport) int y_val(void);
int x_val(void) { return 1; }
int x_calls_y(void) { return y_val() + 100; }
BOOL __stdcall DllMain(HANDLE h, DWORD reason, void *res) { (void)h; (void)res; SetLastError(0x1000u | reason); return 1; }

/* An absolute pointer in .data gives the image a DIR64 base relocation: without ANY relocation entry the loader
 * (rightly) refuses to rebase it, and the exe could only run at its preferred base. */
static int g_anchor_val;
int *volatile g_anchor = &g_anchor_val;
