# PE Loader Testing Guide

## Overview

This guide provides comprehensive testing procedures for the AutomationOS PE loader and Windows compatibility layer. It covers unit tests, integration tests, and real-world Windows executable testing.

## Test Environment Setup

### Prerequisites

```bash
# Build AutomationOS with PE support
cd /path/to/kernel
make clean
make PE_SUPPORT=1

# Create Windows test directory structure
mkdir -p /windows/system32
mkdir -p /windows/test

# Copy test executables (if available)
cp /mnt/windows/system32/*.dll /windows/system32/
```

### Test Directory Structure

```
/windows/
├── system32/           # Windows system DLLs
│   ├── kernel32.dll   # (stub implementation)
│   ├── user32.dll     # (stub implementation)
│   ├── gdi32.dll      # (stub implementation)
│   └── ntdll.dll      # (stub implementation)
├── test/              # Test executables
│   ├── hello.exe      # Simple console program
│   ├── threads.exe    # Multi-threading test
│   ├── fileio.exe     # File I/O test
│   └── gui.exe        # GUI test (stub rendering)
└── programs/          # Real Windows applications
    ├── notepad.exe
    ├── cmd.exe
    └── ping.exe
```

## Unit Tests

### 1. PE Parser Tests

**Test PE file parsing:**

```c
// test_pe_parser.c
#include <kernel/pe_loader.h>
#include <assert.h>

void test_pe_parse_valid(void) {
    // Load valid PE file
    pe_file_t *pe = pe_parse_file("/windows/test/hello.exe");
    
    assert(pe != NULL);
    assert(pe->dos_header->e_magic == DOS_SIGNATURE);
    assert(pe->pe_signature == PE_SIGNATURE);
    assert(pe->section_count > 0);
    
    pe_free(pe);
    printf("✓ PE parser: valid file\n");
}

void test_pe_parse_invalid(void) {
    // Try to parse invalid file
    pe_file_t *pe = pe_parse_file("/etc/passwd");
    
    assert(pe == NULL);
    printf("✓ PE parser: invalid file rejection\n");
}

void test_pe_sections(void) {
    pe_file_t *pe = pe_parse_file("/windows/test/hello.exe");
    
    assert(pe != NULL);
    
    // Check for .text section
    section_header_t *text = pe_find_section(pe, ".text");
    assert(text != NULL);
    assert(text->characteristics & IMAGE_SCN_CNT_CODE);
    assert(text->characteristics & IMAGE_SCN_MEM_EXECUTE);
    
    // Check for .data section
    section_header_t *data = pe_find_section(pe, ".data");
    if (data) {
        assert(data->characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA);
        assert(data->characteristics & IMAGE_SCN_MEM_WRITE);
    }
    
    pe_free(pe);
    printf("✓ PE parser: section parsing\n");
}
```

**Run tests:**

```bash
gcc -o test_pe_parser test_pe_parser.c -I../kernel/include -L../kernel/pe -lpe
./test_pe_parser
```

### 2. DLL Loader Tests

**Test DLL loading and symbol resolution:**

```c
// test_dll_loader.c
#include <kernel/pe_loader.h>
#include <assert.h>

void test_dll_load(void) {
    dll_init();
    
    // Load kernel32.dll
    dll_handle_t *dll = dll_load("kernel32.dll");
    assert(dll != NULL);
    assert(dll->pe != NULL);
    assert(dll->pe->is_dll);
    
    printf("✓ DLL loader: load kernel32.dll\n");
    
    // Try loading again (should return cached)
    dll_handle_t *dll2 = dll_load("kernel32.dll");
    assert(dll2 == dll);
    assert(dll->ref_count == 2);
    
    printf("✓ DLL loader: caching\n");
    
    dll_free(dll);
    dll_free(dll2);
}

void test_get_proc_address(void) {
    dll_handle_t *dll = dll_load("kernel32.dll");
    assert(dll != NULL);
    
    // Get function by name
    void *func = dll_get_proc_address(dll, "CreateFileW");
    assert(func != NULL);
    
    printf("✓ DLL loader: GetProcAddress by name\n");
    
    // Try invalid function
    void *invalid = dll_get_proc_address(dll, "InvalidFunction");
    assert(invalid == NULL);
    
    printf("✓ DLL loader: invalid function rejection\n");
    
    dll_free(dll);
}
```

### 3. Handle Management Tests

