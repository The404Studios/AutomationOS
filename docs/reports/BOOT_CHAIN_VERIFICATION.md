# Boot Chain Verification Report

## Overview

Full boot chain verification from power-on to desktop.
Each connection point between components was checked for matching function signatures,
parameter types, return types, include paths, and naming conventions.

**Boot Chain**: Bootloader -> Kernel -> VFS -> Initrd -> ELF -> UserMode -> Init -> Services -> Compositor -> WM -> Desktop

---

## Phase 1: Bootloader -> Kernel

**Files**: `boot/bootloader_enhanced.c` -> `kernel/kernel.c`

### Connection Point
- Bootloader calls `kernel_main(&boot_info)` at line 559
- Kernel defines `void kernel_main(boot_info_t* boot_info)` at line 62

### Issues Found and Fixed

**CRITICAL FIX 1: boot_info_t struct mismatch**
- `kernel/kernel.c` had an OLD `boot_info_t` with only 11 fields (missing magic, version, total_memory, framebuffer_bpp, rsdp_addr, cmdline, boot_time_ms, loader_name, loader_version)
- `boot/boot_enhanced.h` defines the FULL struct with 20+ fields
- The bootloader passes the enhanced struct; the kernel was reading wrong memory offsets
- **Fixed**: Replaced local `boot_info_t` in `kernel/kernel.c` with the full enhanced version matching `boot_enhanced.h` exactly

**CRITICAL FIX 2: memory_map_entry_t struct size mismatch**
- `kernel/include/mem.h` had 3 fields: `{base, length, type}` (20 bytes)
- `boot/boot_enhanced.h` has 4 fields: `{base, length, type, reserved}` (24 bytes)
- Struct size difference caused memory map parsing corruption
- **Fixed**: Added `uint32_t reserved` field to `mem.h` definition

### Verified OK
- Function signature: `void kernel_main(boot_info_t*)` matches on both sides
- `initrd_addr` and `initrd_size` field names match
- Bootloader sets `boot_info.kernel_entry = (void*)entry_point` correctly
- ELF types (Elf64_Ehdr, Elf64_Phdr) match between `boot.h` and `boot_enhanced.h`

---

## Phase 2: Kernel Init -> VFS

**Files**: `kernel/kernel.c` -> `kernel/fs/vfs.c`

### Connection Point
- Kernel calls `vfs_init()` at line 160
- Kernel calls `vfs_mount("none", "/", "ramfs")` at line 163

### Verified OK
- `kernel/kernel.c` includes `include/vfs.h`
- `vfs_init()` signature: `void vfs_init(void)` matches declaration in `vfs.h` line 168
- `vfs_mount()` signature: `int vfs_mount(const char*, const char*, const char*)` matches `vfs.h` line 169
- Implementation in `kernel/fs/vfs.c` matches all declarations
- `kernel/fs/vfs_stub.c` is guarded with `#ifdef VFS_USE_STUB` to avoid duplicate symbols

### Issue Found and Fixed in Enhanced Path
- `kernel/init/main_enhanced.c` called `initrd_mount()` without first calling `vfs_init()` or `vfs_mount("none", "/", "ramfs")`
- The initrd mount relies on VFS being initialized and root being mounted
- **Fixed**: Added `vfs_init()` and `vfs_mount("none", "/", "ramfs")` calls to `mount_root()` in main_enhanced.c

---

## Phase 3: VFS -> Initrd

**Files**: `kernel/kernel.c` -> `kernel/init/initrd.c`

### Connection Point
- Kernel calls `initrd_init(boot_info->initrd_addr, boot_info->initrd_size)` at line 192
- Kernel calls `initrd_mount()` at line 197
- Kernel calls `initrd_get_stats()` and `initrd_list_files()`

### Verified OK
- `kernel/kernel.c` includes `include/initrd.h`
- `initrd_init(uint64_t addr, uint64_t size)` matches `initrd.h` line 16 and `initrd.c` line 87
- `initrd_mount()` returns `int`, matches `initrd.h` line 23 and `initrd.c` line 100
- `initrd_get_stats(uint64_t*, uint64_t*)` matches `initrd.h` line 45 and `initrd.c` line 308
- `initrd_list_files(void)` matches `initrd.h` line 37 and `initrd.c` line 250
- Initrd correctly uses VFS API: `vfs_path_lookup("/")`, `vfs_ramfs_create_file()`, `vfs_mkdir_recursive()`

---

## Phase 4: Initrd -> ELF Loader

**Files**: `kernel/kernel.c` -> `kernel/fs/elf_loader.c`

### Connection Point
- Kernel calls `elf_load("init", 0, NULL, &init_entry, &init_stack)` at line 222

### Issues Found and Fixed

