# LibC Quick Start Guide

Get started with the userspace libc in 5 minutes.

## Building the Library

```bash
cd userspace/libc
make
```

Output: `../../build/userspace/libc/libc.a`

## Using in Your Application

### Example 1: Hello World

```c
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char* argv[]) {
    printf("Hello, World!\n");
    printf("You provided %d arguments\n", argc);
    return 0;
}
```

**Compile:**
```bash
gcc -nostdlib -ffreestanding -I../../userspace/libc \
    hello.c ../../build/userspace/libc/libc.a -o hello
```

### Example 2: String Conversion

```c
#include "stdio.h"
#include "stdlib.h"

int main(void) {
    const char* num_str = "42";
    int num = atoi(num_str);
    printf("String '%s' as integer: %d\n", num_str, num);

    const char* float_str = "3.14159";
    double pi = atof(float_str);
    printf("Pi is approximately: %d.%d\n", (int)pi, (int)((pi - (int)pi) * 100));

    return 0;
}
```

### Example 3: Sorting Array

```c
#include "stdio.h"
#include "stdlib.h"

int compare_ints(const void* a, const void* b) {
    return (*(int*)a - *(int*)b);
}

int main(void) {
    int numbers[] = {5, 2, 8, 1, 9, 3, 7};
    int n = sizeof(numbers) / sizeof(numbers[0]);

    printf("Before sorting: ");
    for (int i = 0; i < n; i++) {
        printf("%d ", numbers[i]);
    }
    printf("\n");

    qsort(numbers, n, sizeof(int), compare_ints);

    printf("After sorting:  ");
    for (int i = 0; i < n; i++) {
        printf("%d ", numbers[i]);
    }
    printf("\n");

    return 0;
}
```

### Example 4: Time and Date

```c
#include "stdio.h"
#include "time.h"

int main(void) {
    time_t now = time(NULL);
    struct tm* timeinfo = localtime(&now);

    char buffer[80];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    printf("Current time: %s\n", buffer);

    strftime(buffer, sizeof(buffer), "%A, %B %d, %Y", timeinfo);
    printf("Today is: %s\n", buffer);

    return 0;
}
```

### Example 5: File I/O

```c
#include "stdio.h"
#include "string.h"

int main(void) {
    // Write to file
    FILE* fp = fopen("test.txt", "w");
    if (fp) {
        fprintf(fp, "Hello from libc!\n");
        fprintf(fp, "Line 2\n");
        fclose(fp);
        printf("File written successfully\n");
    }

    // Read from file
    fp = fopen("test.txt", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            printf("Read: %s", line);
        }
        fclose(fp);
    }

    return 0;
}
```

### Example 6: Signal Handling

```c
#include "stdio.h"
#include "signal.h"
#include "stdlib.h"

void signal_handler(int sig) {
    printf("Caught signal %d: %s\n", sig, strsignal(sig));
}

int main(void) {
    // Install signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("Signal handlers installed\n");
    printf("Press Ctrl+C to test SIGINT\n");

    // Simulate signal
    raise(SIGUSR1);

    return 0;
}
```

## Common Patterns

### Safe String Copy

```c
#include "string.h"

void safe_copy(char* dest, size_t dest_size, const char* src) {
    size_t src_len = strlen(src);
    if (src_len >= dest_size) {
        src_len = dest_size - 1;
    }
    memcpy(dest, src, src_len);
    dest[src_len] = '\0';
}
```

### Number to String

```c
#include "stdio.h"

char buffer[32];
snprintf(buffer, sizeof(buffer), "%d", 12345);
// buffer now contains "12345"
```

### String to Number with Error Checking

```c
#include "stdlib.h"
#include "stdio.h"

char* endptr;
long value = strtol("123abc", &endptr, 10);
if (*endptr != '\0') {
    printf("Warning: not a pure number, stopped at: %s\n", endptr);
}
```

### Allocate and Initialize Array

```c
#include "stdlib.h"

int* array = (int*)calloc(100, sizeof(int));
if (array) {
    // Use array...
    free(array);  // Note: free() is currently a no-op
}
```

### Measure Time Difference

```c
#include "time.h"
#include "stdio.h"

time_t start = time(NULL);
// ... do work ...
time_t end = time(NULL);
printf("Elapsed: %.0f seconds\n", difftime(end, start));
```

## Available Headers

- `stdio.h` - Standard I/O
- `stdlib.h` - General utilities
- `string.h` - String operations
- `time.h` - Time and date
- `dirent.h` - Directory operations
- `signal.h` - Signal handling
- `syscall.h` - System calls (low-level)

## Common Errors

### Error: undefined reference to `lseek`

**Cause:** The kernel doesn't have a `lseek` syscall yet.

**Workaround:** Don't use `fseek`/`ftell` for now, or implement file positioning in application code.

### Error: `readdir` returns NULL

**Cause:** The kernel doesn't have a `getdents` syscall yet.

**Workaround:** Directory enumeration is not yet supported. Wait for kernel update.

### Error: Memory leak

**Cause:** `free()` is currently a no-op (bump allocator).

**Workaround:** Design applications to minimize allocations, or wait for proper allocator.

## Testing Your Code

Run the included test suite:

```bash
cd tests/unit
make test_libc
./test_libc
```

## Next Steps

1. Read the full documentation: `userspace/libc/README.md`
2. Browse example applications: `userspace/examples/`
3. Check kernel integration guide: `docs/USERSPACE_INTEGRATION.md`

## Getting Help

- Check README.md for detailed API documentation
- Look at test_libc.c for usage examples
- Review kernel syscall documentation
- Open an issue on the project tracker

## Performance Tips

1. **Use snprintf instead of sprintf** - safer
2. **Reuse buffers** - avoid repeated allocations
3. **Use memcpy for large copies** - optimized
4. **Cache strlen results** - avoid repeated calls
5. **Use qsort for large arrays** - O(n log n)

## Security Tips

1. **Always check return values** - especially for `malloc`, `fopen`
2. **Use snprintf not sprintf** - prevent buffer overflows
3. **Validate string lengths** - before copying
4. **Initialize memory** - use `calloc` or `memset`
5. **Check buffer sizes** - before `fread`/`fwrite`

---

**Happy coding!** 🚀
