# AutomationOS Dynamic Linker (ld.so)

## Overview

This is the dynamic linker implementation for AutomationOS, providing runtime loading and linking of shared libraries (`.so` files). It implements the ELF64 dynamic linking specification for x86-64 architecture.

## Architecture

### Components

1. **linker.c** - Main linker logic
   - Object loading and dependency resolution
   - Initialization and finalization
   - Library search path management

2. **elf_dyn.c** - Dynamic section parsing
   - Parse PT_DYNAMIC segment
   - Extract DT_* entries (NEEDED, STRTAB, SYMTAB, etc.)
   - Validate dynamic section consistency

3. **symbol_resolution.c** - Symbol lookup
   - Standard ELF hash table lookup
   - GNU hash table lookup (faster)
   - Global symbol resolution with proper scoping

4. **relocation.c** - Relocation processing
   - R_X86_64_* relocation types
   - Immediate binding (RTLD_NOW)
   - Lazy binding (RTLD_LAZY) preparation

5. **plt_got.c** - PLT/GOT management
   - GOT initialization
   - Lazy binding resolver
   - PLT stub setup

6. **dlopen.c** - Dynamic loading API
   - dlopen() - Load library at runtime
   - dlsym() - Symbol lookup
   - dlclose() - Unload library
   - dlerror() - Error reporting

7. **ld_main.c** - Entry point
   - Kernel invokes ld.so via PT_INTERP
   - Parse auxiliary vector
   - Load and link program
   - Transfer control to application

## Usage

### Installing ld.so

The dynamic linker is installed as `/lib64/ld-linux-x86-64.so.2`:

```bash
make
make install
```

### Building Shared Libraries

To build a shared library with position-independent code:

```bash
x86_64-elf-gcc -fPIC -c mylib.c -o mylib.o
x86_64-elf-ld -shared -o libmylib.so mylib.o
```

### Building Dynamically Linked Executables

To build an executable that uses shared libraries:

```bash
# Compile with -fPIC
x86_64-elf-gcc -fPIC -c main.c -o main.o

# Link with dynamic linker and shared libraries
x86_64-elf-ld -dynamic-linker /lib64/ld-linux-x86-64.so.2 \
    -o myapp main.o -L. -lmylib
```

The linker will automatically add a PT_INTERP segment pointing to ld.so.

### Converting Static Libraries to Shared

To convert an existing static library (`.a`) to shared (`.so`):

```bash
# Extract object files
ar x libc.a

# Rebuild with -fPIC
x86_64-elf-gcc -fPIC -c *.c

# Create shared library
x86_64-elf-ld -shared -o libc.so.1 *.o -soname libc.so.1
```

## ELF Dynamic Linking Process

### 1. Kernel Loads Program

When executing a dynamically linked program:

1. Kernel reads ELF header
2. Finds PT_INTERP segment (points to ld.so)
3. Loads ld.so into memory
4. Loads main executable into memory
5. Sets up auxiliary vector (AT_PHDR, AT_ENTRY, etc.)
6. Jumps to ld.so entry point

### 2. ld.so Takes Control

The dynamic linker:

1. Parses auxiliary vector from stack
2. Finds main executable's PT_DYNAMIC segment
3. Recursively loads all DT_NEEDED dependencies
4. Processes all relocations (R_X86_64_* types)
5. Sets up PLT/GOT for lazy binding (if RTLD_LAZY)
6. Runs DT_INIT and DT_INIT_ARRAY functions
7. Transfers control to main executable's entry point

### 3. Runtime Symbol Resolution

**Immediate Binding (RTLD_NOW):**
- All symbols resolved at load time
- No lazy binding overhead
- Slower startup, faster runtime

**Lazy Binding (RTLD_LAZY):**
- Symbols resolved on first call
- Fast startup, slight runtime overhead
- GOT entries initially point to PLT resolver stub

## Supported Relocation Types

### Data Relocations
- `R_X86_64_64` - Direct 64-bit
- `R_X86_64_32` - Direct 32-bit zero-extended
- `R_X86_64_32S` - Direct 32-bit sign-extended
- `R_X86_64_16` - Direct 16-bit
- `R_X86_64_8` - Direct 8-bit
- `R_X86_64_GLOB_DAT` - Global data pointer
- `R_X86_64_RELATIVE` - Adjust by program base

### Code Relocations
- `R_X86_64_PC32` - PC-relative 32-bit
- `R_X86_64_PC64` - PC-relative 64-bit
- `R_X86_64_PLT32` - PLT entry (32-bit PC-relative)
- `R_X86_64_JUMP_SLOT` - PLT entry pointer

### GOT Relocations
- `R_X86_64_GOTPCREL` - GOT entry (PC-relative)
- `R_X86_64_GOT32` - GOT entry (32-bit)
- `R_X86_64_GOTOFF64` - Offset from GOT

