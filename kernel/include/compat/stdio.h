/* Freestanding kernel compatibility shim for <stdio.h> */
#ifndef _KERNEL_COMPAT_STDIO_H
#define _KERNEL_COMPAT_STDIO_H

#include "../types.h"
#include "../kernel.h"
#include "../string.h"

/* Map printf to kprintf in kernel context */
#ifndef printf
#define printf kprintf
#endif

/* snprintf and vsnprintf are implemented in lib/string.c
 * and declared in string.h -- no stub needed */

#ifndef fprintf
#define fprintf(stream, ...) kprintf(__VA_ARGS__)
#endif

/* FILE type stub (for code that references stderr etc.) */
typedef void FILE;
#ifndef stderr
#define stderr ((FILE*)0)
#endif
#ifndef stdout
#define stdout ((FILE*)0)
#endif

#endif
