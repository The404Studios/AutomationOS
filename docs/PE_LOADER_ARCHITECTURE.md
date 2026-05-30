# PE Loader Architecture

## Overview

The AutomationOS PE Loader provides **native Windows executable support**, enabling the kernel to load and execute Windows PE (Portable Executable) files including `.exe` and `.dll` files directly without emulation.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Windows Application                      │
│                      (example.exe)                           │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       │ Win32 API Calls
                       ▼
┌─────────────────────────────────────────────────────────────┐
│                  Win32 API Stubs                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │Kernel32  │  │ User32   │  │  GDI32   │  │ Registry │   │
│  │File I/O  │  │ Windows  │  │ Graphics │  │ System   │   │
│  │Memory    │  │ Messages │  │ Drawing  │  │          │   │
│  │Threads   │  │ Input    │  │ Fonts    │  │          │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       │ Translation Layer
                       ▼
┌─────────────────────────────────────────────────────────────┐
│                   PE Loader Core                             │
│  ┌──────────────┐  ┌──────────────┐  ┌─────────────────┐  │
│  │ PE Parser    │  │ DLL Loader   │  │ Handle Manager  │  │
│  │ - Headers    │  │ - LoadLibrary│  │ - File handles  │  │
│  │ - Sections   │  │ - GetProcAddr│  │ - Thread handles│  │
│  │ - Imports    │  │ - DLL cache  │  │ - Object refs   │  │
│  │ - Exports    │  │ - Search path│  │                 │  │
│  │ - Relocations│  │              │  │                 │  │
│  └──────────────┘  └──────────────┘  └─────────────────┘  │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       │ AutomationOS Syscalls
                       ▼
┌─────────────────────────────────────────────────────────────┐
│              AutomationOS Kernel                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │   VFS    │  │   VMM    │  │Scheduler │  │  Process │   │
│  │  I/O     │  │  Memory  │  │ Threads  │  │  Mgmt    │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## Core Components

### 1. PE Parser (`pe_loader.c`)

**Parses PE file format:**

- **DOS Header** - Legacy MZ header with PE offset
- **PE Header** - COFF header and optional header
- **Section Headers** - Code, data, resources
- **Import Directory** - Required DLLs and functions
- **Export Directory** - Exported functions (for DLLs)
- **Relocation Directory** - Address fixups
- **TLS Directory** - Thread-local storage

**Key Functions:**
```c
pe_file_t* pe_parse(const void *data, size_t size);
pe_file_t* pe_parse_file(const char *path);
int pe_load(pe_file_t *pe, void *preferred_base);
int pe_load_and_execute(const char *exe_path);
```

### 2. DLL Loader (`dll_loader.c`)

**Dynamic library loading:**

- **DLL Search** - System32, application directory, PATH
- **DLL Cache** - Reference-counted loaded DLLs
- **Symbol Resolution** - GetProcAddress by name/ordinal
- **DllMain Invocation** - DLL_PROCESS_ATTACH/DETACH
- **Forwarded Exports** - Handle DLL forwarding

**Key Functions:**
```c
dll_handle_t* dll_load(const char *dll_name);
void* dll_get_proc_address(dll_handle_t *dll, const char *func_name);
void* dll_get_proc_by_ordinal(dll_handle_t *dll, uint16_t ordinal);
void dll_free(dll_handle_t *dll);
```

### 3. Handle Management (`handles.c`)

**Windows-style object handles:**

- **Handle Table** - 4096 handle slots per process
- **Object Types** - Files, threads, processes, mutexes, events
- **Reference Counting** - Automatic resource management
- **Handle Inheritance** - Child process handle inheritance

**Handle Types:**
- `HANDLE_TYPE_FILE` - File descriptors
- `HANDLE_TYPE_THREAD` - Thread objects
- `HANDLE_TYPE_PROCESS` - Process objects
- `HANDLE_TYPE_MUTEX` - Mutex objects
- `HANDLE_TYPE_SEMAPHORE` - Semaphore objects
- `HANDLE_TYPE_EVENT` - Event objects

### 4. Win32 API Stubs

#### Kernel32.dll (`win32_kernel32.c`)

**File I/O:**
- `CreateFileW` - Open/create files
- `ReadFile` / `WriteFile` - File operations
- `GetFileSize` - Query file size
- `SetFilePointer` - Seek in file
- `CloseHandle` - Close handles

