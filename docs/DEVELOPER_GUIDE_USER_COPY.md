# Developer Guide: Safe User-Kernel Memory Copy

## Quick Start

When implementing system calls that interact with user space memory, always use the safe copy functions:

```c
#include "mem.h"

// Copy FROM user space TO kernel space
int copy_from_user(void* kernel_dst, const void* user_src, size_t n);

// Copy FROM kernel space TO user space
int copy_to_user(void* user_dst, const void* kernel_src, size_t n);
```

## Basic Usage

### Reading Data from User Space

```c
int64_t sys_example_read(uint64_t user_buf_addr, uint64_t size) {
    // Step 1: Allocate kernel buffer
    char* kernel_buf = kmalloc(size);
    if (!kernel_buf) {
        return -ENOMEM;
    }
    
    // Step 2: Copy from user space
    if (copy_from_user(kernel_buf, (void*)user_buf_addr, size) != COPY_SUCCESS) {
        kfree(kernel_buf);
        return -EFAULT;  // Invalid user address
    }
    
    // Step 3: Process data safely in kernel space
    process_data(kernel_buf, size);
    
    // Step 4: Clean up
    kfree(kernel_buf);
    return size;
}
```

### Writing Data to User Space

```c
int64_t sys_example_write(uint64_t user_buf_addr, uint64_t size) {
    // Step 1: Prepare data in kernel space
    char kernel_buf[256];
    prepare_data(kernel_buf, size);
    
    // Step 2: Copy to user space
    if (copy_to_user((void*)user_buf_addr, kernel_buf, size) != COPY_SUCCESS) {
        return -EFAULT;  // Invalid user address
    }
    
    return size;
}
```

### Reading and Writing

```c
int64_t sys_transform(uint64_t user_in_addr, uint64_t user_out_addr, uint64_t size) {
    // Allocate kernel buffers
    char* in_buf = kmalloc(size);
    char* out_buf = kmalloc(size);
    if (!in_buf || !out_buf) {
        kfree(in_buf);
        kfree(out_buf);
        return -ENOMEM;
    }
    
    // Read from user space
    if (copy_from_user(in_buf, (void*)user_in_addr, size) != COPY_SUCCESS) {
        kfree(in_buf);
        kfree(out_buf);
        return -EFAULT;
    }
    
    // Transform data
    transform(in_buf, out_buf, size);
    
    // Write to user space
    if (copy_to_user((void*)user_out_addr, out_buf, size) != COPY_SUCCESS) {
        kfree(in_buf);
        kfree(out_buf);
        return -EFAULT;
    }
    
    kfree(in_buf);
    kfree(out_buf);
    return size;
}
```

## Common Patterns

### Pattern 1: Small Fixed-Size Structures

```c
typedef struct {
    uint64_t field1;
    uint64_t field2;
    char name[32];
} user_struct_t;

int64_t sys_get_struct(uint64_t user_struct_addr) {
    user_struct_t kernel_struct;
    
    // Copy entire struct at once
    if (copy_from_user(&kernel_struct, (void*)user_struct_addr, 
                       sizeof(user_struct_t)) != COPY_SUCCESS) {
        return -EFAULT;
    }
    
    // Use the struct safely
    kprintf("Field1: %lu, Name: %s\n", kernel_struct.field1, kernel_struct.name);
    
    return 0;
}
```

### Pattern 2: Large Dynamic Buffers

```c
int64_t sys_process_large_buffer(uint64_t user_buf_addr, uint64_t size) {
    // Validate size
    if (size > MAX_BUFFER_SIZE) {
        return -EINVAL;
    }
    
    // Allocate kernel buffer
    void* kernel_buf = kmalloc(size);
    if (!kernel_buf) {
        return -ENOMEM;
    }
    
    // Copy and process
    if (copy_from_user(kernel_buf, (void*)user_buf_addr, size) != COPY_SUCCESS) {
        kfree(kernel_buf);
        return -EFAULT;
    }
    
    int64_t result = process_buffer(kernel_buf, size);
    
    kfree(kernel_buf);
    return result;
}
```

### Pattern 3: String Handling

```c
#define MAX_STRING_LEN 4096

int64_t sys_process_string(uint64_t user_str_addr) {
    // Allocate kernel buffer for string
    char* kernel_str = kmalloc(MAX_STRING_LEN);
    if (!kernel_str) {
        return -ENOMEM;
    }
    
    // Copy string from user space
    // Note: This assumes user provides null-terminated string
    if (copy_from_user(kernel_str, (void*)user_str_addr, MAX_STRING_LEN) != COPY_SUCCESS) {
        kfree(kernel_str);
        return -EFAULT;
    }
    
    // Ensure null termination (security!)
    kernel_str[MAX_STRING_LEN - 1] = '\0';
    
    // Process string safely
    kprintf("String: %s\n", kernel_str);
    
    kfree(kernel_str);
    return 0;
}
```

