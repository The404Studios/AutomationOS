#include "stdlib.h"
#include "string.h"

// ============================================================================
// HEAP ALLOCATOR
// ============================================================================
//
// The allocator lives in malloc.c (included below as a single compilation
// unit together with stdlib.c, so the existing Makefile needs no changes).
//
// Design summary:
//   Tier 1 — 8 MB static BSS arena (always available, no syscalls).
//   Tier 2 — 2 MB overflow chunks obtained on demand via SYS_MMAP=37.
//             Falls back to OOM if the kernel rejects the mmap request.
//   Both tiers share the same 32-byte block-header / first-fit / coalescing
//   free-list logic.  All payloads are 16-byte aligned.

#include "malloc.c"

int atoi(const char* str) {
    int sign = 1;
    int value = 0;

    if (!str) {
        return 0;
    }

    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') {
        str++;
    }

    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }

    while (*str >= '0' && *str <= '9') {
        value = value * 10 + (*str - '0');
        str++;
    }

    return sign * value;
}

char* strdup(const char* str) {
    if (!str) {
        return NULL;
    }

    size_t len = strlen(str) + 1;
    char* copy = malloc(len);
    if (!copy) {
        return NULL;
    }

    memcpy(copy, str, len);
    return copy;
}

// ============================================================================
// STRING CONVERSION FUNCTIONS
// ============================================================================

long atol(const char* str) {
    return strtol(str, NULL, 10);
}

long long atoll(const char* str) {
    return strtoll(str, NULL, 10);
}

// Convert string to double
double atof(const char* str) {
    if (!str) {
        return 0.0;
    }

    double result = 0.0;
    double sign = 1.0;
    int decimal_places = 0;

    // Skip whitespace
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') {
        str++;
    }

    // Handle sign
    if (*str == '-') {
        sign = -1.0;
        str++;
    } else if (*str == '+') {
        str++;
    }

    // Parse integer part
    while (*str >= '0' && *str <= '9') {
        result = result * 10.0 + (*str - '0');
        str++;
    }

    // Parse decimal part
    if (*str == '.') {
        str++;
        while (*str >= '0' && *str <= '9') {
            result = result * 10.0 + (*str - '0');
            decimal_places++;
            str++;
        }
    }

    // Adjust for decimal places
    while (decimal_places > 0) {
        result /= 10.0;
        decimal_places--;
    }

    return sign * result;
}

// Helper: check if character is valid for given base
static int is_valid_digit(char c, int base) {
    if (c >= '0' && c <= '9') {
        return (c - '0') < base;
    }
    if (base > 10) {
        if (c >= 'a' && c <= 'z') {
            return (c - 'a' + 10) < base;
        }
        if (c >= 'A' && c <= 'Z') {
            return (c - 'A' + 10) < base;
        }
    }
    return 0;
}

// Helper: convert character to digit value
static int char_to_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    return 0;
}

// Convert string to long with base
long strtol(const char* str, char** endptr, int base) {
    if (!str) {
        if (endptr) {
            *endptr = (char*)str;
        }
        return 0;
    }

    const char* p = str;
    long result = 0;
    int sign = 1;

    // Skip whitespace
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }

    // Handle sign
    if (*p == '-') {
        sign = -1;
        p++;
    } else if (*p == '+') {
        p++;
    }

    // Auto-detect base if base is 0
    if (base == 0) {
        if (*p == '0') {
            if (p[1] == 'x' || p[1] == 'X') {
                base = 16;
                p += 2;
            } else {
                base = 8;
                p++;
            }
        } else {
            base = 10;
        }
    } else if (base == 16) {
        // Skip 0x prefix if present
        if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
        }
    }

    // Check valid base
    if (base < 2 || base > 36) {
        if (endptr) {
            *endptr = (char*)str;
        }
        return 0;
    }

    // Parse digits
    int found_digit = 0;
    while (is_valid_digit(*p, base)) {
        result = result * base + char_to_digit(*p);
        p++;
        found_digit = 1;
    }

    if (endptr) {
        *endptr = (char*)(found_digit ? p : str);
    }

    return sign * result;
}

