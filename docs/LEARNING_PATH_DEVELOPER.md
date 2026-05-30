# AutomationOS Learning Path - Kernel Developers

**Target Audience:** Developers who want to extend and modify the kernel  
**Time Investment:** 12-16 hours  
**Prerequisites:** C programming, basic OS concepts, completed User learning path  

---

## Overview

This learning path will transform you from a user into a kernel developer. You'll learn how to:

- Add new system calls
- Write device drivers
- Modify memory management
- Understand the scheduler
- Debug kernel code
- Contribute features

---

## Prerequisites Check

Before starting, ensure you have:

- [ ] Completed [User Learning Path](LEARNING_PATH_USER.md)
- [ ] Strong C programming skills (pointers, structs, inline assembly helpful)
- [ ] Built and run AutomationOS successfully
- [ ] Written at least one userspace program
- [ ] Basic understanding of computer architecture (registers, memory, interrupts)

**Recommended:** Take the [C Systems Programming Refresher](#c-systems-programming-refresher) if needed.

---

## Learning Path

### Phase 1: Kernel Fundamentals (2 hours)

**Goal:** Understand kernel architecture and development environment.

#### Materials:
- [Architecture Guide](docs/ARCHITECTURE.md) - Full read
- [API Reference](docs/API_REFERENCE.md) - Overview
- [Development Guide](docs/DEVELOPMENT_GUIDE.md) - Setup and workflow

#### What You'll Learn:
- Kernel memory layout
- Boot process in detail
- Interrupt handling
- Privilege levels (Ring 0 vs Ring 3)
- Build system and toolchain

#### Hands-On Exercises:
1. Map out the kernel memory layout
2. Identify all kernel subsystems
3. Trace the boot process with GDB
4. Modify kernel boot banner

**Assessment:** Can you explain how the kernel gets from UEFI to `kernel_main()`?

---

### Phase 2: Debugging Mastery (2 hours)

**Goal:** Master kernel debugging techniques.

#### Materials:
- [Tutorial 4: Debugging with GDB](tutorials/04_DEBUGGING.md)
- [Troubleshooting Guide](docs/TROUBLESHOOTING.md)

#### What You'll Learn:
- QEMU + GDB setup
- Setting breakpoints in kernel code
- Inspecting memory and registers
- Debugging crashes and panics
- Serial console logging
- QEMU monitor

#### Hands-On Exercises:
1. Set breakpoints at each kernel initialization stage
2. Step through a system call from userspace to kernel
3. Debug an intentional null pointer dereference
4. Use watchpoints to track variable changes
5. Examine page tables with GDB

**Assessment:** Can you debug a kernel panic and identify the root cause?

---

### Phase 3: System Calls Deep Dive (2.5 hours)

**Goal:** Add and modify system calls.

#### Materials:
- [Tutorial 3: System Calls](tutorials/03_SYSTEM_CALLS.md)
- [Tutorial 5: Adding a Syscall](tutorials/05_ADDING_SYSCALL.md)
- [kernel/core/syscall/](../kernel/core/syscall/) - Source code

#### What You'll Learn:
- x86_64 syscall mechanism
- Register calling convention
- Syscall dispatch table
- Error handling
- Userspace wrappers
- Security considerations

#### Hands-On Exercises:
1. Implement `sys_uptime()` following Tutorial 5
2. Add `sys_getppid()` - return parent PID
3. Add `sys_process_info()` - return process information struct
4. Implement input validation for all new syscalls
5. Write userspace test programs for each

**Assessment:** Can you design and implement a new syscall from scratch?

---

### Phase 4: Memory Management (3 hours)

**Goal:** Understand and modify the memory manager.

#### Materials:
- [Tutorial 7: Memory Management](tutorials/07_MEMORY_MANAGEMENT.md)
- [API Reference](docs/API_REFERENCE.md) - Memory sections
- [kernel/core/mem/](../kernel/core/mem/) - PMM, VMM, heap source

#### What You'll Learn:
- Physical Memory Manager (buddy allocator)
- Virtual Memory Manager (paging)
- Kernel heap (slab allocator)
- Page tables and TLB
- Memory allocation strategies

#### Hands-On Exercises:
1. Trace memory allocation in GDB
2. Implement memory statistics syscall
3. Add memory pressure detection
4. Modify page size (experiment with huge pages)
5. Implement copy-on-write for `fork()`

**Assessment:** Can you implement a new memory allocation strategy?

---

### Phase 5: Device Drivers (2.5 hours)

**Goal:** Write and debug device drivers.

#### Materials:
- [Tutorial 6: Writing a Driver](tutorials/06_WRITING_DRIVER.md)
- [kernel/drivers/](../kernel/drivers/) - Existing drivers
- [Driver Expansion Plan](docs/DRIVER_EXPANSION_PLAN.md)

#### What You'll Learn:
- Driver architecture
- I/O port access
- Interrupt handling
- Device initialization
- MMIO vs Port I/O

#### Hands-On Exercises:
1. Write a simple LED driver (use parallel port)
2. Implement a virtual device driver
3. Add an interrupt handler
4. Create a character device
5. Write a test suite for your driver

**Assessment:** Can you write a functional device driver from scratch?

---

### Phase 6: Process Scheduling (2 hours)

**Goal:** Understand and modify the scheduler.

#### Materials:
- [Tutorial 8: Process Scheduling](tutorials/08_PROCESS_SCHEDULING.md)
- [kernel/core/sched/](../kernel/core/sched/) - Scheduler source
- [Architecture Guide](docs/ARCHITECTURE.md) - Process Management section

#### What You'll Learn:
- Round-robin scheduling
- Context switching
- Process states
- Timer interrupts
- Priority levels

#### Hands-On Exercises:
1. Trace a context switch in GDB
2. Implement priority scheduling
3. Add CPU time tracking
4. Create scheduling statistics
5. Write CPU-bound test programs

**Assessment:** Can you implement an alternative scheduling algorithm?

---

### Phase 7: Advanced Topics (2 hours)

**Goal:** Tackle advanced kernel features.

#### Materials:
- [Tutorial 9: Kernel Modules](tutorials/09_KERNEL_MODULE.md)
- [Security Guide](docs/SECURITY.md) (when available)
- [Performance Guide](docs/PERFORMANCE.md)

#### What You'll Learn:
- Kernel modules
- Security mechanisms
- Performance optimization
- Concurrency and locking
- Error handling patterns

#### Hands-On Exercises:
1. Write a loadable kernel module
2. Implement reference counting
3. Add mutex support
4. Profile kernel performance
5. Harden syscalls against malicious input

**Assessment:** Can you identify and fix security vulnerabilities?

---

## Milestones

Track your progress:

- [ ] Built kernel with custom changes
- [ ] Added at least 2 new syscalls
- [ ] Wrote a device driver
- [ ] Modified the memory manager
- [ ] Debugged a kernel panic
- [ ] Implemented a scheduler feature
- [ ] Created a kernel module
- [ ] Contributed a patch

---

## Capstone Project

Choose one:

### Option 1: Mini Filesystem
Implement a simple read-only filesystem (e.g., TAR format).

**Requirements:**
- Syscalls: `open()`, `close()`, `read()`, `readdir()`
- VFS layer
- File descriptor table
- Path parsing

**Time:** 8-12 hours

---

### Option 2: Network Stack
Implement basic Ethernet + ARP.

**Requirements:**
- Ethernet driver (virtio-net)
- Packet reception/transmission
- ARP protocol
- Raw socket interface

**Time:** 10-15 hours

---

### Option 3: IPC Mechanism
Implement pipes or message queues.

**Requirements:**
- `pipe()` syscall
- Blocking I/O
- Buffer management
- Process communication

**Time:** 6-10 hours

---

## Assessment Quiz

Test your knowledge:

### Kernel Architecture

1. What's the difference between physical and virtual memory?
2. Explain the buddy allocator algorithm.
3. What happens during a context switch?
4. What is the purpose of the IDT?
5. How does the kernel handle interrupts?

### System Calls

6. What registers are used for syscall arguments?
7. Why is input validation critical?
8. What's the difference between `syscall` and `int 0x80`?
9. How do you return errors from syscalls?
10. What happens if a syscall handler crashes?

### Debugging

11. How do you set a breakpoint in kernel space?
12. What is QEMU's GDB stub?
13. How do you debug a page fault?
14. What information does a backtrace provide?
15. How do you examine page tables in GDB?

### Advanced

16. What is copy-on-write?
17. How does the TLB improve performance?
18. What are the security risks of direct hardware access?
19. How do you prevent race conditions?
20. What is a kernel panic and how do you debug it?

**Answers:** Review tutorials and documentation.

---

## Next Steps

### Specialize

Choose an area to master:

- **Memory Management** - Advanced paging, caching strategies
- **Networking** - Full TCP/IP stack
- **Filesystems** - Read-write filesystems, journaling
- **Security** - Capabilities, sandboxing, secure boot
- **Performance** - Profiling, optimization, scalability

### Contribute

Ready to give back?

- [Contributing Guide](LEARNING_PATH_CONTRIBUTOR.md)
- Fix bugs in GitHub Issues
- Implement features from the roadmap
- Write documentation
- Review pull requests

---

## Resources

### Essential Documentation

- [Architecture Guide](docs/ARCHITECTURE.md)
- [API Reference](docs/API_REFERENCE.md)
- [Development Guide](docs/DEVELOPMENT_GUIDE.md)
- [Build Guide](docs/BUILD_GUIDE.md)

### Tutorials (Developer-Focused)

1. [Debugging with GDB](tutorials/04_DEBUGGING.md)
2. [Adding a Syscall](tutorials/05_ADDING_SYSCALL.md)
3. [Writing a Driver](tutorials/06_WRITING_DRIVER.md)
4. [Memory Management](tutorials/07_MEMORY_MANAGEMENT.md)
5. [Process Scheduling](tutorials/08_PROCESS_SCHEDULING.md)
6. [Kernel Modules](tutorials/09_KERNEL_MODULE.md)

### External Resources

- [OSDev Wiki](https://wiki.osdev.org/) - OS development reference
- [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) - x86_64 reference
- [Linux Kernel](https://www.kernel.org/) - Inspiration and examples
- [xv6](https://pdos.csail.mit.edu/6.828/2020/xv6.html) - Educational OS

---

## C Systems Programming Refresher

Need a brush-up on C? Review these topics:

### Essential C Concepts

- Pointers and pointer arithmetic
- Struct layout and alignment
- Function pointers
- Inline assembly
- Volatile keyword
- Static vs extern
- Header guards

### Practice Problems

```c
// 1. Write a linked list implementation
// 2. Implement a buddy allocator
// 3. Parse a binary file format
// 4. Write inline assembly for I/O ports
// 5. Implement spinlocks
```

### Recommended Book

"C Programming Language" by K&R - Chapters 5-6

---

## Time Estimates

| Phase | Estimated Time |
|-------|----------------|
| Kernel Fundamentals | 2 hours |
| Debugging Mastery | 2 hours |
| System Calls | 2.5 hours |
| Memory Management | 3 hours |
| Device Drivers | 2.5 hours |
| Process Scheduling | 2 hours |
| Advanced Topics | 2 hours |
| **Total** | **16 hours** |

Capstone project: +8-15 hours

**Total commitment:** 24-31 hours to become proficient.

---

## Success Stories

What you'll be able to do after completing this path:

✅ Design and implement new kernel features  
✅ Debug complex kernel issues  
✅ Write device drivers  
✅ Optimize memory management  
✅ Understand scheduling algorithms  
✅ Contribute meaningful patches  
✅ Mentor other developers  
✅ Architect subsystems  

---

## Feedback

Help us improve this learning path:

- Which phase was most challenging?
- What exercises were most valuable?
- What topics need more depth?
- How could we improve the capstone project?

Share your feedback in GitHub Discussions.

---

**Ready to start?** Begin with Phase 1: Kernel Fundamentals

**Already a developer?** Jump to [Learning Path: Contributor](LEARNING_PATH_CONTRIBUTOR.md)

---

*Last Updated: 2026-05-26*
