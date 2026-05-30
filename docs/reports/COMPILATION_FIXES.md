# AutomationOS Compilation Fixes

## Overview
This document lists every compilation issue found and fixed in the AutomationOS kernel.
The codebase had never been compiled before; it was written by independent agents with
many inconsistencies.

---

## 1. Duplicate Symbol Errors (Linker)

### 1.1 Duplicate `start_usermode` definition
- **Files**: `kernel/core/usermode.c`, `kernel/arch/x86_64/usermode.c`
- **Problem**: Both define `start_usermode()` with incompatible signatures (1 arg vs 2 args)
- **Fix**: Guarded `arch/x86_64/usermode.c` with `#ifdef USE_SYSRET_USERMODE`. The canonical implementation is `core/usermode.c` + `arch/x86_64/usermode.asm`.

### 1.2 Duplicate `vfs_init`, `vfs_mount`, `vfs_open`, `vfs_read`, `vfs_close`
- **Files**: `kernel/fs/vfs.c` (full impl), `kernel/fs/vfs_stub.c` (stub)
- **Problem**: Both define the same VFS functions
- **Fix**: Guarded `vfs_stub.c` with `#ifdef VFS_USE_STUB`

### 1.3 Duplicate `elf_load`, `elf_validate_header`
- **Files**: `kernel/fs/elf_loader.c` (full), `kernel/init/elf_loader.c` (simple)
- **Problem**: Both define the same ELF loader functions
- **Fix**: Guarded `kernel/init/elf_loader.c` with `#ifdef ELF_USE_SIMPLE_LOADER`

### 1.4 Duplicate `kernel_main`
- **Files**: `kernel/kernel.c`, `kernel/init/main_enhanced.c`
- **Problem**: Both define the kernel entry point
- **Fix**: Guarded `main_enhanced.c` with `#ifndef USE_ENHANCED_BOOT`

### 1.5 Duplicate string functions (`memcpy`, `memset`, `strlen`, etc.)
- **Files**: `kernel/lib/string.c` (optimized), `kernel/lib/string_original_backup.c` (original)
- **Problem**: Both define all string functions
- **Fix**: Guarded `string_original_backup.c` with `#ifdef USE_ORIGINAL_STRING_FUNCTIONS`

### 1.6 Duplicate `save_flags_cli` / `restore_flags`
- **Files**: `kernel/include/x86_64.h`, `kernel/include/spinlock.h`
- **Problem**: Both define identical static inline functions
- **Fix**: Removed from `x86_64.h`, kept in `spinlock.h` (canonical location)

### 1.7 Conflicting `irq_handler_t` typedef
- **Files**: `kernel/include/x86_64.h` (`void (*)(void)`), `kernel/include/irq.h` (`irq_return_t (*)(uint32_t, void*)`)
- **Problem**: Two incompatible typedefs with the same name
- **Fix**: Renamed `x86_64.h` version to `simple_irq_handler_t`. Updated `idt.c` local typedef to match.

---

## 2. Function Signature Mismatches

### 2.1 `start_usermode` argument count mismatch
- **Header** (`usermode.h`): `void start_usermode(uint64_t entry)` (1 arg)
- **Caller** (`kernel.c`): `start_usermode(init_entry, user_stack_ptr)` (2 args)
- **Definition** (`core/usermode.c`): 1 arg, (`arch/x86_64/usermode.c`): 2 args
- **Fix**: Unified to 2-arg signature: `void start_usermode(uint64_t entry, uint64_t stack)`. Updated header, definition, and caller in `init_usermode.c` (passes 0 for stack).

### 2.2 `framebuffer_init` first parameter type mismatch
- **Header** (`drivers.h`): `void framebuffer_init(void* fb_addr, ...)`
- **Caller** (`kernel.c`): passes `uint64_t` (boot_info->framebuffer_addr)
- **Fix**: Changed to `uint64_t fb_addr` in both header and `framebuffer.c`

---

## 3. Missing Standard Library Headers (Freestanding Environment)

### 3.1 Problem
27+ kernel source files use `#include <stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<string.h>`, `<stdio.h>`, `<stdlib.h>`, `<stdarg.h>`, `<time.h>`, `<errno.h>`. These don't exist in a freestanding environment with `-nostdinc`.