// Shared 64-bit unsigned parser used by strtoul/strtoull. Handles optional
// sign (a leading '-' negates the result modulo 2^64, matching C), whitespace,
// base auto-detection, and the 0x/0 prefixes.
static unsigned long long parse_unsigned(const char* str, char** endptr, int base) {
    if (!str) {
        if (endptr) {
            *endptr = (char*)str;
        }
        return 0;
    }

    const char* p = str;
    int negate = 0;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }

    if (*p == '-') {
        negate = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }

    if (base == 0) {
        if (*p == '0') {
            if (p[1] == 'x' || p[1] == 'X') {
                base = 16;
                p += 2;
            } else {
                base = 8;
                p++;
            }
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
        }
    }

    if (base < 2 || base > 36) {
        if (endptr) {
            *endptr = (char*)str;
        }
        return 0;
    }

    unsigned long long result = 0;
    int found_digit = 0;
    while (is_valid_digit(*p, base)) {
        result = result * (unsigned)base + (unsigned)char_to_digit(*p);
        p++;
        found_digit = 1;
    }

    if (endptr) {
        *endptr = (char*)(found_digit ? p : str);
    }

    return negate ? (unsigned long long)(-(long long)result) : result;
}

unsigned long strtoul(const char* str, char** endptr, int base) {
    return (unsigned long)parse_unsigned(str, endptr, base);
}

long long strtoll(const char* str, char** endptr, int base) {
    return (long long)strtol(str, endptr, base);
}

unsigned long long strtoull(const char* str, char** endptr, int base) {
    return parse_unsigned(str, endptr, base);
}

// Convert string to double (more complete than atof)
double strtod(const char* str, char** endptr) {
    if (!str) {
        if (endptr) {
            *endptr = (char*)str;
        }
        return 0.0;
    }

    const char* p = str;
    double result = 0.0;
    double sign = 1.0;
    int decimal_places = 0;

    // Skip whitespace
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }

    // Handle sign
    if (*p == '-') {
        sign = -1.0;
        p++;
    } else if (*p == '+') {
        p++;
    }

    // Parse integer part
    int found_digit = 0;
    while (*p >= '0' && *p <= '9') {
        result = result * 10.0 + (*p - '0');
        p++;
        found_digit = 1;
    }

    // Parse decimal part
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            result = result * 10.0 + (*p - '0');
            decimal_places++;
            p++;
            found_digit = 1;
        }
    }

    // Adjust for decimal places
    while (decimal_places > 0) {
        result /= 10.0;
        decimal_places--;
    }

    // Handle exponent (simplified - doesn't handle all cases)
    if (*p == 'e' || *p == 'E') {
        p++;
        int exp_sign = 1;
        int exponent = 0;

        if (*p == '-') {
            exp_sign = -1;
            p++;
        } else if (*p == '+') {
            p++;
        }

        while (*p >= '0' && *p <= '9') {
            exponent = exponent * 10 + (*p - '0');
            p++;
        }

        // Apply exponent
        if (exp_sign > 0) {
            for (int i = 0; i < exponent; i++) {
                result *= 10.0;
            }
        } else {
            for (int i = 0; i < exponent; i++) {
                result /= 10.0;
            }
        }
    }

    if (endptr) {
        *endptr = (char*)(found_digit ? p : str);
    }

    return sign * result;
}

// ============================================================================
// SORTING AND SEARCHING
// ============================================================================

// Helper for qsort: partition function
static void qsort_swap(char* a, char* b, size_t size) {
    for (size_t i = 0; i < size; i++) {
        char tmp = a[i];
        a[i] = b[i];
        b[i] = tmp;
    }
}

static void qsort_internal(void* base, size_t nmemb, size_t size,
                           int (*compar)(const void*, const void*)) {
    if (nmemb <= 1) {
        return;
    }

    char* arr = (char*)base;
    size_t pivot_idx = nmemb / 2;

    // Move pivot to end
    qsort_swap(arr + pivot_idx * size, arr + (nmemb - 1) * size, size);

    size_t store_idx = 0;
    for (size_t i = 0; i < nmemb - 1; i++) {
        if (compar(arr + i * size, arr + (nmemb - 1) * size) < 0) {
            if (i != store_idx) {
                qsort_swap(arr + i * size, arr + store_idx * size, size);
            }
            store_idx++;
        }
    }

    // Move pivot to its final position
    qsort_swap(arr + store_idx * size, arr + (nmemb - 1) * size, size);

    // Recursively sort left and right partitions
    qsort_internal(arr, store_idx, size, compar);
    if (store_idx + 1 < nmemb) {
        qsort_internal(arr + (store_idx + 1) * size, nmemb - store_idx - 1, size, compar);
    }
}

