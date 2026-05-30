# SMP Test Suite Integration Guide

## Quick Start

### 1. Copy Test Files

```bash
# Copy test suite to kernel tree
cp test_smp.c ../kernel/tests/
cp smp_test.h ../kernel/tests/

# Make test runner executable
chmod +x run_smp_tests.sh
```

### 2. Modify Kernel Initialization

Add SMP tests to your kernel's main initialization:

**File: `kernel/init/main.c`**

```c
#include "../tests/smp_test.h"

void kernel_main(void) {
    // Early initialization
    console_init();
    mem_init();
    acpi_init();
    
    // Initialize SMP
    smp_init();
    smp_start_aps();
    ipi_init();
    
    // Run SMP validation tests
    #ifdef RUN_SMP_TESTS
    kprintf("\n[KERNEL] Running SMP validation tests...\n");
    smp_run_tests();
    kprintf("[KERNEL] SMP tests complete\n\n");
    #endif
    
    // Continue with rest of initialization
    scheduler_init();
    filesystem_init();
    // ...
}
```

### 3. Build with Tests

```bash
# Build kernel with SMP tests enabled
cd ..
make clean
make EXTRA_CFLAGS="-DRUN_SMP_TESTS"

# Or modify Makefile to add:
# CFLAGS += -DRUN_SMP_TESTS
```

### 4. Run Tests

```bash
cd tests/

# Run with QEMU
./run_smp_tests.sh

# Or manually
qemu-system-x86_64 \
    -kernel ../kernel.bin \
    -smp cpus=4 \
    -m 512M \
    -serial stdio
```

## Integration Options

### Option 1: Boot Parameter (Recommended)

Add command-line parameter to enable tests:

**File: `kernel/init/main.c`**

```c
#include "../tests/smp_test.h"

static bool run_smp_tests = false;

void parse_kernel_cmdline(const char* cmdline) {
    if (strstr(cmdline, "test_smp")) {
        run_smp_tests = true;
    }
}

void kernel_main(void) {
    // ... initialization ...
    
    if (run_smp_tests) {
        smp_run_tests();
    }
    
    // ... rest of init ...
}
```

Boot with: `kernel.bin test_smp`

### Option 2: Interactive Menu

Add to boot menu or shell:

**File: `userspace/bin/autoshell.c`**

```c
#include "../../kernel/tests/smp_test.h"

static void cmd_test_smp(int argc, char** argv) {
    printf("Running SMP validation tests...\n");
    smp_run_tests();
}

// Register command
register_command("test_smp", cmd_test_smp, "Run SMP validation tests");
```

Run from shell: `test_smp`

### Option 3: Always Run (Debug Builds)

**File: `kernel/Makefile`**

```makefile
ifeq ($(DEBUG),1)
CFLAGS += -DRUN_SMP_TESTS
endif
```

Build: `make DEBUG=1`

### Option 4: Separate Test Binary

Build standalone test binary:

```bash
cd tests/
make -f Makefile.smp
./test_smp
```

## Required Kernel Functions

Ensure these functions are available in your kernel:

### Memory Management
```c
void* kmalloc(size_t size);
void kfree(void* ptr);
```

### Console Output
```c
int kprintf(const char* format, ...);
int snprintf(char* buf, size_t size, const char* format, ...);
```

### String Operations
```c
void* memset(void* s, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
int strcmp(const char* s1, const char* s2);
char* strstr(const char* haystack, const char* needle);
```

### Synchronization
```c
void spin_lock_init(spinlock_t* lock);
void spin_lock(spinlock_t* lock);
void spin_unlock(spinlock_t* lock);
```

## Makefile Integration

### Update Main Makefile

**File: `kernel/Makefile`**

```makefile
# Add SMP test support
ifeq ($(RUN_SMP_TESTS),1)
CFLAGS += -DRUN_SMP_TESTS
TEST_OBJS = tests/test_smp.o
else
TEST_OBJS =
endif

# Add test objects to kernel build
OBJS += $(TEST_OBJS)

# Add test target
.PHONY: test-smp
test-smp: kernel.bin
	cd tests && ./run_smp_tests.sh
```

Build and test:
```bash
make RUN_SMP_TESTS=1
make test-smp
```

## Testing Workflow

### Development Cycle

1. **Make SMP changes**
   ```bash
   vim kernel/arch/x86_64/smp.c
   ```

2. **Build with tests**
   ```bash
   make clean
   make RUN_SMP_TESTS=1
   ```

3. **Run tests in QEMU**
   ```bash
   cd tests
   ./run_smp_tests.sh single 4 512
   ```

4. **Check results**
   ```bash
   cat smp_test_4cpu_512mb.log | grep -A 5 "TEST SUMMARY"
   ```

5. **Iterate until all tests pass**

### Pre-Commit Testing

Always run before committing SMP changes:

