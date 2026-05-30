/* Freestanding kernel compatibility shim for <stddef.h> */
#ifndef _KERNEL_COMPAT_STDDEF_H
#define _KERNEL_COMPAT_STDDEF_H

#include "../types.h"

#ifndef offsetof
#define offsetof(type, member) ((size_t)&(((type *)0)->member))
#endif

#endif
