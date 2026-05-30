# AutomationOS Learning Path - Contributors

**Target Audience:** Developers ready to contribute to the project  
**Time Investment:** 4-6 hours (after completing Developer path)  
**Prerequisites:** Completed Developer Learning Path, contributed at least one feature  

---

## Overview

This learning path prepares you to become an active contributor to AutomationOS. You'll learn:

- Project contribution workflow
- Code standards and review process
- Testing requirements
- Documentation standards
- Community guidelines

---

## Prerequisites

Before starting, you should have:

- [x] Completed [Developer Learning Path](LEARNING_PATH_DEVELOPER.md)
- [x] Added at least one syscall or driver
- [x] Fixed at least one bug
- [x] Read the entire codebase overview
- [x] Set up your development environment

---

## Learning Path

### Phase 1: Understanding the Project (1 hour)

**Goal:** Understand project goals, architecture decisions, and roadmap.

#### Materials:
- [README.md](../README.md) - Project vision
- [CHANGELOG.md](../CHANGELOG.md) - Recent changes
- [GitHub Issues](https://github.com/username/AutomationOS/issues) - Current work
- [Roadmap](docs/ROADMAP.md) - Future plans

#### What You'll Learn:
- Project philosophy and design decisions
- Current status and limitations
- Planned features and priorities
- Community needs and pain points

#### Exercises:
1. Read all open issues and categorize by type
2. Identify 3 features you could implement
3. Find 3 bugs you could fix
4. Draft a feature proposal

---

### Phase 2: Contribution Workflow (1 hour)

**Goal:** Master the Git workflow and contribution process.

#### Git Workflow

```bash
# 1. Fork the repository on GitHub

# 2. Clone your fork
git clone https://github.com/YOUR_USERNAME/AutomationOS
cd AutomationOS

# 3. Add upstream remote
git remote add upstream https://github.com/ORIGINAL/AutomationOS

# 4. Create feature branch
git checkout -b feature/my-new-feature

# 5. Make changes and commit
git add .
git commit -m "feat: add new feature

Detailed description of changes.
"

# 6. Push to your fork
git push origin feature/my-new-feature

# 7. Create Pull Request on GitHub
```

#### Commit Message Format

Follow conventional commits:

```
<type>(<scope>): <subject>

<body>

<footer>
```

**Types:**
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation only
- `style`: Formatting, no code change
- `refactor`: Code restructuring
- `perf`: Performance improvement
- `test`: Add/update tests
- `chore`: Build process, tools

**Example:**

```
feat(syscall): add sys_uptime() syscall

Implements a new syscall to return system uptime in timer ticks.
This is useful for benchmarking and timestamps.

- Added sys_uptime() handler
- Registered in syscall table
- Added userspace wrapper
- Includes test program

Closes #42
```

---

### Phase 3: Code Standards (1 hour)

**Goal:** Write code that matches project style.

#### C Style Guide

**Formatting:**
```c
// Indentation: 4 spaces (no tabs)
// Braces: K&R style
// Line length: 100 characters max

// Functions
void my_function(int arg1, int arg2) {
    if (condition) {
        do_something();
    } else {
        do_something_else();
    }
}

// Structs
typedef struct {
    uint64_t field1;
    uint32_t field2;
    char* field3;
} my_struct_t;

// Naming
#define MY_CONSTANT 42           // Constants: UPPER_SNAKE_CASE
typedef uint64_t my_type_t;     // Types: lower_snake_case_t
int global_variable;             // Globals: lower_snake_case
void my_function(void);          // Functions: lower_snake_case
```

**Documentation:**
```c
/**
 * my_function() - Short description
 * @arg1: Description of argument 1
 * @arg2: Description of argument 2
 *
 * Longer description of what the function does.
 * Can span multiple lines.
 *
 * Returns:
 *   0 on success, negative error code on failure
 *
 * Note:
 *   Special considerations or warnings
 */
int my_function(int arg1, int arg2) {
    // Implementation
}
```

**Error Handling:**
```c
// Check all return values
void* ptr = pmm_alloc_page();
if (ptr == NULL) {
    kprintf("[ERROR] Failed to allocate page\n");
    return -ENOMEM;
}

// Validate input
if (fd < 0 || fd >= MAX_FD) {
    return -EBADF;
}

// Use goto for cleanup (when appropriate)
int do_work(void) {
    void* buffer = kmalloc(SIZE);
    if (buffer == NULL) {
        goto err_alloc;
    }
    
    // Work...
    
    kfree(buffer);
    return 0;

err_alloc:
    return -ENOMEM;
}
```

---

### Phase 4: Testing Requirements (1 hour)

**Goal:** Write comprehensive tests for your code.

#### Test Types

**1. Unit Tests**

Test individual functions in isolation:

```c
// tests/unit/test_pmm.c

void test_pmm_alloc_page(void) {
    void* page = pmm_alloc_page();
    assert(page != NULL);
    assert(IS_PAGE_ALIGNED(page));
    pmm_free_page(page);
}

void test_pmm_alloc_multiple(void) {
    void* pages[10];
    for (int i = 0; i < 10; i++) {
        pages[i] = pmm_alloc_page();
        assert(pages[i] != NULL);
    }
    
    for (int i = 0; i < 10; i++) {
        pmm_free_page(pages[i]);
    }
}
```

**2. Integration Tests**

Test components working together:

```python
# tests/integration/test_syscalls.py

def test_write_syscall():
    """Test write syscall from userspace"""
    output = run_in_qemu("test_write")
    assert "Hello, World!" in output
    assert exit_code == 0

def test_fork_syscall():
    """Test fork creates child process"""
    output = run_in_qemu("test_fork")
    assert "Parent PID:" in output
    assert "Child PID:" in output
```

**3. Regression Tests**

Ensure bugs don't reappear:

```c
// tests/regression/test_issue_42.c

/**
 * Regression test for issue #42
 * Bug: sys_write() crashed on large buffers
 */
void test_issue_42_large_write(void) {
    char large_buffer[8192];
    memset(large_buffer, 'A', sizeof(large_buffer));
    
    int ret = write(1, large_buffer, sizeof(large_buffer));
    assert(ret == sizeof(large_buffer));
}
```

#### Test Checklist

For every contribution:

- [ ] Unit tests for new functions
- [ ] Integration tests for new features
- [ ] Regression tests for bug fixes
- [ ] All existing tests still pass
- [ ] Edge cases covered
- [ ] Error conditions tested

---

### Phase 5: Documentation Standards (30 minutes)

**Goal:** Document your code properly.

#### Required Documentation

**1. Code Comments**

```c
// Good: Explain WHY, not WHAT
// Map the physical memory region to virtual address space
// We use identity mapping for the first 4GB to simplify
// kernel initialization.
map_physical_memory(0, 0, SIZE_4GB);

// Bad: Repeats what code says
// Set x to 0
x = 0;
```

**2. Function Documentation**

Every public function needs:
- Short description
- Parameter descriptions
- Return value description
- Error conditions
- Usage notes

**3. File Headers**

```c
/**
 * pmm.c - Physical Memory Manager
 *
 * Implements a buddy allocator for physical page allocation.
 * Supports pages of size 4KB, 8KB, 16KB, etc.
 *
 * Author: Your Name
 * Date: 2026-05-26
 */
```

**4. Markdown Documentation**

For significant features, add:
- `docs/<FEATURE_NAME>.md` - Design document
- Update `docs/API_REFERENCE.md` - API additions
- Update relevant tutorials

---

### Phase 6: Code Review Process (1 hour)

**Goal:** Understand how reviews work and how to respond.

#### Submitting a PR

**1. Before Submitting:**

```bash
# Self-review checklist
- [ ] Code builds without warnings
- [ ] All tests pass
- [ ] New tests added
- [ ] Documentation updated
- [ ] Commit messages follow format
- [ ] Code follows style guide
- [ ] No debugging code left in
- [ ] Performance impact considered
```

**2. PR Description Template:**

```markdown
## Description
Brief description of changes.

## Motivation
Why is this change needed?

## Changes Made
- Added X
- Fixed Y
- Updated Z

## Testing
How was this tested?
- [ ] Manual testing in QEMU
- [ ] Unit tests pass
- [ ] Integration tests pass

## Related Issues
Fixes #42
Related to #37

## Checklist
- [x] Code builds
- [x] Tests pass
- [x] Documentation updated
- [x] Follows style guide
```

**3. Responding to Reviews:**

```markdown
# Reviewer comment:
> This could be optimized by using a hash table

# Good response:
Good idea! I've refactored to use a hash table in commit abc123.
Performance improved by 2x in benchmarks.

# Also acceptable:
I considered a hash table, but for our use case (max 10 items),
the linear search is simpler and performance difference is negligible.
I added a comment explaining this decision.
```

#### Review Feedback Types

- **Must Fix:** Critical issues (bugs, security, correctness)
- **Should Fix:** Important improvements (performance, clarity)
- **Nit:** Minor style/formatting issues
- **Question:** Requesting clarification

#### Review Timeline

- PRs reviewed within 48 hours
- Address feedback within 1 week
- Approved PRs merged within 24 hours

---

## Contribution Ideas

### Good First Issues

Easy contributions to start:

1. **Documentation:**
   - Fix typos
   - Add code examples
   - Clarify confusing sections

2. **Tests:**
   - Increase test coverage
   - Add edge case tests
   - Write integration tests

3. **Small Features:**
   - Add syscalls
   - Shell commands
   - Utility programs

### Intermediate Contributions

After your first PR:

4. **Drivers:**
   - Implement new drivers
   - Enhance existing drivers
   - Add driver tests

5. **Kernel Features:**
   - Memory management improvements
   - Scheduler enhancements
   - IPC mechanisms

6. **Tooling:**
   - Build system improvements
   - Debug tools
   - Testing infrastructure

### Advanced Contributions

Once you're experienced:

7. **Architecture:**
   - Design new subsystems
   - Refactor existing code
   - Performance optimization

8. **Security:**
   - Security audits
   - Implement security features
   - Fuzzing

9. **Mentoring:**
   - Review PRs
   - Answer questions
   - Write tutorials

---

## Community Guidelines

### Code of Conduct

Be:
- **Respectful:** Value diverse perspectives
- **Constructive:** Provide helpful feedback
- **Patient:** Remember everyone is learning
- **Welcoming:** Help newcomers

### Communication Channels

- **GitHub Issues:** Bug reports, feature requests
- **GitHub Discussions:** Questions, ideas, showcase
- **Pull Requests:** Code contributions
- **Email:** Security issues only

### Getting Help

If you're stuck:

1. Search existing issues and PRs
2. Check documentation and tutorials
3. Ask in Discussions
4. Tag maintainers (use sparingly)

---

## Recognition

### Contributor Levels

**Contributor:** Merged at least 1 PR  
**Regular Contributor:** 5+ merged PRs  
**Core Contributor:** 20+ merged PRs or significant feature  
**Maintainer:** Trusted with write access  

### Hall of Fame

Top contributors listed in:
- README.md
- CONTRIBUTORS.md
- Release notes

---

## Success Checklist

You're ready to contribute when you can:

- [ ] Fork and clone the repository
- [ ] Create feature branches
- [ ] Write code following style guide
- [ ] Add comprehensive tests
- [ ] Document your changes
- [ ] Submit well-formatted PRs
- [ ] Respond to review feedback
- [ ] Fix bugs independently
- [ ] Help other contributors

---

## Next Steps

### Your First Contribution

1. Browse [Good First Issues](https://github.com/username/AutomationOS/labels/good%20first%20issue)
2. Comment on an issue you want to work on
3. Fork, implement, test, document
4. Submit PR following guidelines
5. Respond to feedback
6. Celebrate your merged PR!

### Ongoing Contributions

- Regular communication in discussions
- Review other contributors' PRs
- Propose new features
- Mentor newcomers
- Maintain your contributed code

---

## Resources

### Documentation

- [Development Guide](docs/DEVELOPMENT_GUIDE.md)
- [Architecture Guide](docs/ARCHITECTURE.md)
- [API Reference](docs/API_REFERENCE.md)
- [Style Guide](docs/STYLE_GUIDE.md) (if available)

### Tools

- [GitHub CLI](https://cli.github.com/) - PR management
- [git-cliff](https://github.com/orhun/git-cliff) - Changelog generation
- [commitizen](https://commitizen-tools.github.io/) - Commit message helper

### Learning

- [OSDev Wiki](https://wiki.osdev.org/)
- [Linux Kernel Newbies](https://kernelnewbies.org/)
- [How to Contribute to Open Source](https://opensource.guide/how-to-contribute/)

---

## Thank You!

Your contributions make AutomationOS better for everyone.

**Questions?** Ask in Discussions  
**Ready to contribute?** Browse open issues  
**Need help?** Tag a maintainer  

---

*Last Updated: 2026-05-26*
