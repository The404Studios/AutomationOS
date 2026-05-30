# AutomationOS Tutorial Exercises

This directory contains hands-on coding exercises to reinforce tutorial concepts.

---

## Structure

Each exercise includes:

- **README.md** - Exercise description, requirements, hints
- **starter/** - Starter code templates
- **solution/** - Reference solutions
- **tests/** - Automated tests (when applicable)

---

## Exercises by Tutorial

### Tutorial 1: Getting Started

- **Exercise 1.1:** Build AutomationOS from scratch (no exercise file - follow tutorial)
- **Exercise 1.2:** Customize boot banner

### Tutorial 2: Hello World

- **Exercise 2.1:** ASCII Art Printer
- **Exercise 2.2:** Calculator Program
- **Exercise 2.3:** Number Guessing Game

### Tutorial 3: System Calls

- **Exercise 3.1:** Syscall Showcase
- **Exercise 3.2:** Buffered I/O Library
- **Exercise 3.3:** Syscall Benchmark Tool

### Tutorial 4: Debugging

- **Exercise 4.1:** Debug Intentional Bugs
- **Exercise 4.2:** Memory Corruption Detective
- **Exercise 4.3:** GDB Command Script

### Tutorial 5: Adding a Syscall

- **Exercise 5.1:** Implement sys_getppid()
- **Exercise 5.2:** Add sys_mem_free()
- **Exercise 5.3:** Create sys_process_count()

### Tutorial 6: Writing a Driver

- **Exercise 6.1:** Virtual LED Driver
- **Exercise 6.2:** Random Number Device
- **Exercise 6.3:** Keyboard Buffer Enhancement

### Tutorial 7: Memory Management

- **Exercise 7.1:** Memory Statistics Tool
- **Exercise 7.2:** Custom Allocator
- **Exercise 7.3:** Page Table Walker

### Tutorial 8: Process Scheduling

- **Exercise 8.1:** CPU Time Tracker
- **Exercise 8.2:** Priority Scheduler
- **Exercise 8.3:** Scheduling Visualizer

---

## How to Use Exercises

### 1. Read the Exercise Description

Each exercise has a README with:
- Learning objectives
- Requirements
- Hints and tips
- Expected output

### 2. Start with Starter Code

Use the starter code templates as a foundation:

```bash
cd exercises/02_hello_world/calculator/starter
```

### 3. Implement Your Solution

Follow the requirements and try to solve it yourself.

### 4. Test Your Solution

Run the provided tests:

```bash
make test
```

### 5. Compare with Reference Solution

If stuck, check the solution directory:

```bash
cd exercises/02_hello_world/calculator/solution
```

**Important:** Try solving it yourself first!

---

## Exercise Difficulty Levels

- **Easy** - Straightforward, follows tutorial examples closely
- **Medium** - Requires applying concepts in new ways
- **Hard** - Requires research, complex logic, or creative problem-solving
- **Challenge** - Open-ended, multiple solutions, advanced topics

---

## Creating Your Own Exercises

Want to contribute an exercise?

1. Create a new directory: `exercises/<tutorial>/<exercise_name>/`
2. Add:
   - `README.md` - Description and requirements
   - `starter/` - Template code
   - `solution/` - Reference implementation
   - `tests/` - Automated tests (optional)
3. Update this README
4. Submit a pull request

---

## Automated Testing

Some exercises include automated tests:

```bash
# Run all tests
make test-exercises

# Run specific exercise tests
make test-exercise NAME=calculator
```

Tests use a simple test framework in `tests/framework.h`.

---

## Exercise Completion Tracking

Track your progress:

- [ ] Tutorial 1 - All exercises
- [ ] Tutorial 2 - 3/3 exercises
- [ ] Tutorial 3 - 3/3 exercises
- [ ] Tutorial 4 - 3/3 exercises
- [ ] Tutorial 5 - 3/3 exercises
- [ ] Tutorial 6 - 3/3 exercises
- [ ] Tutorial 7 - 3/3 exercises
- [ ] Tutorial 8 - 3/3 exercises

---

## Getting Help

Stuck on an exercise?

1. Re-read the corresponding tutorial
2. Check the hints in the exercise README
3. Review the API reference
4. Look at similar code in the kernel
5. Ask in GitHub Discussions

---

## Contributing Solutions

Found a better solution? Submit it!

1. Create a new solution variant in `solution/variant_<description>/`
2. Explain your approach in a README
3. Submit a pull request

---

## License

All exercises are part of AutomationOS and follow the same license.

---

*Last Updated: 2026-05-26*
