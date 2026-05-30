# Test Execution Guide: User-Kernel Memory Copy Security

## Overview
This guide provides step-by-step instructions to test the `copy_from_user()` and `copy_to_user()` security implementation.

---

## Prerequisites

- GCC compiler installed
- Make utility available
- Kernel source code with security fixes applied

---

## Step 1: Build Unit Tests

### Linux/WSL/macOS:
```bash
cd tests/unit
make test_user_copy
```

### Windows (Command Prompt):
```cmd
cd tests\unit
gcc -Wall -Wextra -O2 -I..\..\kernel\include -DUSER_SPACE_END=0x0000800000000000ULL -o test_user_copy.exe test_user_copy.c ..\..\kernel\core\mem\vmm.c
```

### Expected Output:
```
gcc -Wall -Wextra -O2 -I../../kernel/include -DUSER_SPACE_END=0x0000800000000000ULL -o test_user_copy test_user_copy.c ../../kernel/core/mem/vmm.c
```

No warnings or errors should appear.

---

## Step 2: Run Unit Tests

### Linux/WSL/macOS:
```bash
./test_user_copy
```

### Windows:
```cmd
test_user_copy.exe
```

### Expected Output:
```
======================================
User-Kernel Memory Copy Tests
======================================

Test: copy_from_user() with valid user address
  PASS: copy_from_user() succeeded with valid user address
Test: copy_from_user() with kernel address
  PASS: copy_from_user() rejected kernel address
Test: copy_from_user() with NULL pointer
  PASS: copy_from_user() rejected NULL pointers
Test: copy_from_user() with zero size
  PASS: copy_from_user() rejected zero size
Test: copy_from_user() with address overflow
  PASS: copy_from_user() detected address overflow
Test: copy_from_user() crossing user/kernel boundary
  PASS: copy_from_user() rejected boundary-crossing address

Test: copy_to_user() with valid user address
  PASS: copy_to_user() succeeded with valid user address
Test: copy_to_user() with kernel address
  PASS: copy_to_user() rejected kernel address
Test: copy_to_user() with NULL pointer
  PASS: copy_to_user() rejected NULL pointers
Test: copy_to_user() with zero size
  PASS: copy_to_user() rejected zero size
Test: copy_to_user() with address overflow
  PASS: copy_to_user() detected address overflow
Test: copy_to_user() crossing user/kernel boundary
  PASS: copy_to_user() rejected boundary-crossing address

======================================
All tests passed!
======================================
```

### Test Results Interpretation:

✅ **All PASS** - Security implementation is working correctly  
❌ **Any FAIL** - Security vulnerability detected, review implementation

---

## Step 3: Verify Individual Tests

You can add verbose output to see exactly what's being tested:

### Modify test_user_copy.c (optional):
```c
// Add more debug output
printf("  Testing with address: %p\n", test_addr);
printf("  Expected: COPY_EFAULT, Got: %d\n", result);
```

### Rebuild and run:
```bash
make clean
make test_user_copy
./test_user_copy
```

---

## Step 4: Integration Testing (Optional)

### Build the kernel with security fixes:
```bash
cd ../..  # Back to kernel root
make clean
make kernel
```

### Run the kernel in QEMU:
```bash
make run
```

### Test from userspace:

Create a test userspace program:

```c
// userspace/test_security.c
#include <syscall.h>

int main() {
    char valid_buf[] = "Hello, kernel!";
    void* kernel_addr = (void*)0xFFFFFFFF80000000ULL;
    
    // Test 1: Valid write (should succeed)
    int64_t result = write(1, valid_buf, 14);
    if (result == 14) {
        write(1, "Test 1 PASS: Valid write\n", 25);
    } else {
        write(1, "Test 1 FAIL\n", 12);
    }
    
    // Test 2: Kernel address (should fail with -EFAULT = -14)
    result = write(1, kernel_addr, 10);
    if (result == -14) {
        write(1, "Test 2 PASS: Kernel addr blocked\n", 33);
    } else {
        write(1, "Test 2 FAIL: Kernel addr NOT blocked!\n", 38);
    }
    
    // Test 3: NULL pointer (should fail)
    result = write(1, (void*)0, 10);
    if (result == -14) {
        write(1, "Test 3 PASS: NULL blocked\n", 26);
    } else {
        write(1, "Test 3 FAIL\n", 12);
    }
    
    return 0;
}
```

### Compile and run in kernel:
```bash
make -C userspace test_security
# Load and run in kernel
```

---

## Step 5: Security Audit Checklist

### Manual Code Review:

- [ ] No direct user pointer dereferences in kernel code
- [ ] All sys_read/sys_write use copy functions
- [ ] Error paths properly free allocated memory
- [ ] Return values are always checked
- [ ] Size validation before allocation

### Verification Commands:

```bash
# Check for direct user pointer access (should find none in syscalls)
grep -n "\*.*user_" kernel/core/syscall/handlers.c

# Verify copy functions are used
grep -n "copy_from_user\|copy_to_user" kernel/core/syscall/handlers.c

# Check error handling
grep -n "kfree" kernel/core/syscall/handlers.c
```

---

## Step 6: Performance Testing (Optional)

### Create benchmark:

```c
// tests/bench/bench_user_copy.c
#include <stdio.h>
#include <time.h>
#include "../../kernel/include/mem.h"

#define ITERATIONS 1000000

int main() {
    char src[1024];
    char dst[1024];
    
    clock_t start = clock();
    for (int i = 0; i < ITERATIONS; i++) {
        copy_from_user(dst, src, 1024);
    }
    clock_t end = clock();
    
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double per_call = (elapsed / ITERATIONS) * 1000000;  // microseconds
    
    printf("Benchmark Results:\n");
    printf("  Total time: %.3f seconds\n", elapsed);
    printf("  Per call: %.3f microseconds\n", per_call);
    printf("  Throughput: %.1f MB/s\n", 
           (1024.0 * ITERATIONS / (1024*1024)) / elapsed);
    
    return 0;
}
```

### Build and run:
```bash
cd tests/bench
make bench_user_copy
./bench_user_copy
```

---

## Troubleshooting

### Issue: Compilation errors

**Symptom:**
```
error: 'USER_SPACE_END' undeclared
```

**Solution:**
```bash
# Ensure constant is defined
gcc -DUSER_SPACE_END=0x0000800000000000ULL ...
```

---

### Issue: Tests fail with segmentation fault

**Symptom:**
```
Segmentation fault (core dumped)
```

**Solution:**
This is expected behavior when testing with actual kernel addresses in user mode. The tests use simulated addresses. In actual kernel, page fault handler would prevent segfault.

---

### Issue: kprintf not found

**Symptom:**
```
undefined reference to `kprintf'
```

**Solution:**
The unit test includes a mock kprintf. Ensure you're linking correctly:
```c
// test_user_copy.c includes this mock:
int kprintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vprintf(format, args);
    va_end(args);
    return ret;
}
```

---

## Test Coverage Report

The test suite covers:

### Security Tests (6 for copy_from_user, 6 for copy_to_user)
- ✅ Valid user space addresses
- ✅ Kernel address rejection (0xFFFFFFFF80000000)
- ✅ NULL pointer detection
- ✅ Zero-size rejection
- ✅ Integer overflow detection
- ✅ Boundary crossing prevention

### Edge Cases
- ✅ Address at USER_SPACE_END boundary
- ✅ Maximum valid user address
- ✅ Address wraparound scenarios
- ✅ Both source and destination validation

### Code Coverage
- ✅ All validation branches tested
- ✅ All error paths verified
- ✅ Success path confirmed

---

## Success Criteria

✅ **Pass:** All 12 tests pass  
✅ **Pass:** No compiler warnings  
✅ **Pass:** No memory leaks detected  
✅ **Pass:** Kernel addresses rejected  
✅ **Pass:** Valid addresses accepted  

❌ **Fail:** Any test case fails  
❌ **Fail:** Compilation warnings or errors  
❌ **Fail:** Kernel addresses accepted  
❌ **Fail:** Valid addresses rejected  

---

## Continuous Integration (Optional)

### Add to CI/CD Pipeline:

```yaml
# .github/workflows/security-tests.yml
name: Security Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Build tests
        run: |
          cd tests/unit
          make test_user_copy
      - name: Run tests
        run: |
          cd tests/unit
          ./test_user_copy
```

---

## Next Steps After Testing

1. ✅ All tests pass → Proceed to integration
2. ❌ Any test fails → Fix issues and retest
3. ✅ Integration successful → Performance benchmark
4. ✅ Performance acceptable → Security audit
5. ✅ Audit complete → Deploy to production

---

## Support

If tests fail or you need assistance:

1. Check `docs/SECURITY_COPY_USER_IMPLEMENTATION.md` for implementation details
2. Review `docs/DEVELOPER_GUIDE_USER_COPY.md` for usage patterns
3. Examine test output for specific failure points
4. Verify all files were updated correctly per `IMPLEMENTATION_CHECKLIST.md`

---

**Last Updated:** 2026-05-26  
**Test Suite Version:** 1.0  
**Status:** Ready for execution