**Test Windows handle table:**

```c
// test_handles.c
#include <kernel/pe_win32.h>
#include <assert.h>

void test_handle_create(void) {
    win32_handle_table_t table;
    handle_table_init(&table);
    
    // Create file handle
    int fd = 42;
    HANDLE h = handle_create(&table, (void *)(uint64_t)fd, HANDLE_TYPE_FILE);
    
    assert(h != INVALID_HANDLE_VALUE);
    assert(h != NULL);
    
    // Retrieve object
    void *obj = handle_get_object(&table, h);
    assert(obj == (void *)(uint64_t)fd);
    
    printf("✓ Handles: create and retrieve\n");
    
    // Close handle
    handle_close(&table, h);
    
    // Should not be retrievable after close
    obj = handle_get_object(&table, h);
    assert(obj == NULL);
    
    printf("✓ Handles: close\n");
}

void test_handle_types(void) {
    win32_handle_table_t table;
    handle_table_init(&table);
    
    // Test different handle types
    HANDLE h_file = handle_create(&table, (void *)1, HANDLE_TYPE_FILE);
    HANDLE h_thread = handle_create(&table, (void *)2, HANDLE_TYPE_THREAD);
    HANDLE h_mutex = handle_create(&table, (void *)3, HANDLE_TYPE_MUTEX);
    
    assert(h_file != h_thread);
    assert(h_thread != h_mutex);
    
    handle_entry_t *e_file = handle_get_entry(&table, h_file);
    handle_entry_t *e_thread = handle_get_entry(&table, h_thread);
    handle_entry_t *e_mutex = handle_get_entry(&table, h_mutex);
    
    assert(e_file->type == HANDLE_TYPE_FILE);
    assert(e_thread->type == HANDLE_TYPE_THREAD);
    assert(e_mutex->type == HANDLE_TYPE_MUTEX);
    
    printf("✓ Handles: multiple types\n");
    
    handle_close(&table, h_file);
    handle_close(&table, h_thread);
    handle_close(&table, h_mutex);
}
```

### 4. Registry Tests

**Test registry emulation:**

```c
// test_registry.c
#include <kernel/pe_win32.h>
#include <assert.h>

void test_registry_create_open(void) {
    registry_init();
    
    // Create key
    HKEY hkey;
    LONG result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Test",
                                   0, NULL, 0, 0, NULL, &hkey, NULL);
    assert(result == ERROR_SUCCESS);
    assert(hkey != NULL);
    
    printf("✓ Registry: create key\n");
    
    // Close key
    result = RegCloseKey(hkey);
    assert(result == ERROR_SUCCESS);
    
    // Open existing key
    result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Test",
                          0, 0, &hkey);
    assert(result == ERROR_SUCCESS);
    
    printf("✓ Registry: open existing key\n");
    
    RegCloseKey(hkey);
}

void test_registry_values(void) {
    HKEY hkey;
    RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Test",
                    0, NULL, 0, 0, NULL, &hkey, NULL);
    
    // Set value
    const WCHAR *data = L"TestValue";
    LONG result = RegSetValueExW(hkey, L"TestName", 0, REG_SZ,
                                 (const BYTE *)data,
                                 (lstrlenW(data) + 1) * sizeof(WCHAR));
    assert(result == ERROR_SUCCESS);
    
    printf("✓ Registry: set value\n");
    
    // Query value
    WCHAR buffer[256];
    DWORD size = sizeof(buffer);
    DWORD type;
    
    result = RegQueryValueExW(hkey, L"TestName", NULL, &type,
                             (BYTE *)buffer, &size);
    assert(result == ERROR_SUCCESS);
    assert(type == REG_SZ);
    assert(wcscmp(buffer, data) == 0);
    
    printf("✓ Registry: query value\n");
    
    RegCloseKey(hkey);
}
```

## Integration Tests

### 5. Simple Console Program

**hello.exe test:**

```c
// hello.c - Compile with MSVC or MinGW-w64
#include <windows.h>
#include <stdio.h>

int main(void) {
    printf("Hello from Windows PE!\n");
    
    DWORD pid = GetCurrentProcessId();
    printf("Process ID: %lu\n", pid);
    
    DWORD tick = GetTickCount();
    printf("Uptime: %lu ms\n", tick);
    
    return 0;
}
```

**Test execution:**

