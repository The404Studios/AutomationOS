# Quick Start: Code Quality Tools

**For developers who want to get started immediately.**

---

## TL;DR - 3 Commands

```bash
# 1. Install hooks (one time)
make install-hooks

# 2. Format your code
make format

# 3. Check for issues
make lint
```

That's it! The pre-commit hook handles the rest automatically.

---

## Initial Setup (5 minutes)

### 1. Install Required Tools

**Ubuntu/Debian:**
```bash
sudo apt update
sudo apt install clang-format clang-tidy
```

**macOS:**
```bash
brew install clang-format llvm
```

**Arch Linux:**
```bash
sudo pacman -S clang
```

### 2. Install Git Hooks

```bash
cd /path/to/AutomationOS
make install-hooks
```

**Output:**
```
==========================================
  AutomationOS Git Hooks Installation
==========================================

Installing pre-commit hook...
  ✓ Pre-commit hook installed

Checking for required tools...
  ✓ clang-format 14.0
  ✓ clang-tidy 14.0
  ✓ git 2.39.0

✓ All required tools found

==========================================
Installation complete!
==========================================
```

Done! You're ready to go.

---

## Daily Workflow

### Standard Workflow (Recommended)

```bash
# 1. Make your changes
vim kernel/my_module.c

# 2. Add files (pre-commit hook runs automatically)
git add kernel/my_module.c

# 3. Commit (automatic formatting + checks)
git commit -m "Add new feature"
```

**What happens automatically:**
1. ✅ Code is auto-formatted (clang-format)
2. ✅ Trailing whitespace removed
3. ✅ Linter checks run (clang-tidy)
4. ✅ Style checks pass
5. ✅ Commit proceeds

### Manual Checks (Optional)

```bash
# Format all code before committing
make format

# Run linter manually
make lint

# Check formatting without modifying
make check-format
```

---

## Common Scenarios

### Scenario 1: "My commit was rejected"

**Error Message:**
```
[Pre-Commit] clang-tidy found warnings in 2 files
Run 'make lint' to see full details
To commit anyway: git commit --no-verify
```

**Solution:**
```bash
# See what's wrong
make lint

# Fix the issues manually, then:
git add -u
git commit -m "Fix linter warnings"
```

### Scenario 2: "I need to commit urgently"

**Emergency bypass** (use sparingly):
```bash
git commit --no-verify -m "Emergency fix"
```

**Note:** CI will still check your code. Fix issues ASAP.

### Scenario 3: "CI formatting check failed"

**Error in GitHub Actions:**
```
❌ Formatting check failed!
Run 'make format' to fix formatting issues.
```

**Solution:**
```bash
# Format locally
make format

# Commit and push
git add -u
git commit -m "Fix formatting"
git push
```

### Scenario 4: "False positive from linter"

**Suppress specific warning:**
```c
// NOLINTNEXTLINE(cert-err33-c)
result = malloc(size);  // We handle NULL later
```

**Suppress for entire line:**
```c
result = malloc(size);  // NOLINT
```

---

## Makefile Targets

### Code Quality Targets

| Command | Purpose | When to Use |
|---------|---------|-------------|
| `make format` | Auto-format all files | Before committing |
| `make check-format` | Check without modifying | CI pipelines |
| `make lint` | Run static analysis | Before PR |
| `make install-hooks` | Install git hooks | After clone |

### Build Targets (unchanged)

| Command | Purpose |
|---------|---------|
| `make all` | Build everything |
| `make kernel` | Build kernel only |
| `make test` | Run tests |
| `make qemu` | Run in QEMU |
| `make clean` | Clean build |

---

## Pre-Commit Hook Output

### Success ✓
```
[Pre-Commit] Running code quality checks...
[Pre-Commit] Found 3 staged C/H files

[1/6] Auto-formatting code with clang-format...
  ✓ Formatted: kernel/my_module.c
  ✓ Already formatted: kernel/include/my_module.h

[2/6] Checking for trailing whitespace...
✓ No trailing whitespace

[3/6] Checking indentation (tabs vs spaces)...
✓ Indentation OK (tabs used)

[4/6] Running static analysis with clang-tidy...
  ✓ Clean: kernel/my_module.c

[5/6] Checking for debug markers...
✓ No debug markers found

[6/6] Checking commit message format...
✓ Commit message format OK

==========================================
All pre-commit checks PASSED ✓
Proceeding with commit...
==========================================
```

