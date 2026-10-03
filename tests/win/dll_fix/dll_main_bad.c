/* main for the failing-DllMain scenario: imports bad_fn from dll_bad. */
__declspec(dllimport) int bad_fn(int a);
unsigned start(void) { return (unsigned)bad_fn(10); }

/* An absolute pointer in .data gives the image a DIR64 base relocation: without ANY relocation entry the loader
 * (rightly) refuses to rebase it, and the exe could only run at its preferred base. */
static int g_anchor_val;
int *volatile g_anchor = &g_anchor_val;