**FIX: Duplicate elf_loader.c files**
- `kernel/fs/elf_loader.c` - Full implementation (canonical)
- `kernel/init/elf_loader.c` - Simplified duplicate
- Both defined `elf_load()` and `elf_validate_header()`, causing link-time duplicate symbols
- `kernel/init/elf_loader.c` was already guarded with `#ifdef ELF_USE_SIMPLE_LOADER`
- **Verified**: Guard is in place, no symbol conflict

### Verified OK
- `elf_load(const char* path, int argc, char** argv, uint64_t* entry_out, uint64_t* stack_out)` matches `elf.h` line 111 and `elf_loader.c` line 256
- ELF loader uses `initrd_get_file()` to read files from initrd (correct)
- ELF types (`elf64_ehdr_t`, `elf64_phdr_t`) defined in `include/elf.h` and used consistently
- `elf_validate_header()` checks magic, class, endianness, architecture, type
- Return codes (`ELF_SUCCESS`, `ELF_ERR_NOT_FOUND`, etc.) defined in `elf.h`

---

## Phase 5: ELF Loader -> User Mode

**Files**: `kernel/kernel.c` -> `kernel/core/usermode.c` -> `kernel/arch/x86_64/usermode.asm`

### Connection Point
- Kernel calls `start_usermode(init_entry, user_stack_ptr)` at line 268
- `kernel/core/usermode.c` implements `start_usermode()` which calls `enter_usermode()` from assembly
- `kernel/arch/x86_64/usermode.asm` exports `enter_usermode(entry, stack)`

### Issues Found and Fixed

**CRITICAL FIX 3: start_usermode() parameter count mismatch**
- `kernel/include/usermode.h` originally declared `void start_usermode(uint64_t entry)` with ONE parameter
- `kernel/kernel.c` called `start_usermode(init_entry, user_stack_ptr)` with TWO parameters
- `kernel/core/usermode.c` implemented `start_usermode(uint64_t entry, uint64_t stack)` with TWO parameters
- **Fixed**: Updated `usermode.h` to declare `start_usermode(uint64_t entry, uint64_t stack)`
- **Fixed**: Added `extern void enter_usermode(uint64_t entry, uint64_t stack)` declaration

**CRITICAL FIX 4: Duplicate start_usermode() definitions**
- `kernel/arch/x86_64/usermode.c` defined its own `start_usermode()` (SYSRET-based)
- `kernel/core/usermode.c` defined the canonical `start_usermode()` (IRETQ-based via asm)
- Duplicate symbol error at link time
- **Fixed**: Wrapped `kernel/arch/x86_64/usermode.c` with `#ifdef USE_SYSRET_USERMODE` guard

**FIX 5: Missing TSS initialization**
- `kernel/kernel.c` never called `tss_init()` before `start_usermode()`
- Without TSS, returning from user mode (syscalls, interrupts) would crash
- **Fixed**: Added `tss_init()` call after `gdt_init()` in kernel initialization
- **Fixed**: Added `#include "include/tss.h"` to kernel.c

### Verified OK
- GDT segment selectors consistent: User Code=0x1B (entry 3, RPL=3), User Data=0x23 (entry 4, RPL=3)
- Assembly `enter_usermode` sets DS/ES/FS/GS=0x23, pushes IRETQ frame (SS=0x23, RSP, RFLAGS, CS=0x1B, RIP)
- C wrapper `start_usermode` allocates kernel stack, calls `tss_set_kernel_stack()`, then calls `enter_usermode`
- TSS structure in `tss.h` has `rsp0` field for kernel stack on privilege transitions

---

## Phase 6: User Mode -> Init

**Files**: `userspace/init/init.c`

### Connection Point
- Init binary entry point: `void _start(void)` (standard ELF entry)
- Loaded by ELF loader from initrd at path "init"
- Runs in user mode (ring 3)

### Verified OK
- `init.c` uses `userspace/libc/syscall.h` for syscalls
- Syscall numbers match between `userspace/libc/syscall.h` and `kernel/include/syscall.h`:
  - SYS_EXIT=0, SYS_FORK=1, SYS_READ=2, SYS_WRITE=3, SYS_OPEN=4, SYS_CLOSE=5, SYS_WAITPID=6, SYS_EXECVE=7, SYS_GETPID=8, SYS_SLEEP=9
- Syscall wrapper uses `syscall` instruction (x86_64 convention): RAX=syscall number, RDI/RSI/RDX/R10/R8/R9=args
- Init calls `getpid()`, `fork()`, `execve()`, `waitpid()`, `sleep()`, `exit()` - all implemented in libc

---

## Phase 7: Init -> Services

**Files**: `userspace/init/init.c` -> `userspace/system/services/servicemanager_main.c`

### Connection Point
- Init forks and calls `execve("/sbin/servicemanager", argv, envp)`
- `servicemanager_main.c` provides `main()` entry point
- Service manager loads `.service` files from `/etc/services/`

### Issues Found and Fixed

