# Stdio Buffering Implementation

## Overview

This implementation adds efficient I/O buffering to AutomationOS's libc, following the glibc pattern. The goal is to reduce syscalls by 1000x via 8KB buffers.

## Key Features

### 1. **FILE Structure with Buffering**
```c
typedef struct {
    int fd;              // File descriptor
    int flags;           // File flags (read/write/error/eof)
    int error;           // Error indicator
    int eof;             // EOF indicator
    char* buffer;        // I/O buffer (8KB default)
    size_t buf_size;     // Buffer size (BUFSIZ = 8192)
    size_t buf_pos;      // Current position in buffer
    size_t buf_count;    // Number of bytes in buffer
    int buf_mode;        // Buffering mode (_IOFBF/_IOLBF/_IONBF)
} FILE;
```

### 2. **Buffering Modes**

- **`_IOFBF` (Fully Buffered)**: Default for files
  - Accumulates writes until buffer is full (8KB)
  - Single syscall per 8192 bytes
  - ~1000x reduction for small writes

- **`_IOLBF` (Line Buffered)**: Default for stdout
  - Flushes on newline character
  - Good for interactive output
  - Balances responsiveness and efficiency

- **`_IONBF` (Unbuffered)**: Default for stderr
  - Every write goes directly to syscall
  - No buffering overhead
  - Ensures error messages appear immediately

### 3. **Standard Streams**

- **stdin**: 8KB buffer, fully buffered
- **stdout**: 8KB buffer, line buffered (flushes on '\n')
- **stderr**: No buffer, unbuffered (immediate output)

### 4. **Automatic Buffer Management**

#### Write Buffering (`fwrite`)
1. Checks if buffer is available
2. Copies data to buffer
3. Flushes when:
   - Buffer is full (fully buffered)
   - Newline encountered (line buffered)
   - `fflush()` called explicitly
   - File is closed

#### Read Buffering (`fread`)
1. Checks if buffer has data
2. If empty, reads 8KB chunk from file
3. Returns data from buffer
4. Minimizes syscalls for small reads

### 5. **Key Functions**

#### `fopen()`
- Allocates FILE structure
- Allocates 8KB buffer (BUFSIZ)
- Sets default buffering mode (_IOFBF)

#### `fwrite()`
- Copies data to buffer
- Flushes when buffer is full or newline (line buffered)
- Returns number of elements written

#### `fread()`
- Fills buffer with 8KB chunks
- Returns data from buffer
- Minimizes syscalls

#### `fflush()`
- Writes buffered data to file descriptor
- Resets buffer position
- Returns 0 on success, EOF on error

#### `fclose()`
- Flushes any pending writes
- Closes file descriptor
- Frees buffer and FILE structure

#### `setvbuf()`
- Changes buffering mode and size
- Must be called before any I/O operations

### 6. **Position Tracking**

#### `fseek()`
- Flushes write buffers before seeking
- Invalidates read buffers
- Updates file position

#### `ftell()`
- Returns logical position
- Accounts for buffered data:
  - Adds unflushed write buffer size
  - Subtracts unconsumed read buffer size

## Performance Impact

### Before (Unbuffered)
```c
for (int i = 0; i < 10000; i++) {
    fputc('X', f);  // 10,000 syscalls
}
```
**Result**: ~10,000 write() syscalls

### After (Buffered)
```c
for (int i = 0; i < 10000; i++) {
    fputc('X', f);  // Accumulates in 8KB buffer
}
// Automatic flush at 8192 bytes
// Final flush on fclose()
```
**Result**: ~2 write() syscalls (1 full buffer + 1 remainder)

**Improvement**: ~5000x reduction in syscalls!

## Usage Examples

### Example 1: Default Buffering
```c
FILE* f = fopen("output.txt", "w");
fprintf(f, "Hello, world!\n");  // Buffered
fclose(f);  // Automatic flush
```

### Example 2: Explicit Flush
```c
FILE* f = fopen("log.txt", "w");
fprintf(f, "Critical error\n");
fflush(f);  // Ensure written immediately
// Continue...
fclose(f);
```

### Example 3: Change Buffering Mode
```c
FILE* f = fopen("stream.txt", "w");
setvbuf(f, NULL, _IONBF, 0);  // Disable buffering
fputs("Unbuffered write\n", f);  // Direct syscall
fclose(f);
```

### Example 4: Line Buffering
```c
FILE* f = fopen("interactive.txt", "w");
setvbuf(f, NULL, _IOLBF, BUFSIZ);  // Line buffered
fprintf(f, "Line 1\n");  // Flushes immediately
fprintf(f, "Line 2");    // Stays in buffer
fprintf(f, "\n");        // Flushes now
fclose(f);
```

## Testing

### Unit Tests
Run comprehensive buffering tests:
```bash
cd userspace/tests
make
# Run on AutomationOS:
./test_stdio_buffering
```

Tests verify:
- Basic buffering functionality
- Buffer flush on full
- Explicit fflush()
- Line buffering behavior
- Unbuffered mode
- Multiple small writes (fputc)
- Formatted output (fprintf)
- Read buffering

### Benchmarks
Measure performance improvement:
```bash
./bench_stdio
```

Benchmark scenarios:
1. Unbuffered I/O (baseline)
2. Buffered I/O (8KB buffer)
3. Large single writes
4. Line-buffered output
5. Realistic mixed usage

Expected results:
- **Unbuffered**: ~10,000 syscalls for 10,000 bytes
- **Buffered**: ~2 syscalls for 10,000 bytes
- **Speedup**: ~5000x reduction in syscalls

## Implementation Notes

### Memory Management
- Buffers allocated via `malloc()` in `fopen()`
- Freed in `fclose()`
- Standard streams use static buffers (stdin, stdout)
- `setvbuf()` can use user-provided or auto-allocated buffers

### Edge Cases
- **Buffer full**: Automatic flush before continuing
- **Mixed I/O**: Flush write buffer before reading
- **Seek operations**: Flush writes, invalidate reads
- **Error handling**: Set error flag, return EOF
- **Standard streams**: Cannot be freed (static allocation)

### Compatibility
- Follows POSIX/C standard stdio semantics
- Compatible with existing code using printf/fprintf
- Drop-in replacement for unbuffered I/O

## Integration

The buffering implementation is integrated into:
- `userspace/libc/stdio.c` - Core implementation
- `userspace/libc/stdio.h` - Public API
- All existing code automatically benefits from buffering

No changes required to existing applications - buffering is transparent.

## Future Enhancements

Potential improvements:
1. **Read-ahead**: Prefetch data for sequential reads
2. **Write-behind**: Async buffer flushing
3. **Memory-mapped I/O**: For large files
4. **Dynamic buffer sizing**: Adapt to access patterns
5. **Buffer pooling**: Reduce malloc/free overhead

## References

- POSIX.1-2017 Section 2.5 (Standard I/O Streams)
- glibc libio implementation
- C11 Standard Section 7.21 (Input/output)