### Special Relocations
- `R_X86_64_IRELATIVE` - Indirect function
- `R_X86_64_SIZE32/64` - Symbol size
- `R_X86_64_COPY` - Copy from shared object

### TLS Relocations (Not Yet Implemented)
- `R_X86_64_DTPMOD64` - TLS module ID
- `R_X86_64_DTPOFF64` - TLS offset
- `R_X86_64_TPOFF64` - TLS offset in initial block

## Symbol Resolution Algorithm

1. **DT_SYMBOLIC objects**: Search self first
2. **Main executable**: Search before dependencies
3. **Dependencies**: Breadth-first search in load order
4. **Global scope**: All RTLD_GLOBAL objects
5. **Weak symbols**: Resolve to 0 if undefined

## Hash Tables

### Standard ELF Hash
- Classic hash function (slower)
- Always supported
- Required in DT_HASH

### GNU Hash
- Faster lookup with Bloom filter
- Optional but recommended
- Specified in DT_GNU_HASH

The linker tries GNU hash first, falls back to ELF hash.

## Library Search Paths

Default search order:
1. `DT_RPATH` (deprecated, searched before LD_LIBRARY_PATH)
2. `LD_LIBRARY_PATH` environment variable
3. `DT_RUNPATH` (searched after LD_LIBRARY_PATH)
4. `/lib`
5. `/usr/lib`
6. `/lib64`
7. `/usr/lib64`

## Integration with Kernel

### Required Kernel Support

1. **ELF Loader Modifications** (`kernel/fs/elf_loader.c`)
   - Detect ET_DYN executables (position-independent)
   - Load PT_INTERP if present
   - Set up auxiliary vector on stack
   - Jump to ld.so instead of main executable

2. **Syscalls for File I/O**
   - `open()` - Open library files
   - `read()` - Read ELF headers
   - `mmap()` - Map shared objects into memory
   - `munmap()` - Unmap on dlclose()

3. **Auxiliary Vector** (Already implemented)
   - `AT_PHDR` - Program headers address
   - `AT_PHNUM` - Number of program headers
   - `AT_ENTRY` - Program entry point
   - `AT_BASE` - Interpreter base address

### Kernel Integration Steps

See `KERNEL_INTEGRATION.md` for detailed integration guide.

## Testing

### Unit Tests

Test individual components:

```bash
# Test symbol resolution
./test_symbol_resolution

# Test relocations
./test_relocations

# Test PLT/GOT
./test_plt_got
```

### Integration Tests

Test with real shared libraries:

```bash
# Build test library
make -C tests/libtest

# Build test program
make -C tests/app

# Run
./tests/app/test_app
```

### Debugging

Enable debug output:

```c
linker_ctx.debug = 1;
```

Or set environment variable:

```bash
export LD_DEBUG=all
```

## Performance Considerations

### Lazy Binding Overhead
- First call: ~100-200 cycles (resolver lookup)
- Subsequent calls: 1 cycle (direct jump)
- Recommendation: Use RTLD_NOW for small programs

### Symbol Lookup
- GNU hash: O(1) average case with Bloom filter
- ELF hash: O(n) worst case with chain walking
- Recommendation: Build with `--hash-style=gnu`

### Memory Overhead
- Each shared object: ~8KB metadata
- PLT/GOT: ~16 bytes per external function
- Recommendation: Combine related libraries

## Known Limitations

1. **TLS not yet supported**
   - Thread-local storage relocations return error
   - Single-threaded programs work fine

2. **No COPY relocations**
   - R_X86_64_COPY not implemented
   - Workaround: Use function wrappers

3. **No versioning**
   - Symbol versioning (DT_VERSYM) ignored
   - May cause ABI compatibility issues

4. **Simple error handling**
   - Basic error messages
   - No stack traces or detailed diagnostics

## Future Enhancements

1. **TLS Support**
   - Implement R_X86_64_TLS* relocations
   - Allocate TLS blocks per thread

2. **Symbol Versioning**
   - Parse DT_VERSYM, DT_VERDEF, DT_VERNEED
   - Support multiple symbol versions

3. **Security Features**
   - RELRO (read-only relocations)
   - BIND_NOW enforcement
   - Library path restrictions

4. **Debugging Support**
   - DT_DEBUG for GDB integration
   - Symbol map generation
   - Runtime profiling hooks

## References

- [ELF Specification](https://refspecs.linuxfoundation.org/elf/elf.pdf)
- [System V ABI AMD64](https://refspecs.linuxfoundation.org/elf/x86_64-abi-0.99.pdf)
- [GNU Hash ELF Sections](https://flapenguin.me/elf-dt-gnu-hash)
- [Dynamic Linking Overview](https://www.iecc.com/linker/)

## License

Part of AutomationOS kernel project.

## Contact

See main kernel documentation for contact information.