### Failure ✗
```
[Pre-Commit] Running code quality checks...

[4/6] Running static analysis with clang-tidy...
  ✗ Warnings in: kernel/my_module.c
  kernel/my_module.c:42:5: warning: null pointer dereference

[Pre-Commit] clang-tidy found warnings in 1 files
Run 'make lint' to see full details
To commit anyway: git commit --no-verify

==========================================
Pre-commit checks FAILED
Fix the issues above or use: git commit --no-verify
==========================================
```

---

## Coding Standards Cheat Sheet

### Naming

```c
// Functions: lowercase_snake_case
void pmm_alloc_page(void);

// Variables: lowercase_snake_case
int ready_count = 0;

// Constants: UPPERCASE_SNAKE_CASE
#define PAGE_SIZE 4096

// Types: lowercase_snake_case_t
typedef struct process_t {
    int pid;
} process_t;
```

### Formatting

```c
// Tabs (not spaces) for indentation
void function(void)
{
	if (condition) {
		do_something();
	}
}

// 100 character line limit
int very_long_function_name(int arg1, int arg2,
			    int arg3, int arg4);

// Pointer alignment: right
int* ptr;
char* string;
```

### Error Handling

```c
// Always check NULL
void function(int* ptr)
{
	if (ptr == NULL) {
		kprintf("Error: NULL pointer\n");
		return;
	}

	// Safe to use ptr
	*ptr = 42;
}

// Check allocations
void* buf = kmalloc(size);
if (buf == NULL) {
	return -ENOMEM;
}
```

---

## CI/CD Integration

### GitHub Actions Workflow

**Triggered on:**
- Push to `main` or `develop`
- Pull requests to `main` or `develop`
- Only when `.c` or `.h` files change

**Checks:**
1. ✅ Code formatting (clang-format)
2. ✅ Static analysis (clang-tidy)
3. ✅ Coding standards (tabs, whitespace)
4. ✅ Documentation quality
5. ✅ Compilation with warnings

**View Results:**
- Green checkmark ✓ = All checks passed
- Red X ✗ = Failures (see logs for details)

---

## Troubleshooting

### "clang-format not found"

```bash
# Install
sudo apt install clang-format  # Ubuntu/Debian
brew install clang-format      # macOS
```

### "clang-tidy not found"

```bash
# Install
sudo apt install clang-tidy    # Ubuntu/Debian
brew install llvm              # macOS (includes clang-tidy)
```

### "Pre-commit hook not running"

```bash
# Reinstall hooks
make install-hooks

# Verify
ls -la .git/hooks/pre-commit
```

### "Too many warnings"

```bash
# See all warnings
make lint 2>&1 | less

# Fix one file at a time
clang-tidy kernel/my_module.c -- -Ikernel/include
```

---

## Best Practices

### ✅ Do

- Run `make format` before committing
- Fix linter warnings immediately
- Write descriptive commit messages (>10 chars)
- Check CI results after pushing
- Use `//` for single-line comments
- Use `/* */` for multi-line comments

### ❌ Don't

- Use `git commit --no-verify` habitually
- Mix tabs and spaces
- Leave trailing whitespace
- Commit debug prints (`XXX`, `HACK`)
- Use magic numbers (define constants)
- Ignore compiler warnings

---

## Getting Help

### Documentation

- `docs/CODING_STANDARDS.md` - Complete style guide
- `docs/CODE_REVIEW_CHECKLIST.md` - Review guidelines
- `docs/CODE_QUALITY_IMPLEMENTATION.md` - Full technical details

### Commands

```bash
# Show all make targets
make help

# Show clang-format version
clang-format --version

# Show clang-tidy checks
clang-tidy --list-checks
```

### Support

- Ask in #development channel
- File issue on GitHub
- Contact maintainers

---

## Summary

**Three simple steps:**
1. `make install-hooks` - One time setup
2. Write code normally
3. Commit (hooks handle quality)

**That's it!** The tooling handles the rest automatically.

---

**Questions?** See full documentation in `docs/CODING_STANDARDS.md`
