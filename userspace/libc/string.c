// userspace/libc/string.c - String utility functions

#include "string.h"

unsigned long strlen(const char* str) {
    unsigned long len = 0;
    while (str[len]) {
        len++;
    }
    return len;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, unsigned long n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* strncpy(char* dest, const char* src, unsigned long n) {
    char* d = dest;
    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    while (n--) {
        *d++ = '\0';
    }
    return dest;
}

char* strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char* strchr(const char* str, int ch) {
    char c = (char)ch;
    while (*str) {
        if (*str == c) {
            return (char*)str;
        }
        str++;
    }
    return (c == '\0') ? (char*)str : NULL;
}

void* memset(void* dest, int val, unsigned long count) {
    unsigned char* d = (unsigned char*)dest;
    while (count--) {
        *d++ = (unsigned char)val;
    }
    return dest;
}

void* memcpy(void* dest, const void* src, unsigned long count) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (count--) {
        *d++ = *s++;
    }
    return dest;
}

void* memmove(void* dest, const void* src, unsigned long count) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;

    if (d == s || count == 0) {
        return dest;
    }

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

int memcmp(const void* s1, const void* s2, unsigned long count) {
    const unsigned char* a = (const unsigned char*)s1;
    const unsigned char* b = (const unsigned char*)s2;
    while (count--) {
        if (*a != *b) {
            return *a - *b;
        }
        a++;
        b++;
    }
    return 0;
}

void* memchr(const void* s, int c, unsigned long n) {
    const unsigned char* p = (const unsigned char*)s;
    unsigned char ch = (unsigned char)c;
    while (n--) {
        if (*p == ch) {
            return (void*)p;
        }
        p++;
    }
    return NULL;
}

unsigned long strnlen(const char* str, unsigned long maxlen) {
    unsigned long len = 0;
    while (len < maxlen && str[len]) {
        len++;
    }
    return len;
}

static inline char lc(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int strcasecmp(const char* s1, const char* s2) {
    while (*s1 && lc(*s1) == lc(*s2)) {
        s1++;
        s2++;
    }
    return (unsigned char)lc(*s1) - (unsigned char)lc(*s2);
}

int strncasecmp(const char* s1, const char* s2, unsigned long n) {
    while (n && *s1 && lc(*s1) == lc(*s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (unsigned char)lc(*s1) - (unsigned char)lc(*s2);
}

char* strncat(char* dest, const char* src, unsigned long n) {
    char* d = dest;
    while (*d) {
        d++;
    }
    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    *d = '\0';
    return dest;
}

char* strrchr(const char* str, int ch) {
    char c = (char)ch;
    const char* last = NULL;
    do {
        if (*str == c) {
            last = str;
        }
    } while (*str++);
    return (char*)last;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) {
        return (char*)haystack;
    }
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n) {
            return (char*)haystack;
        }
    }
    return NULL;
}

unsigned long strspn(const char* str, const char* accept) {
    const char* s = str;
    while (*s) {
        const char* a = accept;
        while (*a && *a != *s) {
            a++;
        }
        if (!*a) {
            break;
        }
        s++;
    }
    return (unsigned long)(s - str);
}

unsigned long strcspn(const char* str, const char* reject) {
    const char* s = str;
    while (*s) {
        const char* r = reject;
        while (*r) {
            if (*r == *s) {
                return (unsigned long)(s - str);
            }
            r++;
        }
        s++;
    }
    return (unsigned long)(s - str);
}

char* strpbrk(const char* str, const char* accept) {
    for (; *str; str++) {
        const char* a = accept;
        while (*a) {
            if (*a == *str) {
                return (char*)str;
            }
            a++;
        }
    }
    return NULL;
}

char* strtok_r(char* str, const char* delim, char** saveptr) {
    char* s = str ? str : *saveptr;
    if (!s) {
        return NULL;
    }

    // Skip leading delimiters.
    s += strspn(s, delim);
    if (!*s) {
        *saveptr = s;
        return NULL;
    }

    // Find end of token.
    char* end = s + strcspn(s, delim);
    if (*end) {
        *end = '\0';
        *saveptr = end + 1;
    } else {
        *saveptr = end;
    }
    return s;
}

char* strtok(char* str, const char* delim) {
    static char* saveptr;
    return strtok_r(str, delim, &saveptr);
}
