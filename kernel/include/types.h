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
typedef uint64_t uintptr_t;

// Boolean
typedef uint8_t bool;
#define true 1
#define false 0

// NULL
#define NULL ((void*)0)

// Useful macros
#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#endif
