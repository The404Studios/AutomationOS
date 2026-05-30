/**
 * Lock Dependency Validator (Lockdep-Lite)
 * =========================================
 *
 * Runtime deadlock detection inspired by Linux kernel lockdep.
 * Tracks lock acquisition order and detects potential deadlocks.
 *
 * Features:
 * - Lock class hierarchy
 * - Lock order validation
 * - Circular dependency detection
 * - Per-lock statistics
 * - Debug mode with verbose logging
 *
 * Usage:
 *   1. Define lock classes in lockdep_class_t enum
 *   2. Tag each lock with its class using lockdep_init()
 *   3. Use lockdep_lock()/lockdep_unlock() wrappers
 *   4. System will panic on lock order violation in debug mode
 */

#ifndef LOCKDEP_H
#define LOCKDEP_H

#include "types.h"
#include "spinlock.h"

// Lock classes (hierarchy)
typedef enum {
    LOCKDEP_CLASS_NONE = 0,

    // Memory management locks (highest priority)
    LOCKDEP_CLASS_MM_GLOBAL,     // Global memory allocator lock
    LOCKDEP_CLASS_MM_CACHE,      // Per-CPU cache locks

    // Bus/Device locks
    LOCKDEP_CLASS_BUS,           // Bus-wide locks
    LOCKDEP_CLASS_DEVICE_PARENT, // Parent device locks
    LOCKDEP_CLASS_DEVICE,        // Device locks
    LOCKDEP_CLASS_DEVICE_CHILD,  // Child device locks

    // IRQ locks
    LOCKDEP_CLASS_IRQ_DESC,      // IRQ descriptor locks

    // Filesystem locks
    LOCKDEP_CLASS_FS_SUPER,      // Superblock locks
    LOCKDEP_CLASS_FS_INODE,      // Inode locks
    LOCKDEP_CLASS_FS_DENTRY,     // Dentry locks

    // Audit/logging locks
    LOCKDEP_CLASS_AUDIT,         // Audit buffer locks

    // Must be last
    LOCKDEP_CLASS_MAX
} lockdep_class_t;

// Lock dependency edge (A → B means "A acquired before B")
typedef struct lockdep_edge {
    lockdep_class_t from;
    lockdep_class_t to;
    const char* from_file;
    int from_line;
    const char* to_file;
    int to_line;
    uint64_t count;              // How many times this edge was observed
    struct lockdep_edge* next;
} lockdep_edge_t;

// Per-lock metadata
typedef struct lockdep_lock {
    lockdep_class_t class;
    const char* name;
    const char* file;
    int line;
    uint64_t acquired_count;     // Total times acquired
    uint64_t contended_count;    // Times had to wait
    uint32_t owner_cpu;          // CPU that owns the lock (or -1)
} lockdep_lock_t;

// Per-CPU lock stack (to track what locks are held)
#define LOCKDEP_MAX_STACK_DEPTH 16

typedef struct lockdep_cpu_state {
    lockdep_lock_t* lock_stack[LOCKDEP_MAX_STACK_DEPTH];
    uint32_t stack_depth;
} lockdep_cpu_state_t;

// Global lockdep state
extern bool lockdep_enabled;
extern lockdep_cpu_state_t lockdep_cpu_states[8];  // MAX_CPUS
extern lockdep_edge_t* lockdep_edges;

// Initialize lockdep subsystem
void lockdep_init(void);

// Enable/disable lockdep (can disable for performance)
void lockdep_enable(void);
void lockdep_disable(void);

// Tag a lock with its class and metadata
void lockdep_tag_lock(spinlock_t* lock, lockdep_class_t class,
                     const char* name, const char* file, int line);

// Lock acquisition/release tracking
void lockdep_acquire(spinlock_t* lock, const char* file, int line);
void lockdep_release(spinlock_t* lock, const char* file, int line);

// Check if acquiring this lock would violate lock order
bool lockdep_check_order(lockdep_class_t class, const char* file, int line);

// Detect circular dependencies in lock graph
bool lockdep_detect_cycle(lockdep_class_t from, lockdep_class_t to);

// Print lock dependency graph (for debugging)
void lockdep_print_graph(void);

// Print current lock stack for CPU
void lockdep_print_stack(uint32_t cpu_id);

// Print statistics
void lockdep_print_stats(void);

// Wrapper macros for instrumented locking
#ifdef CONFIG_LOCKDEP

#define LOCKDEP_TAG(lock, class, name) \
    lockdep_tag_lock(lock, class, name, __FILE__, __LINE__)

#define LOCKDEP_LOCK(lock) \
    do { \
        lockdep_acquire(lock, __FILE__, __LINE__); \
        spin_lock(lock); \
    } while (0)

#define LOCKDEP_UNLOCK(lock) \
    do { \
        spin_unlock(lock); \
        lockdep_release(lock, __FILE__, __LINE__); \
    } while (0)

#define LOCKDEP_TRYLOCK(lock, result) \
    do { \
        result = spin_trylock(lock); \
        if (result) { \
            lockdep_acquire(lock, __FILE__, __LINE__); \
        } \
    } while (0)

#else

// Lockdep disabled - no overhead
#define LOCKDEP_TAG(lock, class, name) do { } while (0)
#define LOCKDEP_LOCK(lock) spin_lock(lock)
#define LOCKDEP_UNLOCK(lock) spin_unlock(lock)
#define LOCKDEP_TRYLOCK(lock, result) result = spin_trylock(lock)

#endif

// Lock order matrix (compile-time definition)
// Matrix[A][B] = true means "can acquire B while holding A"
extern const bool lockdep_order_matrix[LOCKDEP_CLASS_MAX][LOCKDEP_CLASS_MAX];

// Human-readable class names
extern const char* lockdep_class_names[LOCKDEP_CLASS_MAX];

#endif // LOCKDEP_H
