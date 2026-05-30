# Stdio Buffering - Quick Reference

## TL;DR
✓ **8KB buffers reduce syscalls by 1000x**  
✓ **Automatic** - no code changes needed  
✓ **Compatible** - drop-in replacement

## Buffering Modes

| Mode | Constant | When It Flushes | Use Case |
|------|----------|-----------------|----------|
| **Fully Buffered** | `_IOFBF` | Buffer full (8KB) | Files (default) |
| **Line Buffered** | `_IOLBF` | On newline (`\n`) | stdout (default) |
| **Unbuffered** | `_IONBF` | Every write | stderr (default) |

## Quick Examples

### Default Usage (Automatic Buffering)
```c
FILE* f = fopen("data.txt", "w");
for (int i = 0; i < 10000; i++) {
    fputc('X', f);  // Buffered! Only ~2 syscalls
}
fclose(f);  // Auto-flush
```

### Explicit Flush
```c
FILE* f = fopen("log.txt", "w");
fprintf(f, "Critical error: %s\n", msg);
fflush(f);  // Force write to disk NOW
// Continue...
fclose(f);
```

### Change Buffering Mode
```c
// Disable buffering
FILE* f = fopen("output.txt", "w");
setvbuf(f, NULL, _IONBF, 0);

// Custom buffer size
char mybuf[16384];
setvbuf(f, mybuf, _IOFBF, 16384);

// Line buffering
setvbuf(f, NULL, _IOLBF, BUFSIZ);
```

## Performance Impact

| Scenario | Before | After | Improvement |
|----------|--------|-------|-------------|
| 10,000 × 1-byte writes | 10,000 syscalls | 2 syscalls | **5000×** |
| 1,000 × 10-byte writes | 1,000 syscalls | 2 syscalls | **500×** |
| 100 × 100-byte writes | 100 syscalls | 2 syscalls | **50×** |

## Common Patterns

### Writing Log Files
```c
FILE* log = fopen("/var/log/app.log", "a");
// Fully buffered by default - efficient
for (int i = 0; i < 1000; i++) {
    fprintf(log, "[%d] Event %d\n", time, i);
}
fclose(log);  // Auto-flush
```

### Interactive Output
```c
// stdout is line-buffered by default
printf("Enter name: ");  // Flushes on newline
fflush(stdout);          // Force flush if no newline
scanf("%s", name);
```

### Error Messages
```c
// stderr is unbuffered - appears immediately
fprintf(stderr, "ERROR: %s\n", msg);
// No need to fflush(stderr)
```

### Binary Data
```c
FILE* f = fopen("image.bin", "wb");
size_t size = width * height * 3;
fwrite(pixels, 1, size, f);  // Buffered
fclose(f);
```

## When to Flush Manually

**DO flush when:**
- ✓ Writing critical data (errors, warnings)
- ✓ Before external programs read the file
- ✓ After writing database transactions
- ✓ Before waiting on user input

**DON'T flush when:**
- ✗ Writing lots of small data (defeats buffering!)
- ✗ After every line (unless truly interactive)
- ✗ Before fclose() (it flushes automatically)

## Buffer Control

```c
// Get buffer info
int mode = stream->buf_mode;
size_t size = stream->buf_size;

// Disable buffering
setvbuf(f, NULL, _IONBF, 0);

// Use default 8KB buffer
setvbuf(f, NULL, _IOFBF, BUFSIZ);

// Provide custom buffer
static char buf[32768];
setvbuf(f, buf, _IOFBF, 32768);
```

## Position Tracking

```c
FILE* f = fopen("data.bin", "r+");

// Write some data
fwrite(data, 100, 1, f);
long pos = ftell(f);  // Accounts for buffered data

// Seek flushes automatically
fseek(f, 0, SEEK_SET);  // Flushes write buffer

fclose(f);
```

## Gotchas

### ⚠️ Mixed Read/Write
```c
FILE* f = fopen("data.txt", "r+");

fwrite("hello", 5, 1, f);
// Must flush or seek before reading!
fflush(f);  // or fseek(f, 0, SEEK_CUR);

char buf[10];
fread(buf, 5, 1, f);
```

### ⚠️ Buffer vs File Position
```c
FILE* f = fopen("data.txt", "w");
fwrite("ABCD", 4, 1, f);

// ftell() includes buffered data
long pos = ftell(f);  // Returns 4

// lseek() doesn't know about buffer!
// Always use ftell(), not raw lseek()
```

### ⚠️ Don't Free Standard Streams
```c
// WRONG - will crash!
fclose(stdout);

// RIGHT - flush only
fflush(stdout);
```

## Debugging

### Check if data is buffered
```c
size_t pending = stream->buf_count;
if (pending > 0) {
    printf("Buffer has %zu bytes pending\n", pending);
}
```

### Force synchronization
```c
fflush(NULL);  // Flush ALL open streams
```

### Disable buffering for debugging
```c
FILE* f = fopen("debug.log", "w");
setvbuf(f, NULL, _IONBF, 0);  // See writes immediately
```

## API Summary

| Function | Purpose | Buffering Impact |
|----------|---------|------------------|
| `fopen()` | Open file | Allocates 8KB buffer |
| `fwrite()` | Write data | Accumulates in buffer |
| `fread()` | Read data | Uses read-ahead buffer |
| `fflush()` | Force write | Empties buffer to disk |
| `fclose()` | Close file | Auto-flush + free buffer |
| `fseek()` | Change position | Flushes write buffer |
| `ftell()` | Get position | Accounts for buffer |
| `setvbuf()` | Set mode/size | Must call before I/O |

## Constants

```c
#define BUFSIZ    8192   // Default buffer size
#define _IOFBF    0      // Fully buffered
#define _IOLBF    1      // Line buffered
#define _IONBF    2      // Unbuffered
```

## Standard Streams

```c
stdin  → 8KB buffer, fully buffered (_IOFBF)
stdout → 8KB buffer, line buffered (_IOLBF)
stderr → No buffer, unbuffered     (_IONBF)
```

## Compile & Test

```bash
# Build libc
cd userspace/libc
make

# Build tests
cd ../tests
make

# Run tests
./test_stdio_buffering  # Unit tests
./bench_stdio           # Performance benchmark
```

## More Info

- Full documentation: `STDIO_BUFFERING.md`
- Architecture diagrams: `BUFFERING_DIAGRAM.txt`
- Implementation details: `stdio.c`

---

**Key Takeaway**: Just use `fopen()`, `fwrite()`, `fclose()`. Buffering happens automatically. Your code gets 1000× faster syscall performance for free! 🚀
