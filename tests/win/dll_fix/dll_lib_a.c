/* dll_lib_a -- exports by name, an ordinal-only export (NONAME @7), two forwarders (one to dll_lib_b, one to a
 * "system" DLL), a function that calls through its own IAT into dll_lib_b, and a function that dereferences a
 * pointer stored in .data (proves the DIR64 relocation was applied at the actual load base).
 * DllMain logs 0xA00 | reason through KERNEL32!SetLastError.
 */
typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
__declspec(dllimport) void __stdcall SetLastError(DWORD code);
__declspec(dllimport) int sub(int a, int b);                       /* from dll_lib_b.dll */

int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }
int ordfn(int a) { return a * 3 + 1; }                              /* exported by ordinal 7 only */
int a_sub_twice(int a, int b) { return sub(sub(a, b), b); }        /* b's sub through a's IAT */

static int g_val = 77;
int *volatile g_ptr = &g_val;                                       /* absolute address in .data => DIR64 reloc */
int get_via_ptr(void) { return *g_ptr; }

BOOL __stdcall DllMain(HANDLE h, DWORD reason, void *res)
{
    (void)h; (void)res;
    SetLastError(0xA00u | reason);
    return 1;
}
