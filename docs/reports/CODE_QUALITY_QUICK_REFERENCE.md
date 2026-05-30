# Code Quality Quick Reference
## AutomationOS Kernel Development Guide

**Last Updated**: 2026-05-26

---

## ⚡ Quick Rules

### DO ✓

- ✓ Use `snake_case` for functions: `pmm_alloc_page()`
- ✓ Use `snake_case_t` for types: `memory_map_entry_t`
- ✓ Use `UPPER_CASE` for constants: `PAGE_SIZE`
- ✓ Use named constants instead of magic numbers
- ✓ Document all public APIs
- ✓ Keep functions under 50 lines
- ✓ Use 4-space indentation
- ✓ Keep lines under 100 characters

### DON'T ✗

- ✗ Use magic numbers (0x9A, 0x80, etc.)
- ✗ Leave public APIs undocumented
- ✗ Use camelCase or PascalCase (except Win32/UEFI compatibility)
- ✗ Write functions longer than 50 lines
- ✗ Leave TODO comments without issue tracking
- ✗ Leave commented-out code
- ✗ Use obvious comments

---

## 🔢 Magic Numbers → Named Constants

### Example: Hardware Registers

**Bad**:
```c
outb(0x3F8 + 3, 0x80);  // What is 0x3F8? What is 3? What is 0x80?
```

**Good**:
```c
outb(COM1_PORT + SERIAL_LINE_CTRL, SERIAL_LCR_DLAB);
```

### Available Constant Headers

1. **`kernel/include/gdt_constants.h`** - GDT segment descriptors
2. **`kernel/include/serial_regs.h`** - Serial port registers
3. **`kernel/include/lapic_constants.h`** - Local APIC flags

### Creating New Constants

```c
// In appropriate header file:
#define NEW_CONSTANT_NAME  0x1234  /* Brief description */
```

---

## 📚 Documentation Style

### Function Documentation Template

```c
/*
 * Brief one-line description
 * param1: description of first parameter
 * param2: description of second parameter
 * Returns: description of return value
 */
return_type function_name(type1 param1, type2 param2) {
    // implementation
}
```

### Example: Good Documentation

```c
/*
 * Allocate a single 4KB physical page
 * Returns: physical address of page, or NULL on failure
 */
void* pmm_alloc_page(void) {
    // implementation
}
```

### File Header Template

```c
/*
 * Module Name
 * ===========
 *
 * Brief description of file purpose.
 * Additional details if needed.
 */
```

---

## 🎯 Naming Conventions

### Functions (snake_case)

```c
void serial_init(void);              // ✓ Good
void lapic_send_ipi(uint32_t id);    // ✓ Good
void SerialInit(void);               // ✗ Bad (PascalCase)
void serialInit(void);               // ✗ Bad (camelCase)
```

### Types (snake_case_t)

```c
typedef struct {
    uint64_t base;
    uint64_t length;
} memory_map_entry_t;                 // ✓ Good

typedef struct MemoryMapEntry;       // ✗ Bad (PascalCase)
```

### Constants (UPPER_CASE)

```c
#define PAGE_SIZE           4096      // ✓ Good
#define KERNEL_VIRTUAL_BASE 0xFF...   // ✓ Good
#define pageSize            4096      // ✗ Bad (camelCase)
```

### Variables (snake_case)

```c
uint64_t total_memory;               // ✓ Good
bool serial_initialized;             // ✓ Good
uint64_t totalMemory;                // ✗ Bad (camelCase)
```

---

## 🔍 Code Review Checklist

Before committing, check:

- [ ] No magic numbers (use named constants)
- [ ] All new functions documented
- [ ] Functions under 50 lines
- [ ] Naming conventions followed
- [ ] No TODO without issue tracking
- [ ] No commented-out code
- [ ] No obvious comments removed
- [ ] Build passes without warnings
- [ ] Tests pass

---

## 🛠️ Common Patterns

### Hardware Register Access