```bash
# Load and execute
./automation_os --load-pe /windows/test/hello.exe

# Expected output:
# PE: Loading executable: /windows/test/hello.exe
# PE: Parsed EXE (3 sections, 1 imports)
# PE: Loaded image at 0x140000000 (size 0x10000)
# PE: Resolving 1 imports
# PE: Loading import DLL: kernel32.dll
# PE: Resolved 5 functions from kernel32.dll
# PE: Entry point: 0x140001000
# PE: Starting execution
# Hello from Windows PE!
# Process ID: 1
# Uptime: 1234 ms
```

### 6. File I/O Test

**fileio.exe test:**

```c
// fileio.c
#include <windows.h>
#include <stdio.h>

int main(void) {
    // Write file
    HANDLE hFile = CreateFileW(L"test.txt", GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        printf("Failed to create file\n");
        return 1;
    }
    
    const char *data = "Test data\n";
    DWORD written;
    WriteFile(hFile, data, strlen(data), &written, NULL);
    CloseHandle(hFile);
    
    printf("Wrote %lu bytes\n", written);
    
    // Read file
    hFile = CreateFileW(L"test.txt", GENERIC_READ, 0, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        printf("Failed to open file\n");
        return 1;
    }
    
    char buffer[256];
    DWORD read;
    ReadFile(hFile, buffer, sizeof(buffer), &read, NULL);
    buffer[read] = '\0';
    
    printf("Read %lu bytes: %s", read, buffer);
    
    CloseHandle(hFile);
    return 0;
}
```

### 7. Multi-threading Test

**threads.exe test:**

```c
// threads.c
#include <windows.h>
#include <stdio.h>

DWORD WINAPI thread_func(LPVOID param) {
    int id = (int)(DWORD_PTR)param;
    
    for (int i = 0; i < 5; i++) {
        printf("Thread %d: iteration %d\n", id, i);
        Sleep(100);
    }
    
    return 0;
}

int main(void) {
    HANDLE threads[4];
    
    // Create threads
    for (int i = 0; i < 4; i++) {
        threads[i] = CreateThread(NULL, 0, thread_func,
                                 (LPVOID)(DWORD_PTR)i, 0, NULL);
        if (threads[i] == NULL) {
            printf("Failed to create thread %d\n", i);
            return 1;
        }
    }
    
    printf("Created 4 threads\n");
    
    // Wait for all threads
    for (int i = 0; i < 4; i++) {
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
    }
    
    printf("All threads completed\n");
    return 0;
}
```

### 8. DLL Import Test

**dlls.exe test:**

```c
// dlls.c
#include <windows.h>
#include <stdio.h>

int main(void) {
    // LoadLibrary
    HMODULE hKernel32 = LoadLibraryW(L"kernel32.dll");
    if (!hKernel32) {
        printf("Failed to load kernel32.dll\n");
        return 1;
    }
    
    printf("Loaded kernel32.dll at %p\n", hKernel32);
    
    // GetProcAddress
    typedef DWORD (WINAPI *GetTickCount_t)(VOID);
    GetTickCount_t pGetTickCount = (GetTickCount_t)GetProcAddress(hKernel32, "GetTickCount");
    
    if (pGetTickCount) {
        DWORD tick = pGetTickCount();
        printf("GetTickCount: %lu ms\n", tick);
    } else {
        printf("Failed to get GetTickCount\n");
    }
    
    FreeLibrary(hKernel32);
    return 0;
}
```

## Real-World Application Tests

### 9. Notepad.exe

```bash
# Copy Windows Notepad
cp /mnt/c/Windows/System32/notepad.exe /windows/programs/

# Try to execute
./automation_os --load-pe /windows/programs/notepad.exe

# Expected:
# - PE loads successfully
# - Imports resolve (kernel32, user32, gdi32)
# - Window creation stubs execute
# - Program runs (no rendering, but no crash)
```

### 10. cmd.exe

```bash
# Copy cmd.exe
cp /mnt/c/Windows/System32/cmd.exe /windows/programs/

# Execute
./automation_os --load-pe /windows/programs/cmd.exe

# Expected:
# - Console initialization
# - Command prompt display
# - Input handling (limited)
```

## Automated Test Suite

### test_suite.sh

