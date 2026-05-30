#ifndef STRING_H
#define STRING_H

#include "types.h"

// Standard string functions (optimized implementations)
void* memcpy(void* dest, const void* src, size_t count);
void* memset(void* dest, int val, size_t count);
void* memmove(void* dest, const void* src, size_t count);
int memcmp(const void* s1, const void* s2, size_t count);

size_t strlen(const char* str);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);

// Additional string functions needed by kernel subsystems
char* strstr(const char* haystack, const char* needle);
char* strcat(char* dest, const char* src);
char* strncat(char* dest, const char* src, size_t n);
size_t strnlen(const char* str, size_t maxlen);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);

// Formatted string functions (kernel implementations)
int vsnprintf(char* buf, size_t size, const char* fmt, __builtin_va_list args);
int snprintf(char* buf, size_t size, const char* fmt, ...);
int ksnprintf(char* buf, size_t size, const char* fmt, ...);

// Performance benchmark (optional - call from kernel_main for testing)
void string_benchmark_run(void);

#endif
