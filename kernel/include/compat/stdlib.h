/* Freestanding kernel compatibility shim for <stdlib.h> */
#ifndef _KERNEL_COMPAT_STDLIB_H
#define _KERNEL_COMPAT_STDLIB_H

#include "../types.h"
#include "../mem.h"

/* Map malloc/free to kernel heap allocator */
#ifndef malloc
#define malloc(size) kmalloc(size)
#endif

#ifndef free
#define free(ptr) kfree(ptr)
#endif

#ifndef calloc
static inline void* calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void* ptr = kmalloc(total);
    if (ptr) {
        /* Use inline memset to avoid circular dependency */
        unsigned char* p = (unsigned char*)ptr;
        for (size_t i = 0; i < total; i++) p[i] = 0;
    }
    return ptr;
}
#endif

#ifndef realloc
static inline void* realloc(void* ptr, size_t size) {
    /* Simple realloc: allocate new, copy, free old */
    /* NOTE: This is a rough approximation - real realloc needs old size */
    void* new_ptr = kmalloc(size);
    if (new_ptr && ptr) {
        /* Cannot know old size, caller must handle data copy */
        /* This stub just allocates - full impl would need size tracking */
    }
    if (ptr) kfree(ptr);
    return new_ptr;
}
#endif

/* abs */
#ifndef abs
static inline int abs(int x) { return x < 0 ? -x : x; }
#endif

/* atoi - convert string to integer */
#ifndef atoi
static inline int atoi(const char* str) {
    int result = 0;
    int sign = 1;
    while (*str == ' ') str++;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') { str++; }
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return sign * result;
}
#endif

/* strdup - duplicate a string using kernel allocator */
static inline char* strdup(const char* s) {
    if (!s) return (char*)0;
    size_t len = 0;
    while (s[len]) len++;
    char* dup = (char*)kmalloc(len + 1);
    if (dup) {
        for (size_t i = 0; i <= len; i++) dup[i] = s[i];
    }
    return dup;
}

#endif
