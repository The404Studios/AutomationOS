__declspec(dllimport) int x_calls_y(void);
__declspec(dllimport) int y_calls_x(void);
unsigned start(void) { return (unsigned)((x_calls_y() == 102 ? 1 : 0) | (y_calls_x() == 201 ? 2 : 0)); }

/* An absolute pointer in .data gives the image a DIR64 base relocation: without ANY relocation entry the loader
 * (rightly) refuses to rebase it, and the exe could only run at its preferred base. */
static int g_anchor_val;
int *volatile g_anchor = &g_anchor_val;
