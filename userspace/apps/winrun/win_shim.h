#ifndef WIN_SHIM_H
#define WIN_SHIM_H
/* The Windows API surface winrun exposes to a loaded PE image. See win_shim.c. */
void win_shim_init(unsigned long long image_base, const char *module_path, const char *cmdline);
/* pe_resolve_fn-compatible resolver (userspace/lib/pe/pe.h): 0 = resolved, nonzero = unresolved (loud stub). */
int win_shim_resolve(const char *dll, const char *name, unsigned short ordinal, int by_ordinal,
                     unsigned long long *out_addr, void *user);
unsigned win_shim_unresolved(void);
const char *win_shim_unresolved_dll(unsigned i);
const char *win_shim_unresolved_name(unsigned i);
#endif