```bash
# Test multiple configurations
cd tests/
./run_smp_tests.sh all

# Check results
./run_smp_tests.sh results

# All tests should pass
```

### Continuous Integration

Add to CI pipeline:

```yaml
# .github/workflows/smp_tests.yml
name: SMP Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    strategy:
      matrix:
        cpus: [2, 4, 8]
    steps:
      - uses: actions/checkout@v2
      - name: Install QEMU
        run: sudo apt-get install -y qemu-system-x86
      - name: Build kernel
        run: make RUN_SMP_TESTS=1
      - name: Run SMP tests
        run: cd tests && ./run_smp_tests.sh single ${{ matrix.cpus }} 512
```

## Troubleshooting Integration

### Build Errors

**Error**: `undefined reference to 'smp_run_tests'`
- **Fix**: Ensure `test_smp.c` is compiled and linked
- **Check**: `OBJS` in Makefile includes test objects

**Error**: `smp_test.h: No such file or directory`
- **Fix**: Check include path in CFLAGS
- **Add**: `-I../kernel/tests` if needed

### Runtime Errors

**Error**: Tests hang or timeout
- **Fix**: Increase timeout in test runner
- **Check**: QEMU has enough CPU cores allocated

**Error**: Tests fail on real hardware but pass in QEMU
- **Fix**: Hardware-specific issue (see checklist)
- **Debug**: Enable SMP_DEBUG, check LAPIC errors

**Error**: Kernel panics during tests
- **Fix**: Stack overflow (increase stack size)
- **Check**: Per-CPU stacks allocated correctly

## Advanced Integration

### Custom Test Filters

Run specific tests only:

```c
// In kernel/tests/test_smp.c
void smp_run_selected_tests(uint32_t test_mask) {
    #define TEST_CPU_DETECTION    (1 << 0)
    #define TEST_AP_STARTUP       (1 << 1)
    #define TEST_PERCPU_ISOLATION (1 << 2)
    #define TEST_IPI_DELIVERY     (1 << 3)
    #define TEST_IPI_LATENCY      (1 << 4)
    #define TEST_TLB_SHOOTDOWN    (1 << 5)
    #define TEST_CACHE_COHERENCE  (1 << 6)
    #define TEST_PERFORMANCE      (1 << 7)
    #define TEST_STRESS           (1 << 8)

    if (test_mask & TEST_CPU_DETECTION) {
        test_cpu_detection(&result);
    }
    // ... etc
}
```

Usage:
```c
// Run only basic tests
smp_run_selected_tests(TEST_CPU_DETECTION | TEST_AP_STARTUP);
```

### Performance Profiling

Add profiling hooks:

```c
#ifdef SMP_PROFILE
#define SMP_PROFILE_START(name) \
    uint64_t profile_##name##_start = __rdtsc()

#define SMP_PROFILE_END(name) \
    do { \
        uint64_t elapsed = __rdtsc() - profile_##name##_start; \
        kprintf("[PROFILE] %s: %llu cycles\n", #name, elapsed); \
    } while(0)
#else
#define SMP_PROFILE_START(name)
#define SMP_PROFILE_END(name)
#endif
```

### Test Results Logging

Save results to file:

```c
void smp_save_results(const char* filename) {
    FILE* fp = fopen(filename, "w");
    if (!fp) return;

    fprintf(fp, "SMP Test Results\n");
    fprintf(fp, "================\n");
    fprintf(fp, "Date: %s\n", get_timestamp());
    fprintf(fp, "CPUs: %u\n", smp_num_cpus);
    fprintf(fp, "Tests Passed: %u\n", tests_passed);
    fprintf(fp, "Tests Failed: %u\n", tests_failed);
    // ... etc
    
    fclose(fp);
}
```

## Platform-Specific Notes

### x86_64
- Tests work out of the box
- Requires LAPIC support
- x2APIC detected automatically

### ARM64 (Future)
- Adapt for GICv3 instead of LAPIC
- Use SGI instead of IPI
- Modify AP startup sequence

### RISC-V (Future)
- Use PLIC for interrupts
- SBI calls for IPI
- Different CPU discovery method

## Maintenance

### When to Update Tests

Update tests when:
- Adding new SMP features
- Changing IPI implementation
- Modifying scheduler
- Changing memory management
- Adding CPU hotplug

### Test Coverage Goals

Maintain:
- 100% coverage of SMP API
- All IPI types tested
- All failure paths tested
- Performance regressions caught

## Support

For integration issues:
1. Check build logs for errors
2. Verify all dependencies present
3. Test in QEMU first
4. Enable debug output
5. Contact kernel team

## Checklist

Integration complete when:
- [ ] Tests compile without errors
- [ ] Tests run in QEMU
- [ ] All tests pass
- [ ] Tests integrated into CI
- [ ] Documentation updated
- [ ] Team trained on running tests

---

**Document Version**: 1.0  
**Last Updated**: 2026-05-26  
**Author**: AutomationOS Kernel Team
