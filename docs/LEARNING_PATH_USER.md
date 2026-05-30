# AutomationOS Learning Path - End Users

**Target Audience:** Users who want to use and understand AutomationOS  
**Time Investment:** 4-6 hours  
**Prerequisites:** Basic command-line experience  

---

## Overview

This learning path will take you from installing AutomationOS to confidently using it for everyday tasks. You'll learn how to:

- Build and boot the operating system
- Navigate the shell
- Run programs
- Understand what's happening under the hood

---

## Learning Path

### Phase 1: Getting Started (30 minutes)

**Goal:** Get AutomationOS running on your machine.

#### Materials:
- [Tutorial 1: Getting Started](tutorials/01_GETTING_STARTED.md)
- [QUICKSTART.md](../QUICKSTART.md)

#### What You'll Learn:
- Install development tools
- Build AutomationOS from source
- Boot in QEMU
- Basic shell commands

#### Hands-On Exercise:
1. Install all prerequisites
2. Build AutomationOS
3. Boot and interact with the shell
4. Try all built-in commands (echo, help, pid, clear, uptime)

---

### Phase 2: Understanding the System (1 hour)

**Goal:** Understand what AutomationOS is and how it works.

#### Materials:
- [Architecture Guide](docs/ARCHITECTURE.md) - Read sections:
  - Introduction
  - System Overview  
  - Boot Process
- [README.md](../README.md) - Features section

#### What You'll Learn:
- What an operating system does
- How AutomationOS boots
- Memory management basics
- Process model

#### Hands-On Exercise:
1. Watch the boot process and identify each stage
2. Explain to someone (or write down) what happens during boot
3. Diagram the system components

---

### Phase 3: Running Programs (1.5 hours)

**Goal:** Run and understand userspace programs.

#### Materials:
- [Tutorial 2: Hello World](tutorials/02_HELLO_WORLD.md)
- [Tutorial 3: System Calls](tutorials/03_SYSTEM_CALLS.md)

#### What You'll Learn:
- How userspace programs work
- The C library (libc)
- What system calls are
- How programs communicate with the kernel

#### Hands-On Exercises:
1. Write "Hello, World!" from scratch
2. Create a program that uses all 5 syscalls
3. Write a calculator program
4. Create a program that displays uptime in a loop

---

### Phase 4: Exploring the Codebase (1 hour)

**Goal:** Navigate and understand the source code.

#### Materials:
- Browse through:
  - `userspace/libc/` - C library
  - `userspace/shell/` - Shell implementation
  - `kernel/core/` - Kernel core (overview)

#### What You'll Learn:
- Project structure
- How the shell works
- How printf is implemented
- Where different features live

#### Hands-On Exercises:
1. Find the implementation of `printf()`
2. Trace a syscall from userspace to kernel
3. Identify where the shell prompt is printed
4. Find the process scheduler code

---

### Phase 5: Customization (1 hour)

**Goal:** Customize AutomationOS for your needs.

#### Materials:
- [Development Guide](docs/DEVELOPMENT_GUIDE.md) - Code Style section
- [Shell source](../userspace/shell/shell.c)

#### What You'll Learn:
- How to modify the shell
- Add custom commands
- Change boot messages
- Customize the prompt

#### Hands-On Exercises:
1. Change the shell prompt from `aos>` to your name
2. Add a `fortune` command that prints random quotes
3. Change the kernel boot banner
4. Add color codes to shell output (ANSI escape codes)

---

### Phase 6: Troubleshooting (30 minutes)

**Goal:** Learn to diagnose and fix common issues.

#### Materials:
- [Troubleshooting Guide](docs/TROUBLESHOOTING.md)
- [Tutorial 4: Debugging](tutorials/04_DEBUGGING.md) - Basic sections

#### What You'll Learn:
- Common build errors
- Boot problems
- Shell issues
- Where to find logs

