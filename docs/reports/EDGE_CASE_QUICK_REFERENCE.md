# Edge Case Quick Reference Card

Quick reference for testing edge cases in AutomationOS.

## Quick Test Commands

```bash
# Run all edge case tests
./test_edge_cases.sh

# Run specific suite
make test-edge-cases
make test-race-conditions
make test-resource-exhaustion
```

## The 5 Critical Edge Cases

### 1. ZERO
```c
void* ptr = kmalloc(0);      // Must return NULL
kfree(NULL);                 // Must be safe
validate_buffer(buf, 0);     // Must reject
```

### 2. MAX
```c
void* ptr = kmalloc(UINT32_MAX);  // Must fail gracefully
sys_write(fd, buf, UINT64_MAX);   // Must reject
```

### 3. NULL
```c
process_ref(NULL);    // Must handle safely
kfree(NULL);          // Must handle safely
if (!ptr) return;     // Always check!
```

### 4. RACE
```c
// Two threads, one resource - only ONE succeeds
void* page1 = pmm_alloc_page();  // Thread 1
void* page2 = pmm_alloc_page();  // Thread 2
// Use spinlocks!
```

### 5. EXHAUSTION
```c
// Allocate until exhaustion
while (ptr = kmalloc(1024)) { }
// System must remain stable
// Next operation must work
```

## Common Bugs to Avoid

### NULL Dereference (CWE-476)
```c
// ❌ BAD
void destroy(obj_t* obj) {
    kfree(obj->data);  // What if obj is NULL?
}

// ✅ GOOD
void destroy(obj_t* obj) {
    if (!obj) return;
    kfree(obj->data);
}
```

### Integer Overflow (CWE-190)
```c
// ❌ BAD
bool valid = (ptr + size < LIMIT);  // Overflow!

// ✅ GOOD
uint64_t end = (uint64_t)ptr + size;
if (end < (uint64_t)ptr) return false;  // Check
bool valid = (end <= LIMIT);
```

### Double Free (CWE-415)
```c
// ❌ BAD
kfree(ptr);
kfree(ptr);  // Crash!

// ✅ GOOD
kfree(ptr);
ptr = NULL;  // Set to NULL after free
```

### Race Condition (CWE-362)
```c
// ❌ BAD
if (resource_available()) {
    use_resource();  // Race here!
}

// ✅ GOOD
spin_lock(&lock);
if (resource_available()) {
    use_resource();
}
spin_unlock(&lock);
```

### Memory Leak (CWE-401)
```c
// ❌ BAD
obj_t* obj = kmalloc(sizeof(obj_t));
if (!obj->init()) {
    return NULL;  // Leaked obj!
}

// ✅ GOOD
obj_t* obj = kmalloc(sizeof(obj_t));
if (!obj->init()) {
    kfree(obj);  // Clean up!
    return NULL;
}
```

## Code Review Checklist

When reviewing code, verify:

```
□ NULL checks on all pointer parameters
□ Integer overflow checks before arithmetic
□ Buffer size validation before memcpy
□ Locks held when accessing shared data
□ Memory freed on error paths (no leaks)
□ Pointers set to NULL after free
□ Reference counts updated atomically
□ Return codes checked and handled
```

## Test Template

```c
void test_my_edge_case(void) {
    TEST("Description of edge case");

    // Setup
    void* resource = allocate_resource();
    EXPECT(resource != NULL, "Setup failed");

    // Trigger edge case
    int result = edge_case_operation(resource);

    // Verify expected behavior
    EXPECT(result == EXPECTED, "Edge case not handled");
    EXPECT(system_stable(), "System unstable");

    // Cleanup
    free_resource(resource);

    kprintf("  PASS: Edge case handled\n");
}
```

## Memory Safety Rules

```c
// 1. Always validate size before allocation
if (size == 0 || size > MAX_SIZE) return NULL;

// 2. Always check allocation result
void* ptr = kmalloc(size);
if (!ptr) return ERROR;

// 3. Always validate pointer before dereference
if (!ptr) return;

// 4. Always check for overflow
uint64_t end = ptr + size;
if (end < ptr) return ERROR;  // Overflow

// 5. Always lock shared data
spin_lock(&lock);
// Access shared data
spin_unlock(&lock);

// 6. Always clean up on error
if (error) {
    kfree(allocated_memory);
    return ERROR;
}

// 7. Always set NULL after free
kfree(ptr);
ptr = NULL;
```

## Syscall Validation Pattern

```c
int64_t sys_operation(uint64_t arg1, uint64_t arg2, uint64_t size) {
    // 1. Validate size
    if (size == 0 || size > MAX_SIZE) return EINVAL;

    // 2. Validate pointer
    void* ptr = (void*)arg1;
    if (!validate_user_buffer(ptr, size)) return EFAULT;

    // 3. Validate range
    if (arg2 < 0 || arg2 >= MAX_VALUE) return EINVAL;

    // 4. Check resource availability
    if (!resource_available()) return ENOMEM;

    // 5. Perform operation
    return do_operation(ptr, arg2, size);
}
```

## Common Edge Case Patterns

### Pattern 1: Off-by-One
```c
// ❌ BAD
for (int i = 0; i <= size; i++)  // Overflow!

// ✅ GOOD
for (int i = 0; i < size; i++)
```

### Pattern 2: Signed/Unsigned Confusion
```c
// ❌ BAD
int size = -1;
if (size > 0) { }  // -1 as unsigned is huge!

// ✅ GOOD
if (size <= 0) return ERROR;
```

### Pattern 3: Boundary Crossing
```c
// ❌ BAD
if (ptr < USER_SPACE_END)  // What if ptr + size crosses?

// ✅ GOOD
if (ptr + size <= USER_SPACE_END && ptr + size >= ptr)
```

## Performance Tips

```c
// Use __builtin_expect for unlikely error paths
if (__builtin_expect(ptr == NULL, 0)) {
    // Unlikely error path
    return ERROR;
}

// Use likely/unlikely macros
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

if (unlikely(!ptr)) return ERROR;
if (likely(size > 0)) process(size);
```

## Emergency Debugging

If edge case bug occurs in production:

```c
// 1. Enable debug logging
#define DEBUG_EDGE_CASES 1

// 2. Add assertions
ASSERT(ptr != NULL);
ASSERT(size > 0 && size <= MAX_SIZE);

// 3. Check for corruption
if (magic != MAGIC_VALUE) {
    kernel_panic("Corruption detected");
}

// 4. Validate invariants
ASSERT(used_memory <= total_memory);
ASSERT(ref_count >= 0);
```

## Test Statistics

- **Total Tests:** 61
- **Boundary Values:** 15
- **Resource Exhaustion:** 12
- **Race Conditions:** 14
- **Invalid Input:** 8
- **Timing Issues:** 5
- **Combinations:** 7

## Quick Links

- Full Guide: `EDGE_CASE_TESTING_GUIDE.md`
- Summary: `EDGE_CASE_TESTING_SUMMARY.md`
- Tests: `tests/unit/test_edge_cases.c`

## Remember

> "Edge cases are where bugs hide. Test early, test often, test thoroughly."

---

Keep this card visible while coding!