// Quicksort implementation
void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*)) {
    if (!base || !compar || size == 0 || nmemb <= 1) {
        return;
    }

    qsort_internal(base, nmemb, size, compar);
}

// Binary search implementation
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size,
              int (*compar)(const void*, const void*)) {
    if (!key || !base || !compar || size == 0 || nmemb == 0) {
        return NULL;
    }

    const char* arr = (const char*)base;
    size_t left = 0;
    size_t right = nmemb;

    while (left < right) {
        size_t mid = left + (right - left) / 2;
        const void* mid_elem = arr + mid * size;
        int cmp = compar(key, mid_elem);

        if (cmp == 0) {
            return (void*)mid_elem;
        } else if (cmp < 0) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }

    return NULL;
}

// ============================================================================
// ABSOLUTE VALUE AND DIVISION
// ============================================================================

int abs(int n) {
    return (n < 0) ? -n : n;
}

long labs(long n) {
    return (n < 0) ? -n : n;
}

long long llabs(long long n) {
    return (n < 0) ? -n : n;
}

div_t div(int numer, int denom) {
    div_t result;
    if (denom == 0) { result.quot = 0; result.rem = numer; return result; }
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}

ldiv_t ldiv(long numer, long denom) {
    ldiv_t result;
    if (denom == 0) { result.quot = 0; result.rem = numer; return result; }
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}

lldiv_t lldiv(long long numer, long long denom) {
    lldiv_t result;
    if (denom == 0) { result.quot = 0; result.rem = numer; return result; }
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}

// ============================================================================
// RANDOM NUMBERS (Simple Linear Congruential Generator)
// ============================================================================

static unsigned int rand_seed = 1;

int rand(void) {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (int)((rand_seed / 65536) % (RAND_MAX + 1));
}

void srand(unsigned int seed) {
    rand_seed = seed;
}

// ============================================================================
// ENVIRONMENT
// ============================================================================
//
// The kernel does not yet pass an environment block to new processes, so the
// environment is kept in an in-process table. `environ` points at a
// NULL-terminated array of "NAME=VALUE" strings. Entries created by setenv are
// heap-allocated and owned by libc; putenv stores the caller's pointer directly
// (per POSIX). The backing array is grown on the heap as needed.

#define ENV_INITIAL_CAP 16

static char** env_table = NULL;     // NULL-terminated; same storage as `environ`
static size_t env_count = 0;        // number of live entries (excl. terminator)
static size_t env_capacity = 0;     // slots allocated incl. terminator
static unsigned char env_owned[1024]; // 1 if libc malloc'd entry i (must free)

char** environ = NULL;

// Ensure the table can hold at least `need` entries plus a NULL terminator.
static int env_reserve(size_t need) {
    if (need + 1 <= env_capacity) {
        return 0;
    }
    size_t new_cap = env_capacity ? env_capacity * 2 : ENV_INITIAL_CAP;
    while (new_cap < need + 1) {
        new_cap *= 2;
    }
    char** nt = (char**)malloc(new_cap * sizeof(char*));
    if (!nt) {
        return -1;
    }
    for (size_t i = 0; i < env_count; i++) {
        nt[i] = env_table[i];
    }
    nt[env_count] = NULL;
    if (env_table) {
        free(env_table);
    }
    env_table = nt;
    env_capacity = new_cap;
    environ = env_table;
    return 0;
}

// Compare the "NAME" portion of entry against name (length namelen).
static int env_name_matches(const char* entry, const char* name, size_t namelen) {
    if (strncmp(entry, name, namelen) != 0) {
        return 0;
    }
    return entry[namelen] == '=';
}

static size_t env_find(const char* name, size_t namelen) {
    for (size_t i = 0; i < env_count; i++) {
        if (env_name_matches(env_table[i], name, namelen)) {
            return i;
        }
    }
    return (size_t)-1;
}

char* getenv(const char* name) {
    if (!name || !env_table) {
        return NULL;
    }
    size_t namelen = strlen(name);
    size_t idx = env_find(name, namelen);
    if (idx == (size_t)-1) {
        return NULL;
    }
    return env_table[idx] + namelen + 1;  // skip "NAME="
}

