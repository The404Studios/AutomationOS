/*
 * Kernel String Functions - BACKUP (DISABLED)
 * =======================
 *
 * DISABLED: Optimized implementations in string.c take precedence.
 * This backup file compiles to an empty translation unit to avoid
 * duplicate symbol errors.
 */

#ifdef USE_ORIGINAL_STRING_FUNCTIONS  /* Disabled - use string.c instead */

#include "../include/types.h"

/*
 * Fill memory with constant byte
 * dest: destination pointer
 * val: byte value to set
 * count: number of bytes
 * Returns: dest
 */
void* memset(void* dest, int val, size_t count) {
    uint8_t* d = (uint8_t*)dest;
    while (count--) {
        *d++ = (uint8_t)val;
    }
    return dest;
}

/*
 * Copy memory block (non-overlapping)
 * dest: destination pointer
 * src: source pointer
 * count: number of bytes
 * Returns: dest
 */
void* memcpy(void* dest, const void* src, size_t count) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (count--) {
        *d++ = *s++;
    }
    return dest;
}

/*
 * Copy memory block (handles overlapping regions)
 * dest: destination pointer
 * src: source pointer
 * count: number of bytes
 * Returns: dest
 */
void* memmove(void* dest, const void* src, size_t count) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;

    if (d < s) {
        while (count--) {
            *d++ = *s++;
        }
    } else {
        d += count;
        s += count;
        while (count--) {
            *--d = *--s;
        }
    }
    return dest;
}

/*
 * Compare memory blocks
 * s1: first memory block
 * s2: second memory block
 * count: number of bytes to compare
 * Returns: 0 if equal, <0 if s1<s2, >0 if s1>s2
 */
int memcmp(const void* s1, const void* s2, size_t count) {
    const uint8_t* a = (const uint8_t*)s1;
    const uint8_t* b = (const uint8_t*)s2;
    while (count--) {
        if (*a != *b) {
            return *a - *b;
        }
        a++;
        b++;
    }
    return 0;
}

/*
 * Calculate string length
 * str: null-terminated string
 * Returns: length excluding null terminator
 */
size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) {
        len++;
    }
    return len;
}

/*
 * Compare strings
 * s1: first string
 * s2: second string
 * Returns: 0 if equal, <0 if s1<s2, >0 if s1>s2
 */
int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const uint8_t*)s1 - *(const uint8_t*)s2;
}

/*
 * Compare strings up to n characters
 * s1: first string
 * s2: second string
 * n: maximum characters to compare
 * Returns: 0 if equal, <0 if s1<s2, >0 if s1>s2
 */
int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return *(const uint8_t*)s1 - *(const uint8_t*)s2;
}

/*
 * Copy string
 * dest: destination buffer
 * src: source string
 * Returns: dest
 */
char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

/*
 * Copy string up to n characters
 * dest: destination buffer
 * src: source string
 * n: maximum characters to copy
 * Returns: dest (pads with null bytes if src < n)
 */
char* strncpy(char* dest, const char* src, size_t n) {
    char* d = dest;
    while (n && (*d++ = *src++)) {
        n--;
    }
    while (n--) {
        *d++ = '\0';
    }
    return dest;
}

#endif /* USE_ORIGINAL_STRING_FUNCTIONS */
