# AutomationOS Coding Standards

**Version:** 1.0  
**Last Updated:** 2026-05-26

This document defines the official coding standards for AutomationOS. All code contributions must adhere to these guidelines to maintain consistency, readability, and quality across the codebase.

---

## Table of Contents

1. [File Organization](#file-organization)
2. [Naming Conventions](#naming-conventions)
3. [Formatting](#formatting)
4. [Comments and Documentation](#comments-and-documentation)
5. [Language Features](#language-features)
6. [Error Handling](#error-handling)
7. [Memory Management](#memory-management)
8. [Architecture-Specific Code](#architecture-specific-code)
9. [Testing](#testing)
10. [Tools and Automation](#tools-and-automation)

---

## 1. File Organization

### 1.1 File Structure

Every C source file should follow this structure:

```c
/*
 * Module Name - Brief Description
 * ================================
 *
 * Detailed description of the module's purpose,
 * key algorithms, or important implementation notes.
 */

#include "local_headers.h"   // Local project headers first
#include <system_headers.h>  // System headers second (if any)

// Constants and macros
#define CONSTANT_NAME 42

// Type definitions
typedef struct {
    int field;
} my_struct_t;

// Static (private) function declarations
static void private_function(void);

// Static (private) variables
static int module_state = 0;

// Public function implementations
void public_function(void) {
    // Implementation
}

// Static (private) function implementations
static void private_function(void) {
    // Implementation
}
```

### 1.2 Header Files

Header files must use include guards:

```c
#ifndef MODULE_NAME_H
#define MODULE_NAME_H

// Declarations

#endif  // MODULE_NAME_H
```

**Rules:**
- Include guards must be `ALL_CAPS` based on the file path
- Example: `kernel/include/mem.h` → `MEM_H`
- Always include a trailing comment: `#endif  // MODULE_NAME_H`

### 1.3 Include Order

Headers must be included in this order:

1. Local project headers (relative paths)
2. System/library headers (angle brackets)
3. Blank line between groups

```c
#include "../../include/kernel.h"
#include "../../include/mem.h"

#include <stdint.h>
#include <stdbool.h>
```

---

## 2. Naming Conventions

### 2.1 General Rules

| Element | Convention | Example |
|---------|-----------|---------|
| Functions | `lowercase_snake_case` | `pmm_alloc_page()` |
| Variables | `lowercase_snake_case` | `ready_queue_head` |
| Constants | `UPPERCASE_SNAKE_CASE` | `DEFAULT_TIME_SLICE` |
| Macros | `UPPERCASE_SNAKE_CASE` | `PAGE_SIZE` |
| Types | `lowercase_snake_case_t` | `process_t`, `boot_info_t` |
| Enums | `UPPERCASE_SNAKE_CASE` | `enum { STATE_READY, STATE_RUNNING }` |
| Struct members | `lowercase_snake_case` | `struct { int ref_count; }` |

### 2.2 Naming Patterns

**Module Prefixes:**
Functions should be prefixed with their module name:
```c
// Memory management
void pmm_init(void);
void* pmm_alloc_page(void);
void pmm_free_page(void* page);

// Scheduler
void scheduler_init(void);
void scheduler_add_process(process_t* proc);
```

**Private (Static) Functions:**
Static functions may omit the module prefix if they're clearly internal:
```c
static void init_free_lists(void);  // Internal to pmm.c
static process_t* dequeue_next(void);  // Internal to scheduler.c
```

### 2.3 Avoid Hungarian Notation

**Don't:**
```c
int iCount;
uint32_t u32Value;
char* pszName;
```

**Do:**
```c
int count;
uint32_t value;
char* name;
```

Exception: Type suffixes like `_t` are encouraged for clarity.

---

## 3. Formatting

### 3.1 Indentation

- **Use TABS for indentation** (not spaces)
- Tab width: 8 spaces (for display)
- Rationale: Follows Linux kernel style; allows developers to choose their preferred visual width

### 3.2 Line Length

- **Maximum line length: 100 characters**
- Break long lines at logical points
- Indent continuation lines by one additional tab

```c
// Good
int result = very_long_function_name(argument1, argument2,
				     argument3, argument4);

// Also acceptable
int result = very_long_function_name(
	argument1,
	argument2,
	argument3,
	argument4
);
```

### 3.3 Braces

**K&R Style:**
- Opening brace on same line for functions
- Opening brace on same line for control structures
- Closing brace on its own line

```c
void function(void)
{
	if (condition) {
		// Code
	} else {
		// Code
	}

	while (condition) {
		// Code
	}

	for (int i = 0; i < count; i++) {
		// Code
	}
}
```

**Exception:** Single-line blocks may omit braces for simple statements:
```c
if (ptr == NULL)
	return;

for (int i = 0; i < 10; i++)
	array[i] = 0;
```

### 3.4 Spacing

**Binary operators:**
```c
// Good
int result = a + b * c;
if (x == 10 && y != 20)
	return;

// Bad
int result=a+b*c;
if(x==10&&y!=20)
	return;
```

**Unary operators:**
```c
// Good
ptr = &variable;
value = *ptr;
count++;

// Bad
ptr = & variable;
value = * ptr;
```

**Function calls:**
```c
// Good
result = function(arg1, arg2);

// Bad
result = function (arg1,arg2);
```

**Pointer declarations:**
```c
// Good
int* ptr;
char* string;

// Acceptable (consistent within file)
int *ptr;
char *string;
```

### 3.5 Control Structures

Always use braces for multi-line bodies:

```c
// Good
if (condition) {
	statement1;
	statement2;
}

// Bad (prone to errors)
if (condition)
	statement1;
	statement2;  // NOT in the if block!
```

**Switch statements:**
```c
switch (value) {
case 0:
	// Fallthrough
case 1:
	do_something();
	break;
case 2:
	do_another_thing();
	break;
default:
	handle_error();
	break;
}
```

---

## 4. Comments and Documentation

### 4.1 File Headers

Every source file should start with a block comment:

```c
/*
 * Physical Memory Manager (Buddy Allocator)
 * =========================================
 *
 * Implements a buddy allocator for efficient page allocation.
 * Supports orders 0-10 (4KB to 4MB pages).
 *
 * Key algorithms:
 *  - O(1) allocation from free lists
 *  - Buddy coalescing on free
 *  - Bitmap tracking for allocations
 */
```

### 4.2 Function Comments

Public functions should have documentation comments:

```c
/*
 * Allocate a physical page
 *
 * Returns: Physical address of allocated page, or NULL on failure
 *
 * Note: This function panics if no memory is available.
 *       Consider adding proper error handling in production.
 */
void* pmm_alloc_page(void)
{
	// Implementation
}
```

### 4.3 Inline Comments

Use inline comments to explain **why**, not **what**:

```c
// Good
// CRITICAL: Do NOT reset time_slice here!
// This function is called when process is preempted.
// We must preserve the time_slice value to maintain fairness.
proc->state = PROCESS_READY;

// Bad
// Set process state to ready
proc->state = PROCESS_READY;
```

### 4.4 TODO/FIXME/XXX Tags

Use standard tags for tracking issues:

```c
// TODO: Implement SMP support
// FIXME: Race condition in scheduler_remove_process()
// XXX: This is a temporary hack, remove before production
```

### 4.5 Multi-line Comments

Use `/* */` for multi-line, `//` for single-line:

```c
/*
 * Multi-line comment explaining
 * complex algorithm or design decision
 */

// Single-line comment for simple explanation
```

---

## 5. Language Features

### 5.1 Types

**Use standard integer types:**
```c
// Good
uint32_t count;
int64_t offset;
size_t length;

// Bad
unsigned int count;
long long offset;
unsigned long length;  // Platform-dependent size!
```

**Boolean type:**
```c
#include <stdbool.h>

bool is_ready = true;
if (is_ready) {
	// ...
}
```

### 5.2 Constants

Prefer `#define` for compile-time constants:
```c
#define PAGE_SIZE 4096
#define MAX_PROCESSES 256
```

Use `const` for runtime constants:
```c
const uint32_t cpu_count = detect_cpus();
```

### 5.3 Function Declarations

**Always specify parameter types:**
```c
// Good
void function(void);
int process(int value, char* string);

// Bad
void function();
int process();
```

### 5.4 Type Casting

**Be explicit with casts:**
```c
// Good
uint32_t value = (uint32_t)some_expression;
void* ptr = (void*)0x1000;

// Bad (implicit)
uint32_t value = some_expression;
void* ptr = 0x1000;
```

---

## 6. Error Handling

### 6.1 Return Codes

Functions should return meaningful error codes:

```c
// Return 0 on success, negative on error
int function(void)
{
	if (error_condition)
		return -1;  // Or specific error code
	return 0;
}
```

### 6.2 NULL Checks

**Always check pointers before dereferencing:**

```c
void function(process_t* proc)
{
	if (proc == NULL) {
		kprintf("Error: NULL process\n");
		return;
	}

	// Safe to use proc now
	proc->state = PROCESS_READY;
}
```

### 6.3 Assertions

Use assertions for sanity checks in debug builds:

```c
#include "kernel.h"

void function(int* ptr)
{
	ASSERT(ptr != NULL);
	ASSERT(*ptr >= 0);

	// Implementation
}
```

### 6.4 Panic Conditions

Use `kernel_panic()` only for unrecoverable errors:

```c
if (critical_failure) {
	kernel_panic("Critical failure: cannot continue");
}
```

---

## 7. Memory Management

### 7.1 Allocation

```c
// Always check allocation results
void* buffer = kmalloc(size);
if (buffer == NULL) {
	kprintf("Error: allocation failed\n");
	return -1;
}
```

### 7.2 Cleanup

```c
// Free in reverse order of allocation
void* buf1 = kmalloc(SIZE1);
void* buf2 = kmalloc(SIZE2);

// Later...
kfree(buf2);
kfree(buf1);
```

### 7.3 Reference Counting

```c
// Take reference when storing pointer
process_ref(proc);
queue_add(proc);

// Release when removing
queue_remove(proc);
process_unref(proc);
```

---

## 8. Architecture-Specific Code

### 8.1 Portability

Keep architecture-specific code in separate files:

```
kernel/
  core/           # Architecture-independent
  arch/
    x86_64/       # x86-64 specific
    arm64/        # ARM64 specific
```

### 8.2 Inline Assembly

Use minimal inline assembly, document thoroughly:

```c
static inline uint64_t rdtsc(void)
{
	uint32_t lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}
```

---

## 9. Testing

### 9.1 Unit Tests

- Every module should have unit tests in `tests/unit/`
- Test file naming: `test_<module>.c`
- Test function naming: `test_<feature>()`

### 9.2 Test Structure

```c
void test_pmm_allocation(void)
{
	void* page = pmm_alloc_page();
	ASSERT(page != NULL);
	ASSERT(((uintptr_t)page & 0xFFF) == 0);  // Page-aligned
	pmm_free_page(page);
}
```

---

## 10. Tools and Automation

### 10.1 Code Formatting

**Use clang-format:**
```bash
make format           # Format all code
make check-format     # Check formatting without modifying
```

Configuration: `.clang-format` in project root

### 10.2 Static Analysis

**Use clang-tidy:**
```bash
make lint             # Run static analysis
```

Configuration: `.clang-tidy` in project root

### 10.3 Pre-Commit Hooks

Install pre-commit hooks to enforce standards automatically:

```bash
./scripts/install-hooks.sh
```

This will:
- Auto-format code before commit
- Run linter checks
- Reject commits with errors

---

## Enforcement

All code submissions must:

1. Pass `make format` with no changes
2. Pass `make lint` with zero warnings
3. Follow naming and style conventions
4. Include appropriate comments and documentation
5. Pass all unit tests

Pull requests that don't meet these standards will be rejected by CI.

---

## References

- Linux Kernel Coding Style: https://www.kernel.org/doc/html/latest/process/coding-style.html
- MISRA C Guidelines (safety-critical subset)
- NASA C Style Guide

---

**Questions?** See `docs/DEVELOPMENT_GUIDE.md` or ask in #development channel.
