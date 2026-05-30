# Contributing to AutomationOS

Thank you for your interest in contributing to AutomationOS! This guide will help you get started.

---

## Table of Contents

- [Getting Started](#getting-started)
- [Development Workflow](#development-workflow)
- [Code Standards](#code-standards)
- [Testing Requirements](#testing-requirements)
- [Commit Guidelines](#commit-guidelines)
- [Pull Request Process](#pull-request-process)
- [CI/CD Requirements](#cicd-requirements)

---

## Getting Started

### Prerequisites

Before contributing, ensure you have:

1. **Cross-compiler toolchain** (x86_64-elf-gcc)
2. **Build tools** (make, nasm, python3)
3. **QEMU** for testing
4. **Git** for version control

See [QUICKSTART.md](../QUICKSTART.md) for detailed setup instructions.

### Fork and Clone

```bash
# Fork the repository on GitHub
# Then clone your fork
git clone https://github.com/YOUR_USERNAME/AutomationOS.git
cd AutomationOS

# Add upstream remote
git remote add upstream https://github.com/ORIGINAL_OWNER/AutomationOS.git
```

### Install Pre-Commit Hooks

```bash
make install-hooks
```

This installs hooks that:
- Auto-format code with clang-format
- Run static analysis
- Check for common issues

---

## Development Workflow

### 1. Create a Branch

```bash
git checkout -b feature/my-new-feature
```

**Branch naming conventions:**
- `feature/` - New features
- `fix/` - Bug fixes
- `docs/` - Documentation updates
- `refactor/` - Code refactoring
- `test/` - Test additions/fixes

### 2. Make Changes

Edit code following our [Code Standards](#code-standards).

### 3. Build and Test

```bash
# Build everything
make all

# Run tests
make test

# Run in QEMU
make qemu
```

### 4. Commit Changes

```bash
git add <files>
git commit
```

Pre-commit hooks will automatically:
- Format your code
- Run static analysis
- Check for issues

See [Commit Guidelines](#commit-guidelines) for commit message format.

### 5. Push and Create PR

```bash
git push origin feature/my-new-feature
```

Then create a Pull Request on GitHub.

---

## Code Standards

### C Code Style

**General Rules:**
- Use **tabs** for indentation (not spaces)
- K&R brace style
- Maximum line length: 100 characters
- Use `snake_case` for functions and variables
- Use `UPPER_CASE` for constants and macros

**Example:**

```c
#define MAX_BUFFER_SIZE 4096

typedef struct {
	uint64_t address;
	size_t size;
} memory_region_t;

int allocate_memory(size_t size, uint64_t *out_addr)
{
	if (size == 0 || size > MAX_BUFFER_SIZE) {
		return -1;
	}

	// Allocation logic here
	*out_addr = 0xFFFF800000000000;
	return 0;
}
```

### Assembly Style

**NASM conventions:**
- Use lowercase for instructions
- Indent instructions with tab
- Comment complex operations

**Example:**

```nasm
global kernel_entry
extern kernel_main

kernel_entry:
    cli                     ; Disable interrupts
    mov rsp, stack_top      ; Set up stack
    call kernel_main        ; Jump to C code
    hlt                     ; Halt CPU
```

### Documentation

**Comment Requirements:**
- All functions must have docstring comments
- Complex algorithms must be explained
- Public APIs must be documented

**Example:**

```c
/**
 * Allocate physical memory pages.
 *
 * @param num_pages Number of contiguous pages to allocate
 * @param flags Allocation flags (ALLOC_ZERO, ALLOC_DMA, etc.)
 * @return Physical address of allocated memory, or 0 on failure
 */
uint64_t pmm_alloc(size_t num_pages, uint32_t flags);
```

---

## Testing Requirements

### All PRs Must Include Tests

**Required:**
- Unit tests for new functions
- Integration tests for new features
- QEMU boot tests must pass

### Writing Unit Tests

Create test in `tests/unit/`:

```c
#include "unity.h"
#include "pmm.h"

void test_pmm_alloc_single_page(void)
{
	uint64_t addr = pmm_alloc(1, 0);
	TEST_ASSERT_NOT_EQUAL(0, addr);
	TEST_ASSERT_TRUE(is_aligned(addr, PAGE_SIZE));

	pmm_free(addr, 1);
}

void test_pmm_alloc_zero_pages(void)
{
	uint64_t addr = pmm_alloc(0, 0);
	TEST_ASSERT_EQUAL(0, addr);
}
```

### Writing Integration Tests

Create test in `tests/integration/`:

```python
def test_new_feature():
    """Test that new feature works in QEMU"""
    boot = BootTest(timeout=20)
    output = boot.run()

    # Check for expected output
    assert '[NEW_FEATURE]' in output
    assert 'Feature initialized' in output
```

### Running Tests

```bash
# Unit tests
cd tests/unit
make test

# Integration tests
python3 tests/integration/test_boot.py

# All tests
make test-full
```

---

## Commit Guidelines

### Conventional Commits

We follow [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <description>

[optional body]

[optional footer]
```

**Types:**
- `feat` - New feature
- `fix` - Bug fix
- `docs` - Documentation changes
- `style` - Code style changes (formatting)
- `refactor` - Code refactoring
- `perf` - Performance improvements
- `test` - Test additions/fixes
- `build` - Build system changes
- `ci` - CI/CD changes
- `chore` - Other changes (dependencies, etc.)

**Examples:**

```
feat(pmm): implement buddy allocator

Add buddy allocator for efficient physical memory management.
Supports orders from 4KB (2^0) to 2MB (2^9).

Closes #42
```

```
fix(vmm): correct page table entry flags

Fixed incorrect NX bit handling in 4-level page tables.
```

```
docs: add CI/CD architecture documentation
```

### Commit Message Rules

1. **First line:**
   - Start with type and scope
   - Use imperative mood ("add" not "added")
   - No period at end
   - Max 72 characters

2. **Body (optional):**
   - Explain *what* and *why*, not *how*
   - Wrap at 72 characters

3. **Footer (optional):**
   - Reference issues: `Closes #123`
   - Breaking changes: `BREAKING CHANGE: description`

---

## Pull Request Process

### Before Submitting

**Checklist:**
- [ ] Code follows style guidelines
- [ ] All tests pass locally
- [ ] New tests added for new features
- [ ] Documentation updated
- [ ] Commit messages follow conventions
- [ ] Pre-commit hooks pass

### Creating the PR

1. **Title:** Use conventional commit format
   ```
   feat(scheduler): add round-robin scheduling
   ```

2. **Description:** Include:
   - What changed
   - Why it changed
   - How to test it
   - Related issues

**Template:**

```markdown
## Summary
Brief description of changes.

## Changes
- Change 1
- Change 2
- Change 3

## Testing
How to test these changes:
1. Build with `make all`
2. Run `make test`
3. Verify in QEMU: `make qemu`

## Checklist
- [ ] Tests pass
- [ ] Documentation updated
- [ ] No new warnings

Closes #123
```

### CI/CD Checks

Your PR will trigger:
- **Build checks** on Ubuntu, macOS, WSL2
- **Unit tests**
- **Integration tests**
- **Static analysis**
- **Security scan**

**All checks must pass before merge.**

### Review Process

1. **Automated checks** run first
2. **Maintainer review** (within 48 hours)
3. **Address feedback**
4. **Approval and merge**

---

## CI/CD Requirements

### Required CI Checks

All PRs must pass:

1. **Build on all platforms**
   - Ubuntu 22.04
   - Ubuntu 24.04
   - macOS latest
   - Docker

2. **Test suite**
   - Unit tests
   - Integration tests
   - QEMU boot tests

3. **Static analysis**
   - cppcheck
   - clang-tidy
   - No warnings

4. **Security scan**
   - CodeQL analysis
   - Vulnerability scan

### Optional CI Checks

These run on main branch:
- Benchmark tests
- Fuzzing (1 hour)
- Performance regression

### Local CI Testing

Run CI checks locally:

```bash
# Build in Docker (like CI)
docker-compose up build

# Run all tests
make test-full

# Run static analysis
make lint

# Run security checks
make security-check
```

---

## Code Review Guidelines

### For Contributors

**Responding to Feedback:**
- Be open to suggestions
- Ask questions if unclear
- Make requested changes promptly
- Push new commits (don't force-push during review)

### For Reviewers

**Review Focus:**
- Correctness
- Code quality
- Test coverage
- Documentation
- Performance implications
- Security considerations

**Feedback Style:**
- Be constructive and respectful
- Explain *why* changes are needed
- Suggest alternatives
- Approve when ready

---

## Getting Help

### Resources

- **Documentation:** [docs/](../docs/)
- **API Reference:** [docs/API_REFERENCE.md](API_REFERENCE.md)
- **Architecture:** [docs/ARCHITECTURE.md](ARCHITECTURE.md)
- **CI/CD:** [docs/CI_CD.md](CI_CD.md)

### Communication

- **GitHub Issues** - Bug reports, feature requests
- **GitHub Discussions** - Questions, ideas
- **Pull Requests** - Code contributions

---

## Recognition

Contributors will be:
- Listed in CONTRIBUTORS.md
- Credited in release notes
- Thanked in commit messages

---

## License

By contributing, you agree that your contributions will be licensed under the project's license.

---

## Code of Conduct

### Our Standards

- Be respectful and inclusive
- Accept constructive criticism
- Focus on what's best for the project
- Show empathy toward others

### Unacceptable Behavior

- Harassment or discrimination
- Trolling or insulting comments
- Personal or political attacks
- Publishing others' private information

---

## Questions?

If you have questions about contributing, please:
1. Check existing documentation
2. Search GitHub Issues
3. Open a new Discussion
4. Ask in your Pull Request

Thank you for contributing to AutomationOS! 🚀

---

**Last Updated:** 2026-05-26