### 3.2 Fix: Compatibility shim headers
Created `kernel/include/compat/` directory with shim headers that redirect to kernel types:
- `compat/stdint.h` -> `types.h`
- `compat/stddef.h` -> `types.h` + `offsetof` macro
- `compat/stdbool.h` -> `types.h`
- `compat/string.h` -> `kernel/include/string.h`
- `compat/stdio.h` -> `kernel.h` (maps `printf` -> `kprintf`)
- `compat/stdlib.h` -> `mem.h` (maps `malloc` -> `kmalloc`, `free` -> `kfree`, provides `atoi`, `strdup`, `calloc`, `realloc`, `abs`)
- `compat/stdarg.h` -> GCC built-in `__builtin_va_*`
- `compat/time.h` -> kernel time stubs (`time_t`, `time()`, `ctime()`)
- `compat/errno.h` -> POSIX error codes + global `errno` variable

### 3.3 Fix: `<kernel/...>` includes (PE subsystem)
PE subsystem uses `<kernel/pe_loader.h>`, `<kernel/pe_win32.h>`, `<kernel/process.h>`, `<kernel/memory.h>`, `<kernel/vfs.h>`, `<kernel/scheduler.h>`. Created `compat/kernel/` shim directory.

### 3.4 Makefile update
Added `-nostdinc -isystem include/compat` to CFLAGS so `#include <...>` resolves to our shims.

---

## 4. Missing/Wrong Include Paths

### 4.1 `kernel/include/vfs.h`: `#include <stdint.h>` / `<stddef.h>`
- **Fix**: Changed to `#include "types.h"`

### 4.2 `kernel/include/initrd.h`: `#include <stdint.h>`
- **Fix**: Changed to `#include "types.h"`

### 4.3 `kernel/include/pe_loader.h`: `#include <stdint.h>` / `<stdbool.h>`
- **Fix**: Changed to `#include "types.h"`

### 4.4 `kernel/include/pe_win32.h`: `#include <stdint.h>` / `<stdbool.h>`
- **Fix**: Changed to `#include "types.h"`

### 4.5 `kernel/include/autofs.h`: `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<time.h>`
- **Fix**: Changed to `#include "types.h"` + guarded `time_t` typedef

### 4.6 `kernel/fs/vfs.c`: `#include <string.h>`
- **Fix**: Changed to `#include "../include/string.h"`

### 4.7 `kernel/lib/string_benchmark.c`: `#include "../include/kprintf.h"` (nonexistent)
- **Fix**: Changed to `#include "../include/kernel.h"`

### 4.8 `kernel/power/power.c`: `#include "../include/memory.h"` (nonexistent)
- **Fix**: Changed to `#include "../include/mem.h"`

### 4.9 `kernel/acpi/acpi.c`: `#include "../include/memory.h"` (nonexistent)
- **Fix**: Changed to `#include "../include/mem.h"`

### 4.10 `kernel/drivers/net/wireless/mac80211/mac80211_core.c`: wrong relative path
- **Problem**: `../../../include/` resolves to `drivers/include/` not `kernel/include/`
- **Fix**: Changed to `../../../../include/`

### 4.11 `kernel/include/sched.h`: uses `NORETURN` without including `kernel.h`
- **Fix**: Added `#include "kernel.h"`

### 4.12 `kernel/fs/elf_test.c`: uses `USER_SPACE_END` without including `mem.h`
- **Fix**: Added `#include "../include/mem.h"`

---

## 5. Missing Type Definitions

### 5.1 `UINT64_MAX` not defined
- **Used in**: `kernel/include/perf.h` (`stat->min_cycles = UINT64_MAX`)
- **Fix**: Added `UINT64_MAX`, `INT64_MAX`, `UINT32_MAX` etc. to `types.h`

### 5.2 `wchar_t` not defined (freestanding)
- **Used in**: `kernel/include/pe_win32.h` (`typedef wchar_t WCHAR`)
- **Fix**: Changed to `typedef uint16_t WCHAR`

### 5.3 `intptr_t` / `ptrdiff_t` not defined
- **Fix**: Added to `types.h`

---

## 6. Missing Headers Created

### 6.1 `kernel/include/io.h`
- Wrapper that includes `x86_64.h` (provides `inb`/`outb`/etc.)
- Used by: `power/thermal.c`, `power/cpufreq.c`, `power/battery.c`, `acpi/acpi.c`

### 6.2 `kernel/include/time.h`
- Kernel time interface using PIT timer
- Provides `get_ticks()`, `ms_to_ticks()`, `ticks_to_ms()`, `TIMER_FREQ`
- Used by: `kernel/tests/test_pmm_deadlock.c`

### 6.3 Global `errno` variable
- Defined in `kernel/lib/panic.c`
- Needed by: autofs subsystem, PE subsystem, seccomp

---

## 7. Inline Assembly Syntax Errors

### 7.1 `kernel/core/test_usermode.c`: Intel syntax in AT&T context
- **Problem**: Used Intel syntax (`mov rax, 3`) in GCC inline asm (AT&T default)
- **Fix**: Converted to AT&T syntax (`mov $3, %%rax`)