**Memory Management:**
- `VirtualAlloc` - Allocate virtual memory
- `VirtualFree` - Free virtual memory
- `VirtualProtect` - Change memory protection

**Process/Thread:**
- `CreateThread` - Create threads
- `ExitProcess` / `ExitThread` - Termination
- `GetCurrentProcessId` / `GetCurrentThreadId`
- `Sleep` - Suspend execution

**Synchronization:**
- `CreateMutexW` / `ReleaseMutex`
- `WaitForSingleObject` - Wait for object

**Time:**
- `GetSystemTime` - Get system time
- `GetTickCount` / `GetTickCount64` - Uptime
- `QueryPerformanceCounter` - High-resolution timer

#### User32.dll (`win32_user32.c`)

**Window Management:**
- `CreateWindowExW` - Create windows
- `DestroyWindow` - Destroy windows
- `ShowWindow` - Show/hide windows
- `UpdateWindow` - Update window

**Message Loop:**
- `GetMessageW` - Get message from queue
- `TranslateMessage` - Translate virtual keys
- `DispatchMessageW` - Dispatch to window procedure
- `PostQuitMessage` - Post quit message

**Input:**
- `GetAsyncKeyState` - Get key state
- `GetCursorPos` - Get cursor position

#### GDI32.dll (`win32_gdi32.c`)

**Device Contexts:**
- `BeginPaint` / `EndPaint` - Begin/end painting
- `GetDC` / `ReleaseDC` - Get/release DC

**Drawing:**
- `TextOutW` - Draw text
- `Rectangle` - Draw rectangle
- `LineTo` / `MoveToEx` - Draw lines
- `SetPixel` - Set pixel color

**GDI Objects:**
- `CreateSolidBrush` - Create brush
- `SelectObject` - Select object into DC
- `DeleteObject` - Delete GDI object

### 5. Registry Emulation (`registry.c`)

**File system mapping:**
```
HKEY_LOCAL_MACHINE  -> /etc/registry/machine/
HKEY_CURRENT_USER   -> ~/.registry/
HKEY_CLASSES_ROOT   -> /etc/registry/classes/
HKEY_USERS          -> /etc/registry/users/
```

**Registry APIs:**
- `RegOpenKeyExW` - Open registry key
- `RegQueryValueExW` - Read registry value
- `RegSetValueExW` - Write registry value
- `RegCloseKey` - Close registry key
- `RegCreateKeyExW` - Create registry key
- `RegDeleteKeyW` - Delete registry key

**Storage:**
- Keys are directories
- Values are files
- Binary data stored directly
- String data stored as UTF-8

## Loading Process

### 1. Parse PE File

```c
// Load PE file from disk
pe_file_t *pe = pe_parse_file("C:\\Windows\\notepad.exe");

// Validate headers
// - Check MZ signature
// - Check PE signature
// - Validate section headers
```

### 2. Allocate Memory

```c
// Allocate memory for image
void *base = vmm_alloc_at(proc->vmm, pe->image_base, pe->image_size);

// Map sections into memory
for (each section) {
    memcpy(base + section->virtual_address, section->data, section->size);
    vmm_protect(base + section->virtual_address, section->size, prot);
}
```

### 3. Resolve Imports

```c
// For each imported DLL
for (each import_descriptor) {
    // Load DLL
    dll_handle_t *dll = dll_load(dll_name);
    
    // Resolve functions
    for (each function) {
        void *addr = dll_get_proc_address(dll, func_name);
        *import_address_table = addr;
    }
}
```

### 4. Apply Relocations

```c
// Calculate delta if not loaded at preferred base
uint64_t delta = actual_base - pe->image_base;

// Apply relocations
for (each relocation_block) {
    for (each relocation_entry) {
        *target_address += delta;
    }
}
```

### 5. Call TLS Callbacks

```c
// Thread-local storage initialization
if (pe->tls_directory) {
    for (each tls_callback) {
        callback(base, DLL_PROCESS_ATTACH, NULL);
    }
}
```

### 6. Execute Entry Point

```c
// Set entry point
proc->entry_point = base + pe->entry_point;

// Start execution
scheduler_add_process(proc);
```

## Memory Layout

