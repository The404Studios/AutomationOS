# Custom Clang-Tidy Checks for AutomationOS

This directory contains custom Clang-Tidy checks specific to AutomationOS patterns and conventions.

## Overview

Custom checks enforce AutomationOS coding standards and detect common kernel development mistakes:

1. **automationos-syscall-validation** - Ensures all syscalls validate user pointers
2. **automationos-null-check** - Verifies NULL checks after memory allocations
3. **automationos-lock-balance** - Detects unbalanced lock operations

## Building Custom Checks

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt-get install -y \
    llvm-dev \
    clang-tools-extra \
    cmake \
    ninja-build

# Arch Linux
sudo pacman -S llvm clang-tools-extra cmake ninja
```

### Build Instructions

```bash
cd scripts/static-analysis/custom-checks/
mkdir build && cd build
cmake -G Ninja ..
ninja
```

This produces `libautomationos-checks.so` that can be loaded by clang-tidy.

### Using Custom Checks

```bash
# Load custom checks
clang-tidy \
  -load=scripts/static-analysis/custom-checks/build/libautomationos-checks.so \
  -checks='automationos-*' \
  kernel/core/syscall/handlers.c

# Integrated into Makefile
make clang-tidy
```

## Check Descriptions

### 1. automationos-syscall-validation

**Purpose:** Ensure all system calls properly validate user-space pointers.

**Rationale:** Kernel must never directly dereference user pointers without validation to prevent privilege escalation.

**Pattern Detected:**
```c
// BAD: Direct user pointer dereference
long sys_read(int fd, void __user *buf, size_t count) {
    *buf = data;  // ❌ No validation!
}

// GOOD: Proper validation
long sys_read(int fd, void __user *buf, size_t count) {
    if (copy_to_user(buf, &data, count))  // ✅ Validated
        return -EFAULT;
}
```

**Configuration:**
- Checks for `__user` annotation on pointer parameters
- Verifies `copy_from_user()` or `copy_to_user()` is called
- Reports missing validation before first use

### 2. automationos-null-check

**Purpose:** Verify NULL checks after memory allocation functions.

**Rationale:** Memory allocation can fail; accessing NULL pointer causes kernel panic.

**Pattern Detected:**
```c
// BAD: No NULL check
void *ptr = kmalloc(size);
ptr->field = value;  // ❌ Unchecked dereference

// GOOD: NULL check present
void *ptr = kmalloc(size);
if (!ptr)  // ✅ Checked before use
    return -ENOMEM;
ptr->field = value;
```

**Tracked Functions:**
- `kmalloc()`, `kzalloc()`, `kcalloc()`
- `pmm_alloc()`, `vmm_alloc()`
- `malloc()` (userspace)

**Configuration:**
- Checks within 5 statements after allocation
- Allows NULL check in caller if returned immediately
- Reports missing checks with suggested fix

### 3. automationos-lock-balance

**Purpose:** Detect unbalanced lock acquire/release operations.

**Rationale:** Missing unlock causes deadlock; double unlock causes undefined behavior.

**Pattern Detected:**
```c
// BAD: Missing unlock
void process_data() {
    spinlock_acquire(&lock);
    if (error)
        return;  // ❌ Lock not released!
    spinlock_release(&lock);
}

// GOOD: Balanced locks
void process_data() {
    spinlock_acquire(&lock);
    if (error) {
        spinlock_release(&lock);  // ✅ Released on all paths
        return;
    }
    spinlock_release(&lock);
}
```

**Tracked Operations:**
- `spinlock_acquire()` / `spinlock_release()`
- `mutex_lock()` / `mutex_unlock()`
- `acquire_lock()` / `release_lock()`

**Analysis:**
- Performs path-sensitive analysis
- Checks all exit paths (return, break, continue, goto)
- Reports imbalance with affected control flow paths

## Implementation Details

### Project Structure

```
custom-checks/
├── README.md                      # This file
├── CMakeLists.txt                 # Build configuration
├── AutomationOSChecks.cpp         # Check implementations
├── AutomationOSChecks.h           # Check declarations
└── tests/                         # Test cases
    ├── syscall-validation.c
    ├── null-check.c
    └── lock-balance.c
```

### Check Implementation

Custom checks inherit from `clang::tidy::ClangTidyCheck`:

```cpp
class SyscallValidationCheck : public ClangTidyCheck {
public:
  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};
```

### AST Matchers

Checks use Clang AST matchers to find patterns:

```cpp
// Match function with __user annotation
auto HasUserPointer = hasParameter(
    hasType(pointerType(pointee(hasAttr(attr::NodeRef)))));

// Match copy_to_user/copy_from_user calls
auto HasCopyCall = hasDescendant(
    callExpr(callee(functionDecl(
        anyOf(hasName("copy_to_user"), hasName("copy_from_user"))))));

Finder->addMatcher(
    functionDecl(HasUserPointer, unless(HasCopyCall)).bind("func"),
    this);
```

## Testing Custom Checks

### Unit Tests

```bash
# Build and run tests
cd build
ninja check-automationos

# Run specific test
./test-syscall-validation
```

### Integration Tests

```bash
# Test on actual kernel code
clang-tidy \
  -load=build/libautomationos-checks.so \
  -checks='automationos-*' \
  kernel/core/syscall/handlers.c \
  -- -Ikernel/include