#### Hands-On Exercises:
1. Intentionally break the build and fix it
2. Corrupt a binary and diagnose the issue
3. Enable verbose logging and understand the output

---

## Milestones

Track your progress:

- [ ] Successfully built AutomationOS
- [ ] Booted in QEMU
- [ ] Ran all shell commands
- [ ] Wrote "Hello, World!" program
- [ ] Understood all 5 system calls
- [ ] Navigated the source code
- [ ] Customized the shell
- [ ] Solved at least one problem independently

---

## Assessment Quiz

Test your knowledge:

### Basic Understanding

1. What is a system call?
2. What does the init process do?
3. Name the 5 syscalls in AutomationOS Phase 1.
4. What's the difference between kernel and user space?
5. What is a bootloader?

### Applied Knowledge

6. How would you print your process ID from a C program?
7. Where is the shell source code located?
8. What happens when you type `echo hello` in the shell?
9. How do you enable debug output in the kernel?
10. What file would you edit to add a new shell command?

**Answers:** Check your understanding in the tutorials.

---

## Next Steps

### Become a Power User

- Create useful utility programs (ls, cat, grep)
- Write shell scripts (when shell scripting is implemented)
- Optimize your workflow
- Contribute documentation improvements

### Transition to Developer

Ready to modify the kernel? Continue to:
- [Learning Path: Developer](LEARNING_PATH_DEVELOPER.md)

---

## Resources

### Essential Documentation

- [QUICKSTART.md](../QUICKSTART.md) - Quick reference
- [README.md](../README.md) - Project overview
- [docs/INDEX.md](docs/INDEX.md) - All documentation

### Tutorials (User-Focused)

1. [Getting Started](tutorials/01_GETTING_STARTED.md)
2. [Hello World](tutorials/02_HELLO_WORLD.md)
3. [System Calls](tutorials/03_SYSTEM_CALLS.md)
4. [Debugging](tutorials/04_DEBUGGING.md) - Basic sections

### Community

- GitHub Issues - Ask questions, report bugs
- Discussions - Share your projects
- Contributing Guide - Help improve AutomationOS

---

## Tips for Success

### 1. Learn by Doing

Don't just read - type the code, run the commands, experiment.

### 2. Take Notes

Keep a journal of:
- What you learned
- Commands you found useful
- Problems you solved
- Questions you still have

### 3. Teach Others

The best way to solidify your knowledge:
- Explain concepts to a friend
- Write blog posts
- Create video tutorials
- Answer questions in discussions

### 4. Don't Skip Exercises

The hands-on exercises are where real learning happens.

### 5. Ask Questions

If you're stuck:
- Check the Troubleshooting Guide
- Search existing GitHub issues
- Ask in Discussions
- Review related documentation

---

## Time Estimates

| Phase | Estimated Time |
|-------|----------------|
| Getting Started | 30 minutes |
| Understanding the System | 1 hour |
| Running Programs | 1.5 hours |
| Exploring Codebase | 1 hour |
| Customization | 1 hour |
| Troubleshooting | 30 minutes |
| **Total** | **5.5 hours** |

Add buffer time for experimentation and exercises: **6-8 hours total**.

---

## Success Stories

What you'll be able to do after completing this path:

✅ Build and run AutomationOS from source  
✅ Write userspace programs in C  
✅ Understand how operating systems work  
✅ Navigate and modify the codebase  
✅ Customize the system for your needs  
✅ Diagnose and fix common issues  
✅ Explain OS concepts to others  

---

## Feedback

Help us improve this learning path:

- Was anything confusing?
- Which exercises were most helpful?
- What topics need more coverage?
- How long did it actually take you?

Share your feedback in GitHub Discussions or Issues.

---

**Ready to start?** Begin with [Tutorial 1: Getting Started](tutorials/01_GETTING_STARTED.md)

**Want to go deeper?** Continue to [Learning Path: Developer](LEARNING_PATH_DEVELOPER.md)

---

*Last Updated: 2026-05-26*