**Pattern**:
```c
#define DEVICE_BASE_PORT    0x3F8
#define DEVICE_REG_CTRL     0
#define DEVICE_REG_DATA     1
#define DEVICE_CTRL_ENABLE  0x80

void device_init(void) {
    outb(DEVICE_BASE_PORT + DEVICE_REG_CTRL, DEVICE_CTRL_ENABLE);
}
```

### Bit Flags

**Pattern**:
```c
#define FLAG_ENABLE    (1 << 0)
#define FLAG_READY     (1 << 1)
#define FLAG_ERROR     (1 << 2)

if (status & FLAG_READY) {
    // Handle ready state
}
```

### MSR Operations

**Pattern**:
```c
#define MSR_FEATURE_ENABLE  (1 << 10)

uint64_t msr = rdmsr(MSR_NUMBER);
msr |= MSR_FEATURE_ENABLE;
wrmsr(MSR_NUMBER, msr);
```

---

## 🚫 Anti-Patterns to Avoid

### Magic Number Anti-Pattern

```c
// ✗ BAD
if (status & 0x20) {
    outb(port + 3, 0x80);
}

// ✓ GOOD
if (status & SERIAL_LSR_TX_EMPTY) {
    outb(port + SERIAL_LINE_CTRL, SERIAL_LCR_DLAB);
}
```

### Obvious Comment Anti-Pattern

```c
// ✗ BAD
i++;  // Increment i

// ✓ GOOD (no comment needed)
i++;

// ✓ GOOD (useful comment)
i++;  // Skip null terminator
```

### Long Function Anti-Pattern

```c
// ✗ BAD: 150-line function
void do_everything(void) {
    // 150 lines of code
}

// ✓ GOOD: Split into smaller functions
void init_hardware(void) { }
void setup_memory(void) { }
void load_drivers(void) { }
```

---

## 📊 Quality Metrics

### Current Standards

| Metric | Target | Current |
|--------|--------|---------|
| Overall Compliance | 90% | 94% ✓ |
| Magic Numbers | 0 | 0 ✓ |
| API Documentation | 100% | 100% ✓ |
| Function Size | <50 lines | 98% ✓ |

---

## 🔗 Related Files

- **Full Audit**: `CODE_QUALITY_AUDIT_REPORT.md`
- **Fixes Applied**: `CODE_QUALITY_FIXES_APPLIED.md`
- **Verification**: `CODE_QUALITY_CHECKLIST.md`
- **Summary**: `CODE_QUALITY_EXECUTIVE_SUMMARY.md`

---

## 📞 Questions?

If you're unsure about code quality:
1. Check this guide
2. Look at recent examples in core kernel files
3. Reference the constant headers
4. Review the audit report

---

## 🎓 Examples from Codebase

### GDT Configuration ✓

```c
#include "../../include/gdt_constants.h"

gdt_set_gate(1, 0, GDT_LIMIT_FULL, 
             GDT_ACCESS_KERNEL_CODE, 
             GDT_GRAN_64BIT_MODE);
```

### Serial Port Initialization ✓

```c
#include "../../include/serial_regs.h"

outb(COM1_PORT + SERIAL_INT_ENABLE, SERIAL_INT_DISABLE_ALL);
outb(COM1_PORT + SERIAL_LINE_CTRL, SERIAL_LCR_DLAB);
outb(COM1_PORT + SERIAL_DIVISOR_LOW, SERIAL_BAUD_38400);
```

### LAPIC Operations ✓

```c
#include "../../include/lapic_constants.h"

if (ecx & CPUID_FEAT_X2APIC) {
    apic_base_msr |= APIC_MSR_X2APIC_ENABLE;
}
```

---

## ⚠️ Exceptions

### Approved Naming Exceptions

1. **Win32 API** - `BOOL`, `DWORD`, `HWND` (Windows compatibility)
2. **UEFI Types** - `EFI_STATUS`, `UINTN` (UEFI specification)
3. **Boolean** - `true`, `false` (Standard C convention)

These are required for external API compatibility.

---

**Keep this guide handy during development!**

**Quality is not an act, it is a habit.** - Aristotle