```bash
#!/bin/bash

echo "=== AutomationOS PE Loader Test Suite ==="
echo

# Unit tests
echo "Running unit tests..."
./test_pe_parser
./test_dll_loader
./test_handles
./test_registry
echo

# Integration tests
echo "Running integration tests..."

# Test 1: Hello world
echo "Test 1: Hello World"
./automation_os --load-pe /windows/test/hello.exe > test1.log 2>&1
if grep -q "Hello from Windows PE" test1.log; then
    echo "✓ PASS"
else
    echo "✗ FAIL"
fi

# Test 2: File I/O
echo "Test 2: File I/O"
./automation_os --load-pe /windows/test/fileio.exe > test2.log 2>&1
if grep -q "Read.*bytes" test2.log; then
    echo "✓ PASS"
else
    echo "✗ FAIL"
fi

# Test 3: Threading
echo "Test 3: Multi-threading"
./automation_os --load-pe /windows/test/threads.exe > test3.log 2>&1
if grep -q "All threads completed" test3.log; then
    echo "✓ PASS"
else
    echo "✗ FAIL"
fi

# Test 4: DLL loading
echo "Test 4: DLL Loading"
./automation_os --load-pe /windows/test/dlls.exe > test4.log 2>&1
if grep -q "Loaded kernel32.dll" test4.log; then
    echo "✓ PASS"
else
    echo "✗ FAIL"
fi

echo
echo "=== Test Suite Complete ==="
```

**Run test suite:**

```bash
chmod +x test_suite.sh
./test_suite.sh
```

## Performance Testing

### Benchmark PE Loading

```c
// bench_pe_load.c
#include <kernel/pe_loader.h>
#include <time.h>
#include <stdio.h>

void benchmark_pe_load(const char *exe_path, int iterations) {
    clock_t start = clock();
    
    for (int i = 0; i < iterations; i++) {
        pe_file_t *pe = pe_parse_file(exe_path);
        if (pe) {
            pe_load(pe, NULL);
            pe_free(pe);
        }
    }
    
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double avg = elapsed / iterations;
    
    printf("%s: %d iterations in %.3f sec (%.3f ms avg)\n",
           exe_path, iterations, elapsed, avg * 1000);
}

int main(void) {
    printf("=== PE Loader Benchmarks ===\n");
    
    benchmark_pe_load("/windows/test/hello.exe", 100);
    benchmark_pe_load("/windows/test/fileio.exe", 100);
    benchmark_pe_load("/windows/test/threads.exe", 100);
    
    return 0;
}
```

### Expected Performance

- **PE parsing**: < 1 ms for small executables
- **DLL loading**: < 5 ms with cache, < 50 ms without
- **Symbol resolution**: < 0.1 ms per function
- **Memory allocation**: < 10 ms for 10 MB image
- **Overall startup**: < 100 ms for typical application

## Debug and Troubleshooting

### Enable Debug Logging

```bash
# Set debug level
export PE_DEBUG=1

# Run with verbose output
./automation_os --load-pe --verbose /windows/test/hello.exe
```

### Common Issues

**1. DLL Not Found**

```
Error: Failed to find DLL: user32.dll
```

Solution: Ensure DLL is in search path or use stub DLL.

**2. Import Resolution Failure**

```
Error: Failed to resolve import: CreateWindowExW
```

Solution: Implement stub function or use forwarding.

**3. Relocation Failure**

```
Error: Failed to apply relocations
```

Solution: Check if executable has relocation table.

**4. Memory Allocation Failure**

```
Error: Failed to allocate memory at preferred base
```

Solution: Try allocating at different address or increase available memory.

### Debugging Tools

**PE dump tool:**

```bash
# Display PE information
./pe_dump /windows/test/hello.exe

# Output:
# Image Base: 0x140000000
# Entry Point: 0x1000
# Sections: 3
#   .text: RVA 0x1000, Size 0x2000
#   .data: RVA 0x3000, Size 0x1000
#   .rdata: RVA 0x4000, Size 0x1000
# Imports: 1
#   kernel32.dll (5 functions)
```

**Handle viewer:**

```bash
# Display open handles
./handle_viewer

# Output:
# Handle 0x1: FILE fd=3 refs=1
# Handle 0x2: THREAD tid=100 refs=1
# Handle 0x3: MUTEX refs=1
```

## See Also

- `PE_LOADER_ARCHITECTURE.md` - Architecture documentation
- `WIN32_COMPATIBILITY.md` - API compatibility matrix
- `kernel/pe/` - Implementation source code
