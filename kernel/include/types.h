#ifndef TYPES_H
#define TYPES_H

// Basic integer types
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef signed long long int64_t;

// Size types
typedef uint64_t size_t;
typedef int64_t ssize_t;
typedef int64_t off_t;
typedef uint64_t uintptr_t;

// Boolean
typedef uint8_t bool;
#define true 1
#define false 0

// NULL
#define NULL ((void*)0)

// Limits
#define UINT64_MAX 0xFFFFFFFFFFFFFFFFULL
#define INT64_MAX  0x7FFFFFFFFFFFFFFFLL
#define INT64_MIN  (-INT64_MAX - 1)
#define UINT32_MAX 0xFFFFFFFFU
#define INT32_MAX  0x7FFFFFFF
#define INT32_MIN  (-INT32_MAX - 1)

// Useful macros
#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// stdint.h / stddef.h compatibility for kernel code
// These typedefs are already provided above but some files use
// #include <stdint.h> or <stddef.h> which don't exist in freestanding.
// We provide a typedef for intptr_t here as well.
typedef int64_t intptr_t;
typedef int64_t ptrdiff_t;

#endif