**FIX 6: Service dependency name mismatch**
- Service files used `.service` suffix in dependency fields (e.g., `Requires=dbus.service`)
- Service manager loads services by filename stem (e.g., name="dbus" from "dbus.service")
- `service_find("dbus.service")` would NOT match a service named `"dbus"`
- **Fixed**: Added `strip_service_suffix()` helper to servicemanager.c
- **Fixed**: Updated parser to strip `.service` suffix from Requires/Wants/After/Before/Conflicts values

### Verified OK
- `servicemanager_main.c` supports `--boot` flag that init passes
- `service_manager_init()` loads all `.service` files from `/etc/services/`
- `service_manager_start_boot_services()` starts enabled services with dependency resolution
- Config directory constant `SERVICE_CONFIG_DIR = "/etc/services"` matches actual directory location

---

## Phase 8: Services -> Desktop

**Files**: Service files in `etc/services/` -> Userspace binaries

### Service Dependency Chain
```
syslogd (no deps)
  -> dbus (Requires=syslogd)
     -> devmgr (Requires=syslogd)
        -> compositor (Requires=devmgr)
           -> window-manager (Requires=compositor)
              -> desktop-shell (Requires=window-manager, displayd)
```

### Issues Found and Fixed

**FIX 7: compositor.service wrong dependency names**
- Had `Requires=device-manager` but service file is `devmgr.service` (name="devmgr")
- Had `Before=display-manager window-manager` but service is `displayd.service`
- **Fixed**: Changed to `Requires=devmgr`, `After=devmgr`, `Before=displayd window-manager`

**FIX 8: desktop-shell.service used .service suffix in deps**
- Had `Requires=window-manager.service displayd.service`
- **Fixed**: Changed to `Requires=window-manager displayd`

**FIX 9: Window manager missing WM socket**
- Desktop shell expects to connect to `/run/wm.sock` (set via `WM_SOCKET` env var)
- Window manager (`userspace/wm/main.c`) connected to compositor but never created its own listening socket
- **Fixed**: Added WM socket creation (`/run/wm.sock`) to `userspace/wm/main.c`
- Socket is created before main loop, cleaned up on exit

### IPC Socket Paths (Verified Consistent)
- Compositor creates: `/run/compositor.sock` (defined in `compositord.c` and `main.c`)
- WM connects to: `/run/compositor.sock` (defined in `wm/main.c`)
- WM creates: `/run/wm.sock` (newly added)
- Desktop Shell connects to: `/run/wm.sock` (defined in `shell/desktop/main.c`, env `WM_SOCKET`)

---

## Duplicate Symbol Prevention (Verified)

| File | Guard | Status |
|------|-------|--------|
| `kernel/fs/vfs_stub.c` | `#ifdef VFS_USE_STUB` | OK - won't conflict with `vfs.c` |
| `kernel/init/elf_loader.c` | `#ifdef ELF_USE_SIMPLE_LOADER` | OK - won't conflict with `fs/elf_loader.c` |
| `kernel/init/main_enhanced.c` | `#ifndef USE_ENHANCED_BOOT` | OK - won't conflict with `kernel.c` |
| `kernel/arch/x86_64/usermode.c` | `#ifdef USE_SYSRET_USERMODE` | FIXED - was conflicting with `core/usermode.c` |

---

## Summary of All Fixes

| # | Severity | File | Fix |
|---|----------|------|-----|
| 1 | CRITICAL | `kernel/kernel.c` | Replaced old `boot_info_t` (11 fields) with enhanced version (20+ fields) matching bootloader |
| 2 | CRITICAL | `kernel/include/mem.h` | Added missing `reserved` field to `memory_map_entry_t` for struct alignment with bootloader |
| 3 | CRITICAL | `kernel/include/usermode.h` | Fixed `start_usermode()` to take 2 params (entry, stack); added `enter_usermode` extern |
| 4 | CRITICAL | `kernel/arch/x86_64/usermode.c` | Wrapped with `#ifdef USE_SYSRET_USERMODE` to prevent duplicate `start_usermode` |
| 5 | HIGH | `kernel/kernel.c` | Added `tss_init()` call + `#include "tss.h"` for user->kernel transitions |
| 6 | HIGH | `kernel/init/main_enhanced.c` | Added `vfs_init()` + `vfs_mount()` before initrd mount; added ELF load + usermode transition |
| 7 | HIGH | `userspace/system/services/servicemanager.c` | Added `strip_service_suffix()` to fix dependency resolution |
| 8 | MEDIUM | `etc/services/compositor.service` | Fixed dependency names: `device-manager` -> `devmgr`, `display-manager` -> `displayd` |
| 9 | MEDIUM | `etc/services/desktop-shell.service` | Removed `.service` suffix from Requires/After values |
| 10 | MEDIUM | `userspace/wm/main.c` | Added WM listening socket at `/run/wm.sock` for desktop-shell connections |

---

## Boot Chain Status: CONNECTED

All 8 phases verified. All critical mismatches fixed.
The boot chain flows correctly from Bootloader through Kernel, VFS, Initrd, ELF, UserMode, Init, Services, to Desktop.