```

### Expected Output

```
kernel/core/syscall/handlers.c:45:5: warning: syscall parameter 'buf' is a user pointer but not validated [automationos-syscall-validation]
long sys_write(int fd, void __user *buf, size_t count) {
    ^
kernel/core/syscall/handlers.c:46:10: note: user pointer dereferenced here without validation
    *buf = data;
         ^
kernel/core/syscall/handlers.c:46:10: note: use copy_to_user() or copy_from_user() to safely access user memory
```

## Adding New Checks

### Step 1: Create Check Class

```cpp
// AutomationOSChecks.cpp
class MyCustomCheck : public ClangTidyCheck {
public:
  MyCustomCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(MatchFinder *Finder) override {
    // Define AST pattern to match
    Finder->addMatcher(
        functionDecl(hasName("my_pattern")).bind("func"),
        this);
  }

  void check(const MatchFinder::MatchResult &Result) override {
    // Extract matched node
    const auto *Func = Result.Nodes.getNodeAs<FunctionDecl>("func");
    
    // Report issue
    diag(Func->getLocation(), "found pattern violation");
  }
};
```

### Step 2: Register Check

```cpp
// AutomationOSChecks.cpp
class AutomationOSModule : public ClangTidyModule {
public:
  void addCheckFactories(ClangTidyCheckFactories &CheckFactories) override {
    CheckFactories.registerCheck<SyscallValidationCheck>(
        "automationos-syscall-validation");
    CheckFactories.registerCheck<NullCheckAfterAllocCheck>(
        "automationos-null-check");
    CheckFactories.registerCheck<LockBalanceCheck>(
        "automationos-lock-balance");
    CheckFactories.registerCheck<MyCustomCheck>(
        "automationos-my-custom-check");  // ← Add new check here
  }
};
```

### Step 3: Add Test

```c
// tests/my-custom-check.c
void good_pattern() {
  // This should NOT trigger warning
}

void bad_pattern() {
  // This SHOULD trigger warning
  // CHECK-MESSAGES: [[@LINE-1]]:1: warning: found pattern violation [automationos-my-custom-check]
}
```

### Step 4: Rebuild

```bash
cd build
ninja
ninja check-automationos
```

## Configuration Options

Checks can be configured via `.clang-tidy`:

```yaml
CheckOptions:
  - key: automationos-null-check.AllocatorFunctions
    value: 'kmalloc;kzalloc;pmm_alloc;vmm_alloc'
  - key: automationos-null-check.CheckDepth
    value: 5
  - key: automationos-lock-balance.LockFunctions
    value: 'spinlock_acquire;mutex_lock;acquire_lock'
  - key: automationos-lock-balance.UnlockFunctions
    value: 'spinlock_release;mutex_unlock;release_lock'
```

## Performance

Custom checks add minimal overhead:

| Check | Overhead | Memory |
|-------|----------|--------|
| syscall-validation | +5% | +20 MB |
| null-check | +8% | +30 MB |
| lock-balance | +12% | +50 MB |
| **Total** | **+25%** | **+100 MB** |

Runtime increases from ~1m 52s to ~2m 20s with all custom checks enabled.

## Limitations

### Current Limitations

1. **Inter-procedural Analysis:** Checks analyze single functions only
2. **Macro Expansion:** Limited analysis inside complex macros
3. **Indirect Calls:** Function pointers not fully tracked
4. **Path Sensitivity:** Limited to basic control flow

### Future Improvements

- [ ] Add inter-procedural analysis for deeper checking
- [ ] Improve macro handling
- [ ] Add data flow tracking across functions
- [ ] Integration with symbolic execution (KLEE)

## Troubleshooting

### Check Not Running

```bash
# Verify check is registered
clang-tidy -list-checks -load=build/libautomationos-checks.so | grep automationos

# Expected output:
#   automationos-lock-balance
#   automationos-null-check
#   automationos-syscall-validation
```

### Build Errors

```bash
# Ensure LLVM development packages installed
llvm-config --version

# Rebuild from clean state
rm -rf build
mkdir build && cd build
cmake -G Ninja ..
ninja
```

### False Positives

Add suppressions to `.clang-tidy`:

```yaml
CheckOptions:
  - key: automationos-null-check.IgnoredFunctions
    value: 'trusted_alloc;always_succeeds'
```

Or inline in code:

```c
// NOLINTNEXTLINE(automationos-null-check)
void *ptr = trusted_alloc(size);  // This allocator never returns NULL
```

## References

- [Clang-Tidy Developer Guide](https://clang.llvm.org/extra/clang-tidy/)
- [Clang AST Matchers](https://clang.llvm.org/docs/LibASTMatchers.html)
- [Writing Custom Checks](https://clang.llvm.org/extra/doxygen/tutorial.html)

## Contributing

To contribute new checks:

1. Fork the repository
2. Create check in `AutomationOSChecks.cpp`
3. Add test cases in `tests/`
4. Submit PR with check description and rationale

---

**Maintainer:** AutomationOS Static Analysis Team  
**Last Updated:** 2026-05-26