### Pattern 4: Multiple Small Copies

```c
int64_t sys_scatter_gather(uint64_t user_array_addr, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint64_t value;
        
        // Calculate address of array element
        void* elem_addr = (void*)(user_array_addr + i * sizeof(uint64_t));
        
        // Copy one element at a time
        if (copy_from_user(&value, elem_addr, sizeof(uint64_t)) != COPY_SUCCESS) {
            return -EFAULT;
        }
        
        process_value(value);
    }
    
    return count;
}
```

## Error Handling

### Always Check Return Values

```c
// ✅ CORRECT: Check return value
if (copy_from_user(kernel_buf, user_buf, size) != COPY_SUCCESS) {
    return -EFAULT;
}

// ❌ WRONG: Ignoring return value
copy_from_user(kernel_buf, user_buf, size);  // DANGEROUS!
```

### Clean Up on Failure

```c
// ✅ CORRECT: Free resources on error
char* buf1 = kmalloc(size1);
char* buf2 = kmalloc(size2);

if (copy_from_user(buf1, user_buf1, size1) != COPY_SUCCESS) {
    kfree(buf1);
    kfree(buf2);
    return -EFAULT;
}

// ❌ WRONG: Memory leak on error
char* buf1 = kmalloc(size1);
char* buf2 = kmalloc(size2);

if (copy_from_user(buf1, user_buf1, size1) != COPY_SUCCESS) {
    return -EFAULT;  // Leaked buf1 and buf2!
}
```

### Return Appropriate Error Codes

```c
int64_t sys_example(uint64_t user_buf, uint64_t size) {
    // Size validation
    if (size > MAX_SIZE) {
        return -EINVAL;  // Invalid argument
    }
    
    // Memory allocation
    void* buf = kmalloc(size);
    if (!buf) {
        return -ENOMEM;  // Out of memory
    }
    
    // User space copy
    if (copy_from_user(buf, (void*)user_buf, size) != COPY_SUCCESS) {
        kfree(buf);
        return -EFAULT;  // Bad address
    }
    
    // ... process ...
    
    kfree(buf);
    return 0;  // Success
}
```

## Security Considerations

### 1. Never Trust User Pointers

```c
// ❌ WRONG: Direct dereference
int64_t sys_bad(uint64_t user_ptr) {
    uint64_t value = *(uint64_t*)user_ptr;  // SECURITY VULNERABILITY!
    return value;
}

// ✅ CORRECT: Use copy function
int64_t sys_good(uint64_t user_ptr) {
    uint64_t value;
    if (copy_from_user(&value, (void*)user_ptr, sizeof(value)) != COPY_SUCCESS) {
        return -EFAULT;
    }
    return value;
}
```

### 2. Validate All Sizes

```c
// ✅ CORRECT: Validate before allocating
int64_t sys_safe(uint64_t user_buf, uint64_t size) {
    if (size == 0 || size > MAX_ALLOWED_SIZE) {
        return -EINVAL;
    }
    
    void* buf = kmalloc(size);
    // ... rest of function ...
}
```

### 3. Handle Partial Copies

```c
// Note: Current implementation does not support partial copies
// Either entire buffer is copied or operation fails with COPY_EFAULT
// Future enhancement: Return number of bytes copied
```

### 4. Time-of-Check Time-of-Use (TOCTOU)

```c
// ⚠️ CAREFUL: User data can change between copies
int64_t sys_check_then_use(uint64_t user_value_ptr) {
    uint64_t value;
    
    // First read
    if (copy_from_user(&value, (void*)user_value_ptr, sizeof(value)) != COPY_SUCCESS) {
        return -EFAULT;
    }
    
    // Security check
    if (value > MAX_VALUE) {
        return -EINVAL;
    }
    
    // Second read - value might have changed!
    if (copy_from_user(&value, (void*)user_value_ptr, sizeof(value)) != COPY_SUCCESS) {
        return -EFAULT;
    }
    
    // Solution: Use the first copy throughout
    // Don't re-read from user space after validation
}

// ✅ CORRECT: Copy once, use that copy
int64_t sys_safe_check(uint64_t user_value_ptr) {
    uint64_t value;
    
    // Copy once
    if (copy_from_user(&value, (void*)user_value_ptr, sizeof(value)) != COPY_SUCCESS) {
        return -EFAULT;
    }
    
    // Check the kernel copy
    if (value > MAX_VALUE) {
        return -EINVAL;
    }
    
    // Use the kernel copy (not user space)
    return process_value(value);
}
```

## Performance Tips

### 1. Minimize Copies

```c
// ❌ INEFFICIENT: Multiple small copies
for (int i = 0; i < count; i++) {
    uint64_t value;
    copy_from_user(&value, user_array + i, sizeof(uint64_t));
    process(value);
}

// ✅ EFFICIENT: One large copy
uint64_t* kernel_array = kmalloc(count * sizeof(uint64_t));
copy_from_user(kernel_array, user_array, count * sizeof(uint64_t));
for (int i = 0; i < count; i++) {
    process(kernel_array[i]);
}
kfree(kernel_array);
```

