// userspace/libc/string.h - String function declarations

#ifndef STRING_H
#define STRING_H

#ifndef NULL
#define NULL ((void*)0)
#endif

// String functions
unsigned long strlen(const char* str);
unsigned long strnlen(const char* str, unsigned long maxlen);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, unsigned long n);
int strcasecmp(const char* s1, const char* s2);
int strncasecmp(const char* s1, const char* s2, unsigned long n);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, unsigned long n);
char* strcat(char* dest, const char* src);
char* strncat(char* dest, const char* src, unsigned long n);
char* strchr(const char* str, int ch);
char* strrchr(const char* str, int ch);
char* strstr(const char* haystack, const char* needle);
char* strpbrk(const char* str, const char* accept);
unsigned long strspn(const char* str, const char* accept);
unsigned long strcspn(const char* str, const char* reject);
char* strtok(char* str, const char* delim);
char* strtok_r(char* str, const char* delim, char** saveptr);
char* strerror(int errnum);
int strerror_r(int errnum, char* buf, unsigned long buflen);

// Memory functions
void* memset(void* dest, int val, unsigned long count);
void* memcpy(void* dest, const void* src, unsigned long count);
void* memmove(void* dest, const void* src, unsigned long count);
int memcmp(const void* s1, const void* s2, unsigned long count);
void* memchr(const void* s, int c, unsigned long n);

#endif