```
Virtual Address Space Layout:

0x0000000000000000  ┌──────────────────┐
                    │   NULL guard     │
0x0000000000010000  ├──────────────────┤
                    │                  │
                    │   User stack     │
                    │   (grows down)   │
                    │                  │
0x0000007FFFFFFFFF  ├──────────────────┤
                    │                  │
                    │   PE Image       │
                    │  - Headers       │
                    │  - .text (code)  │
                    │  - .data         │
                    │  - .rdata        │
                    │  - .bss          │
                    │                  │
                    ├──────────────────┤
                    │                  │
                    │  Loaded DLLs     │
                    │  - kernel32.dll  │
                    │  - user32.dll    │
                    │  - ntdll.dll     │
                    │                  │
0x00007FFFFFFFFFFF  ├──────────────────┤
                    │  Kernel space    │
0xFFFF800000000000  └──────────────────┘
```

## ABI Translation

### Windows x64 Calling Convention

**Arguments:** RCX, RDX, R8, R9, stack
**Return:** RAX
**Callee-saved:** RBX, RBP, RDI, RSI, RSP, R12-R15
**Caller-saved:** RAX, RCX, RDX, R8-R11

### System V AMD64 ABI (Linux)

**Arguments:** RDI, RSI, RDX, RCX, R8, R9, stack
**Return:** RAX
**Callee-saved:** RBX, RBP, R12-R15
**Caller-saved:** RAX, RCX, RDX, RSI, RDI, R8-R11

### Translation Bridge

```c
// Windows -> System V translation
void* abi_translate_call(void *windows_func, void *args) {
    // Save registers
    // Rearrange arguments: RCX->RDI, RDX->RSI, R8->RDX, R9->RCX
    // Call function
    // Restore registers
    // Return result
}
```

## Exception Handling

### Structured Exception Handling (SEH)

```c
__try {
    // Protected code
    *ptr = value;
} __except (filter_expression) {
    // Exception handler
    printf("Access violation\n");
} __finally {
    // Cleanup code (always executed)
    free(ptr);
}
```

### Implementation

- **Exception frames** - Chain of handlers on stack
- **Exception dispatcher** - Walks frame chain
- **Filter execution** - Determines handler selection
- **Unwinding** - Call finally blocks

## Performance Optimizations

### DLL Caching

- **Reference counting** - Keep loaded DLLs in memory
- **Symbol cache** - Cache GetProcAddress results
- **Lazy binding** - Resolve imports on first call

### Memory Efficiency

- **Shared sections** - Share read-only sections between processes
- **Demand paging** - Load sections on access
- **Memory-mapped files** - Map PE files directly

### Fast Path

- **Direct execution** - No emulation overhead
- **Native syscalls** - Direct kernel calls
- **JIT optimization** - Optimize hot paths

## Testing

### Test Suite

1. **Simple console programs** - Hello World, file I/O
2. **Notepad.exe** - Basic Win32 application
3. **Command-line tools** - cmd.exe, ping.exe
4. **DLL loading** - Dynamic library tests
5. **Threading** - Multi-threaded applications
6. **GUI applications** - Windows with User32/GDI32

### Validation

```bash
# Load and execute Windows executable
./automation_os --load-pe /windows/system32/notepad.exe

# Test DLL loading
./test_dll_loader kernel32.dll

# Test Win32 APIs
./test_win32_api
```

## Security Considerations

### Address Space Layout Randomization (ASLR)

- Randomize PE base address
- Randomize DLL load addresses
- Randomize stack location

### Data Execution Prevention (DEP)

- Mark code sections as executable
- Mark data sections as non-executable
- Prevent stack execution

### Handle Security

- Validate handle ownership
- Prevent handle hijacking
- Access control on handles

## Future Enhancements

1. **DirectX Support** - Graphics API translation
2. **.NET Support** - CoreCLR integration
3. **64-bit ARM** - ARM64EC support
4. **COM/OLE** - Component Object Model
5. **Named Pipes** - IPC support
6. **Advanced SEH** - Full exception handling
7. **Driver Loading** - Kernel driver support

## References

- Microsoft PE/COFF Specification
- Windows Internals (Russinovich, Solomon)
- Windows System Programming (Hart)
- PE Format Documentation (Microsoft Docs)

## See Also

- `WIN32_COMPATIBILITY.md` - API compatibility matrix
- `TESTING_GUIDE.md` - Testing procedures
- `kernel/pe/` - Implementation source code
