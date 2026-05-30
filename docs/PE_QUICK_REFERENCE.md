# PE Loader Quick Reference

## Fast Command Reference

### Load Windows Executable

```bash
# Basic execution
./automation_os --load-pe /path/to/program.exe

# With debug output
./automation_os --load-pe --verbose program.exe

# Specify DLL search path
./automation_os --load-pe --dll-path /windows/system32 program.exe
```

### Build Commands

```bash
# Build PE loader library
cd kernel/pe && make

# Build kernel with PE support
cd kernel && make PE_SUPPORT=1

# Run tests
cd tests && make test_pe && ./test_pe_parser
```

## API Quick Reference

### PE Loader Core

```c
#include <kernel/pe_loader.h>

// Parse PE file
pe_file_t *pe = pe_parse_file("C:\\test.exe");

// Load into memory
pe_load(pe, NULL);

// Load and execute
pe_load_and_execute("C:\\test.exe");

// Free PE structure
pe_free(pe);
```

### DLL Loading

```c
// Initialize DLL subsystem
dll_init();

// Load DLL
dll_handle_t *dll = dll_load("kernel32.dll");

// Get function pointer
void *func = dll_get_proc_address(dll, "CreateFileW");

// Free DLL
dll_free(dll);
```

### Handle Management

```c
// Initialize handle table
win32_handle_table_t table;
handle_table_init(&table);

// Create handle
HANDLE h = handle_create(&table, object, HANDLE_TYPE_FILE);

// Get object from handle
void *obj = handle_get_object(&table, h);

// Close handle
handle_close(&table, h);
```

## Win32 API Examples

### File I/O

```c
// Open file
HANDLE hFile = CreateFileW(L"test.txt", GENERIC_READ, 0, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

// Read file
char buffer[1024];
DWORD bytes_read;
ReadFile(hFile, buffer, sizeof(buffer), &bytes_read, NULL);

// Close file
CloseHandle(hFile);
```

### Memory Allocation

```c
// Allocate memory
void *mem = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
                        PAGE_READWRITE);

// Use memory
memset(mem, 0, 4096);

// Free memory
VirtualFree(mem, 0, MEM_RELEASE);
```

### Threading

```c
// Thread function
DWORD WINAPI thread_func(LPVOID param) {
    printf("Thread running\n");
    return 0;
}

// Create thread
HANDLE hThread = CreateThread(NULL, 0, thread_func, NULL, 0, NULL);

// Wait for thread
WaitForSingleObject(hThread, INFINITE);

// Close handle
CloseHandle(hThread);
```

### Registry

```c
// Open key
HKEY hKey;
RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Test", 0, 0, &hKey);

// Read value
WCHAR buffer[256];
DWORD size = sizeof(buffer);
RegQueryValueExW(hKey, L"ValueName", NULL, NULL, (BYTE *)buffer, &size);

// Write value
RegSetValueExW(hKey, L"ValueName", 0, REG_SZ, (BYTE *)L"Value", 12);

// Close key
RegCloseKey(hKey);
```

## PE File Structure

```
DOS Header (MZ)
  ↓
PE Signature ("PE\0\0")
  ↓
COFF Header
  ↓
Optional Header (PE32+)
  ↓
Section Headers
  ↓
Sections:
  - .text  (code)
  - .data  (initialized data)
  - .rdata (read-only data)
  - .bss   (uninitialized data)
  ↓
Import Directory
  ↓
Export Directory (DLLs)
  ↓
Relocation Directory
  ↓
TLS Directory
```

## Memory Layout

```
0x0000000000000000  NULL guard
0x0000000000010000  User stack (grows down)
0x0000007FFFFFFFFF  ──────────────────
0x0000000100000000  PE Image
                    - Headers
                    - .text (RX)
                    - .data (RW)
                    - .rdata (R)
0x0000010000000000  Loaded DLLs
                    - kernel32.dll
                    - user32.dll
0x00007FFFFFFFFFFF  ──────────────────
0xFFFF800000000000  Kernel space
```

## Common Error Codes

| Code | Constant | Description |
|------|----------|-------------|
| 0 | ERROR_SUCCESS | Success |
| 2 | ERROR_FILE_NOT_FOUND | File not found |
| 5 | ERROR_ACCESS_DENIED | Access denied |
| 6 | ERROR_INVALID_HANDLE | Invalid handle |
| 8 | ERROR_NOT_ENOUGH_MEMORY | Out of memory |
| 87 | ERROR_INVALID_PARAMETER | Invalid parameter |

