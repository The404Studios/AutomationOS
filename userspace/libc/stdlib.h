#ifndef STDLIB_H
#define STDLIB_H

#ifndef NULL
#define NULL ((void*)0)
#endif

typedef unsigned long size_t;

// Memory allocation (implementation in malloc.c / included by stdlib.c)
void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* ptr, size_t size);
void free(void* ptr);

// In-process libc selftest: returns 0 on pass, negative on first failure.
int libc_selftest(void);

// String conversion
int atoi(const char* str);
long atol(const char* str);
long long atoll(const char* str);
double atof(const char* str);
long strtol(const char* str, char** endptr, int base);
unsigned long strtoul(const char* str, char** endptr, int base);
long long strtoll(const char* str, char** endptr, int base);
unsigned long long strtoull(const char* str, char** endptr, int base);
double strtod(const char* str, char** endptr);

// String utilities
char* strdup(const char* str);

// Sorting and searching
void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size,
              int (*compar)(const void*, const void*));

// Absolute value
int abs(int n);
long labs(long n);
long long llabs(long long n);

// Division
typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

div_t div(int numer, int denom);
ldiv_t ldiv(long numer, long denom);
lldiv_t lldiv(long long numer, long long denom);

// Random numbers (simple LCG)
#define RAND_MAX 32767
int rand(void);
void srand(unsigned int seed);

// Environment (backed by an in-process table; see stdlib.c)
extern char** environ;
char* getenv(const char* name);
int setenv(const char* name, const char* value, int overwrite);
int unsetenv(const char* name);
int putenv(char* string);

// Program termination
void exit(int status) __attribute__((noreturn));
void _Exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int atexit(void (*function)(void));

#endif