int setenv(const char* name, const char* value, int overwrite) {
    if (!name || !*name) {
        return -1;
    }
    // Names may not contain '='.
    for (const char* p = name; *p; p++) {
        if (*p == '=') {
            return -1;
        }
    }
    if (!value) {
        value = "";
    }

    size_t namelen = strlen(name);
    size_t vallen = strlen(value);
    size_t idx = env_find(name, namelen);

    if (idx != (size_t)-1 && !overwrite) {
        return 0;  // exists, caller asked not to overwrite
    }

    char* entry = (char*)malloc(namelen + 1 + vallen + 1);
    if (!entry) {
        return -1;
    }
    memcpy(entry, name, namelen);
    entry[namelen] = '=';
    memcpy(entry + namelen + 1, value, vallen);
    entry[namelen + 1 + vallen] = '\0';

    if (idx != (size_t)-1) {
        if (idx < sizeof(env_owned) && env_owned[idx]) {
            free(env_table[idx]);
        }
        env_table[idx] = entry;
        if (idx < sizeof(env_owned)) {
            env_owned[idx] = 1;
        }
        return 0;
    }

    if (env_reserve(env_count + 1) != 0) {
        free(entry);
        return -1;
    }
    env_table[env_count] = entry;
    if (env_count < sizeof(env_owned)) {
        env_owned[env_count] = 1;
    }
    env_count++;
    env_table[env_count] = NULL;
    environ = env_table;
    return 0;
}

int unsetenv(const char* name) {
    if (!name || !*name || !env_table) {
        return -1;
    }
    size_t namelen = strlen(name);
    size_t idx = env_find(name, namelen);
    if (idx == (size_t)-1) {
        return 0;  // not present is success per POSIX
    }
    if (idx < sizeof(env_owned) && env_owned[idx]) {
        free(env_table[idx]);
    }
    // Shift remaining entries (and their ownership flags) down by one.
    for (size_t i = idx; i < env_count - 1; i++) {
        env_table[i] = env_table[i + 1];
        if (i + 1 < sizeof(env_owned)) {
            env_owned[i] = (i < sizeof(env_owned)) ? env_owned[i + 1] : 0;
        }
    }
    env_count--;
    env_table[env_count] = NULL;
    return 0;
}

// putenv stores the caller-supplied "NAME=VALUE" pointer directly (not copied).
int putenv(char* string) {
    if (!string) {
        return -1;
    }
    const char* eq = string;
    while (*eq && *eq != '=') {
        eq++;
    }
    if (*eq != '=') {
        return -1;  // must contain '='
    }
    size_t namelen = (size_t)(eq - string);
    size_t idx = env_find(string, namelen);

    if (idx != (size_t)-1) {
        if (idx < sizeof(env_owned) && env_owned[idx]) {
            free(env_table[idx]);
            env_owned[idx] = 0;
        }
        env_table[idx] = string;
        return 0;
    }

    if (env_reserve(env_count + 1) != 0) {
        return -1;
    }
    env_table[env_count] = string;
    if (env_count < sizeof(env_owned)) {
        env_owned[env_count] = 0;  // not libc-owned
    }
    env_count++;
    env_table[env_count] = NULL;
    environ = env_table;
    return 0;
}

// ============================================================================
// PROGRAM TERMINATION
// ============================================================================

// Declared in syscall.h, implemented in syscall.c
// void exit(int status) __attribute__((noreturn));

void _Exit(int status) {
    exit(status);
}

void abort(void) {
    exit(134);  // 128 + SIGABRT (6)
}

// Simple atexit support (limited to 32 functions)
#define MAX_ATEXIT 32
static void (*atexit_funcs[MAX_ATEXIT])(void);
static int atexit_count = 0;

int atexit(void (*function)(void)) {
    if (!function || atexit_count >= MAX_ATEXIT) {
        return -1;
    }

    atexit_funcs[atexit_count++] = function;
    return 0;
}

// This should be called by exit() before terminating
void __call_atexit_handlers(void) {
    for (int i = atexit_count - 1; i >= 0; i--) {
        if (atexit_funcs[i]) {
            atexit_funcs[i]();
        }
    }
}