## PE Loader Flags

```c
// File access
#define GENERIC_READ     0x80000000
#define GENERIC_WRITE    0x40000000

// File creation
#define CREATE_NEW       1
#define CREATE_ALWAYS    2
#define OPEN_EXISTING    3

// Memory protection
#define PAGE_READONLY           0x02
#define PAGE_READWRITE          0x04
#define PAGE_EXECUTE_READ       0x20
#define PAGE_EXECUTE_READWRITE  0x40

// Memory allocation
#define MEM_COMMIT   0x1000
#define MEM_RESERVE  0x2000
#define MEM_RELEASE  0x8000
```

## Debug Output

### Enable Verbose Logging

```bash
export PE_DEBUG=1
./automation_os --load-pe --verbose program.exe
```

### Sample Debug Output

```
PE: Loading executable: C:\test.exe
PE: Parsed EXE (3 sections, 2 imports)
PE: Loaded image at 0x140000000 (size 0x10000)
PE: Mapping section .text at RVA 0x1000 (size 0x2000)
PE: Mapping section .data at RVA 0x3000 (size 0x1000)
PE: Resolving 2 imports
PE: Loading import DLL: kernel32.dll
PE: Resolved 10 functions from kernel32.dll
PE: Entry point: 0x140001000
PE: Starting execution
```

## Testing Checklist

- [ ] PE parser loads valid executables
- [ ] PE parser rejects invalid files
- [ ] Sections mapped correctly
- [ ] Imports resolved
- [ ] Relocations applied
- [ ] Entry point called
- [ ] DLL loading works
- [ ] Handle management works
- [ ] Registry operations work
- [ ] File I/O works
- [ ] Threading works
- [ ] Synchronization works

## Troubleshooting

### DLL Not Found

**Problem:** `Failed to find DLL: user32.dll`

**Solution:**
```bash
# Create stub DLL or add to search path
export DLL_PATH=/windows/system32
```

### Import Resolution Failure

**Problem:** `Failed to resolve import: CreateWindowExW`

**Solution:** Implement stub function in `win32_user32.c`

### Memory Allocation Failure

**Problem:** `Failed to allocate memory at preferred base`

**Solution:** PE loader automatically tries alternative addresses

### Relocation Failure

**Problem:** `Failed to apply relocations`

**Solution:** Ensure PE file has relocation table (compile with `/DYNAMICBASE`)

## Performance Tips

1. **Use DLL caching** - Load DLLs once, reuse across processes
2. **Preload common DLLs** - kernel32, user32, gdi32 at boot
3. **Share read-only sections** - Reduce memory usage
4. **Profile hot paths** - Optimize frequently called APIs

## File Locations

```
kernel/pe/                  # PE loader implementation
  pe_loader.c              # Core PE parser and loader
  dll_loader.c             # DLL loading and symbol resolution
  handles.c                # Handle table management
  win32_kernel32.c         # Kernel32 API stubs
  win32_user32.c           # User32 API stubs
  win32_gdi32.c            # GDI32 API stubs
  registry.c               # Registry emulation
  win32_init.c             # Initialization
  Makefile                 # Build system

kernel/include/            # Headers
  pe_loader.h              # PE loader API
  pe_win32.h               # Win32 types and APIs

docs/                      # Documentation
  PE_LOADER_ARCHITECTURE.md
  WIN32_COMPATIBILITY.md
  PE_TESTING_GUIDE.md
  PE_LOADER_IMPLEMENTATION_COMPLETE.md
  PE_QUICK_REFERENCE.md    # This file
```

## Documentation Links

- **Architecture:** `PE_LOADER_ARCHITECTURE.md`
- **Compatibility:** `WIN32_COMPATIBILITY.md`
- **Testing:** `PE_TESTING_GUIDE.md`
- **Implementation:** `PE_LOADER_IMPLEMENTATION_COMPLETE.md`

## Support

For issues or questions:
1. Check documentation in `docs/`
2. Review source code comments in `kernel/pe/`
3. Run test suite in `tests/`
4. Enable debug logging with `PE_DEBUG=1`

---

**Quick Start:**

```bash
# 1. Build
cd kernel/pe && make

# 2. Test
cd tests && ./test_pe_parser

# 3. Run
./automation_os --load-pe /path/to/program.exe
```

**That's it! Your Windows application should now run natively on AutomationOS.**
