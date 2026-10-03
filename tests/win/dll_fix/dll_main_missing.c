/* main whose only DLL does not exist: foo() must resolve to the host's loud stub, never crash the loader. */
__declspec(dllimport) int foo(int a);
unsigned start(void) { return (unsigned)foo(7); }

/* An absolute pointer in .data gives the image a DIR64 base relocation: without ANY relocation entry the loader
 * (rightly) refuses to rebase it, and the exe could only run at its preferred base. */
static int g_anchor_val;
int *volatile g_anchor = &g_anchor_val;