---

## 8. Missing Function Definitions

### 8.1 `vfs_unmount`, `vfs_rmdir`, `vfs_dentry_lookup`, `vfs_dentry_add_child`
- **Declared in**: `kernel/include/vfs.h`
- **Not defined in**: `kernel/fs/vfs.c`
- **Fix**: Added stub implementations to `vfs.c`

---

## 9. kprintf Format String Support

### 9.1 Missing format specifiers
- **Problem**: Original `kprintf` only supported `%s`, `%d`, `%u`, `%x`, `%p`
- **Missing**: `%ld`, `%lu`, `%lx`, `%llu`, `%016lx`, `%.2f`, `%c`, `%X`, `%zu`
- **Fix**: Rewrote `kernel/lib/printf.c` with full format support:
  - Length modifiers: `l`, `ll`, `h`, `hh`, `z`
  - Width and padding: `%016lx`, `%8lu`
  - Precision: `%.2f`
  - Format chars: `%d`, `%i`, `%u`, `%x`, `%X`, `%s`, `%c`, `%p`, `%f`, `%%`

---

## 10. Build System Updates

### 10.1 Root kernel Makefile
- Added `-nostdinc` to prevent searching host system headers
- Added `-isystem include/compat` for compatibility shim headers

### 10.2 `kernel/fs/Makefile`
- Updated to use consistent compiler flags with kernel Makefile
- Added all VFS source files to the build

---

## Summary of Files Modified

### Headers Modified:
- `kernel/include/types.h` - Added UINT64_MAX, intptr_t, ptrdiff_t
- `kernel/include/x86_64.h` - Removed duplicate save_flags_cli/restore_flags, renamed irq_handler_t
- `kernel/include/vfs.h` - Fixed includes (<stdint.h> -> "types.h")
- `kernel/include/initrd.h` - Fixed includes
- `kernel/include/usermode.h` - Fixed start_usermode signature to 2 args
- `kernel/include/sched.h` - Added missing kernel.h include
- `kernel/include/drivers.h` - Fixed framebuffer_init signature
- `kernel/include/pe_loader.h` - Fixed includes
- `kernel/include/pe_win32.h` - Fixed includes, wchar_t
- `kernel/include/autofs.h` - Fixed includes, time_t

### Source Files Modified:
- `kernel/kernel.c` - (kept as-is, fixes were in headers/definitions it uses)
- `kernel/fs/vfs.c` - Fixed string.h include, added missing function stubs
- `kernel/fs/vfs_stub.c` - Guarded with ifdef
- `kernel/fs/elf_test.c` - Added missing mem.h include
- `kernel/init/elf_loader.c` - Guarded with ifdef
- `kernel/init/main_enhanced.c` - Guarded with ifdef
- `kernel/core/usermode.c` - Updated to 2-arg signature
- `kernel/core/init_usermode.c` - Updated call to 2-arg
- `kernel/core/test_usermode.c` - Fixed inline asm syntax
- `kernel/arch/x86_64/usermode.c` - Guarded with ifdef
- `kernel/arch/x86_64/idt.c` - Renamed irq_handler_t to simple_irq_handler_t
- `kernel/drivers/framebuffer.c` - Updated framebuffer_init signature
- `kernel/drivers/net/wireless/mac80211/mac80211_core.c` - Fixed include path
- `kernel/lib/printf.c` - Rewrote with full format support
- `kernel/lib/string_benchmark.c` - Fixed nonexistent kprintf.h include
- `kernel/lib/string_original_backup.c` - Guarded with ifdef
- `kernel/lib/panic.c` - Added errno variable
- `kernel/power/power.c` - Fixed memory.h -> mem.h
- `kernel/acpi/acpi.c` - Fixed memory.h -> mem.h
- `kernel/Makefile` - Added -nostdinc and -isystem compat
- `kernel/fs/Makefile` - Updated build rules

### New Files Created:
- `kernel/include/compat/stdint.h`
- `kernel/include/compat/stddef.h`
- `kernel/include/compat/stdbool.h`
- `kernel/include/compat/string.h`
- `kernel/include/compat/stdio.h`
- `kernel/include/compat/stdlib.h`
- `kernel/include/compat/stdarg.h`
- `kernel/include/compat/time.h`
- `kernel/include/compat/errno.h`
- `kernel/include/compat/kernel/memory.h`
- `kernel/include/compat/kernel/pe_loader.h`
- `kernel/include/compat/kernel/pe_win32.h`
- `kernel/include/compat/kernel/process.h`
- `kernel/include/compat/kernel/vfs.h`
- `kernel/include/compat/kernel/scheduler.h`
- `kernel/include/io.h`
- `kernel/include/time.h`