### 2. Stack vs Heap

```c
// ✅ GOOD: Use stack for small buffers
int64_t sys_small(uint64_t user_buf) {
    char kernel_buf[64];  // Stack allocation
    if (copy_from_user(kernel_buf, (void*)user_buf, 64) != COPY_SUCCESS) {
        return -EFAULT;
    }
    // No need to free
}

// ✅ GOOD: Use heap for large buffers
int64_t sys_large(uint64_t user_buf, uint64_t size) {
    char* kernel_buf = kmalloc(size);  // Heap allocation
    if (!kernel_buf) return -ENOMEM;
    
    if (copy_from_user(kernel_buf, (void*)user_buf, size) != COPY_SUCCESS) {
        kfree(kernel_buf);
        return -EFAULT;
    }
    
    // ... process ...
    kfree(kernel_buf);
}
```

### 3. Avoid Unnecessary Allocations

```c
// ❌ WASTEFUL: Allocate even if copy might fail
void* buf = kmalloc(size);
if (copy_from_user(buf, user_buf, size) != COPY_SUCCESS) {
    kfree(buf);
    return -EFAULT;
}

// Note: In current implementation, this is unavoidable since we need
// the kernel buffer for copy_from_user. Future optimization: validate
// address before allocating.
```

## Testing Your System Call

### Unit Test Template

```c
void test_my_syscall(void) {
    // Test 1: Normal operation
    char user_buf[] = "test data";
    int64_t result = sys_my_syscall((uint64_t)user_buf, strlen(user_buf));
    assert(result >= 0);
    
    // Test 2: Kernel address (should fail)
    void* kernel_addr = (void*)0xFFFFFFFF80000000ULL;
    result = sys_my_syscall((uint64_t)kernel_addr, 10);
    assert(result == -EFAULT);
    
    // Test 3: NULL pointer (should fail)
    result = sys_my_syscall(0, 10);
    assert(result == -EFAULT);
    
    // Test 4: Zero size
    result = sys_my_syscall((uint64_t)user_buf, 0);
    assert(result == 0 || result == -EINVAL);
}
```

## Common Mistakes

### Mistake 1: Forgetting to Free on Error Path

```c
// ❌ WRONG
char* buf = kmalloc(size);
if (copy_from_user(buf, user_buf, size) != COPY_SUCCESS) {
    return -EFAULT;  // Leaked buf!
}

// ✅ CORRECT
char* buf = kmalloc(size);
if (copy_from_user(buf, user_buf, size) != COPY_SUCCESS) {
    kfree(buf);
    return -EFAULT;
}
```

### Mistake 2: Using User Pointers Directly

```c
// ❌ WRONG
const char* user_str = (const char*)user_str_ptr;
kprintf("User said: %s\n", user_str);  // SECURITY HOLE!

// ✅ CORRECT
char kernel_str[256];
if (copy_from_user(kernel_str, (void*)user_str_ptr, 256) != COPY_SUCCESS) {
    return -EFAULT;
}
kernel_str[255] = '\0';  // Ensure null termination
kprintf("User said: %s\n", kernel_str);
```

### Mistake 3: Wrong Buffer Size

```c
// ❌ WRONG: Size mismatch
uint64_t value;
copy_from_user(&value, user_ptr, 4);  // Should be 8 bytes!

// ✅ CORRECT: Use sizeof
uint64_t value;
copy_from_user(&value, user_ptr, sizeof(value));
```

### Mistake 4: Not Checking Return Value

```c
// ❌ WRONG
copy_from_user(buf, user_buf, size);
process(buf);  // buf might be uninitialized!

// ✅ CORRECT
if (copy_from_user(buf, user_buf, size) != COPY_SUCCESS) {
    return -EFAULT;
}
process(buf);
```

## Reference

### Constants

```c
#define COPY_SUCCESS  0   // Copy successful
#define COPY_EFAULT  -1   // Copy failed (invalid address)
```

### Error Codes

```c
#define ESUCCESS     0    // Success
#define EINVAL     -22    // Invalid argument
#define EBADF       -9    // Bad file descriptor
#define ENOMEM     -12    // Out of memory
#define EFAULT     -14    // Bad address
```

### Memory Layout

```c
#define USER_SPACE_END      0x0000800000000000ULL
#define KERNEL_SPACE_START  0xFFFF800000000000ULL
```

### Helper Functions

```c
bool validate_user_buffer(const void* ptr, size_t size);
bool validate_user_string(const char* str, size_t max_len);
```

## Additional Resources

- Security Implementation: `docs/SECURITY_COPY_USER_IMPLEMENTATION.md`
- API Reference: `docs/API_REFERENCE.md`
- Test Suite: `tests/unit/test_user_copy.c`
