# Exercise 2.2: Calculator Program

**Tutorial:** 02 - Hello World  
**Difficulty:** Easy  
**Time:** 20-30 minutes  
**Skills:** C programming, userspace development, basic I/O  

---

## Learning Objectives

- Practice writing userspace programs
- Use printf with format specifiers
- Implement command-line argument parsing
- Handle edge cases and errors

---

## Requirements

Create a calculator program that performs basic arithmetic operations.

### Functional Requirements

1. **Operations:** Support `add`, `sub`, `mul`, `div`
2. **Input:** Two integers as operands
3. **Output:** Result of the operation
4. **Error Handling:** Invalid operations, division by zero

### Usage

```bash
aos> calc add 5 3
Result: 8

aos> calc sub 10 4
Result: 6

aos> calc mul 7 6
Result: 42

aos> calc div 20 5
Result: 4

aos> calc div 10 0
Error: Division by zero

aos> calc invalid 1 2
Error: Unknown operation 'invalid'
```

---

## Starter Code

See `starter/calc.c` for template.

---

## Implementation Steps

### Step 1: Parse Arguments

The program receives arguments similar to `main()`:

```c
void _start(void) {
    // In a real shell, you'd get argc/argv
    // For now, hardcode test cases
}
```

### Step 2: Implement Operations

```c
int add(int a, int b) {
    return a + b;
}

int sub(int a, int b) {
    return a - b;
}

// ... etc
```

### Step 3: Handle Input

Parse the operation string and call the appropriate function.

### Step 4: Error Handling

Check for:
- Invalid operations
- Division by zero
- Invalid numbers

---

## Hints

### Hint 1: String Comparison

```c
#include "../../../libc/string.h"

if (strcmp(op, "add") == 0) {
    // It's addition
}
```

### Hint 2: Number Parsing

For simplicity, hardcode numbers initially. Advanced: implement `atoi()`.

```c
// Simple version
int a = 5;
int b = 3;
```

### Hint 3: Testing

Test each operation:

```c
void test_calc(void) {
    assert(add(5, 3) == 8);
    assert(sub(10, 4) == 6);
    assert(mul(7, 6) == 42);
    assert(div(20, 5) == 4);
    printf("All tests passed!\n");
}
```

---

## Expected Output

```
========================================
  AutomationOS Calculator
========================================

Testing operations:
  5 + 3 = 8
  10 - 4 = 6
  7 * 6 = 42
  20 / 5 = 4

Division by zero:
  10 / 0 = Error: Division by zero

Unknown operation:
  Error: Unknown operation 'xyz'

========================================
  Calculator tests complete!
========================================
```

---

## Extension Ideas

### Easy Extensions

1. Add modulo operator (`mod`)
2. Add power operator (`pow`)
3. Support negative numbers

### Medium Extensions

4. Implement `atoi()` for parsing numbers from strings
5. Support floating-point operations
6. Add parentheses support (order of operations)

### Hard Extensions

7. Implement a full expression parser (e.g., "5 + 3 * 2")
8. Add variables (e.g., "x = 5; y = 3; x + y")
9. Create an interactive calculator mode

---

## Building and Testing

```bash
# Build
cd starter/
make

# Run in AutomationOS
# (Add to shell built-ins or load as program)

# Test
cd ../tests/
make test
```

---

## Solution

Reference solution available in `solution/calc.c`.

**Try solving it yourself first!**

---

## Common Mistakes

### Mistake 1: Forgetting to exit

```c
void _start(void) {
    // Do work
    // Missing exit(0)!
}
```

Always call `exit()` at the end.

### Mistake 2: Division by zero

```c
int div(int a, int b) {
    return a / b;  // Crash if b == 0!
}
```

Check for zero first:

```c
if (b == 0) {
    printf("Error: Division by zero\n");
    return 0;
}
```

### Mistake 3: Buffer overflows

```c
char op[4];
strcpy(op, user_input);  // Dangerous!
```

Use safe functions or check lengths.

---

## Assessment

Your calculator should:

- [x] Perform all 4 basic operations
- [x] Handle division by zero gracefully
- [x] Detect invalid operations
- [x] Print clear output
- [x] Exit properly
- [x] No memory leaks or crashes

---

## Next Steps

After completing this exercise:

1. Move to Exercise 2.3 (Number Guessing Game)
2. Explore more complex I/O
3. Learn about memory allocation (when available)

---

*Last Updated: 2026-05-26*
