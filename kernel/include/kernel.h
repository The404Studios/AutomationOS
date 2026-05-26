#ifndef KERNEL_H
#define KERNEL_H

#include "types.h"

// Kernel version
#define KERNEL_VERSION_MAJOR 0
#define KERNEL_VERSION_MINOR 1
#define KERNEL_VERSION_PATCH 0

// Memory constants
#define PAGE_SIZE 4096
#define KERNEL_VIRTUAL_BASE 0xFFFFFFFF80000000ULL

// Compiler attributes
#define PACKED __attribute__((packed))
#define ALIGNED(x) __attribute__((aligned(x)))
#define NORETURN __attribute__((noreturn))
#define UNUSED __attribute__((unused))

// Panic and assertions
void kernel_panic(const char* message) NORETURN;
#define ASSERT(cond) do { if (!(cond)) kernel_panic("Assertion failed: " #cond); } while(0)

// Kernel printf
int kprintf(const char* format, ...);

#endif
