#ifndef IRQ_H
#define IRQ_H

#include "types.h"

// Forward declarations
struct device;
struct irq_desc;

// IRQ flags
#define IRQ_FLAG_SHARED        0x0001  // Can be shared with other handlers
#define IRQ_FLAG_ONESHOT       0x0002  // Run handler once, then mask
#define IRQ_FLAG_PROBE         0x0004  // IRQ is being probed
#define IRQ_FLAG_NO_SUSPEND    0x0008  // Don't disable during suspend
#define IRQ_FLAG_FORCE_RESUME  0x0010  // Force enable on resume
#define IRQ_FLAG_EARLY_RESUME  0x0020  // Resume early in the resume sequence
#define IRQ_FLAG_TIMER         0x0040  // IRQ is used for timer
#define IRQ_FLAG_PERCPU        0x0080  // IRQ is per-CPU
#define IRQ_FLAG_NO_THREAD     0x0100  // Don't thread this IRQ
#define IRQ_FLAG_NO_DEBUG      0x0200  // Exclude from IRQ debugging
#define IRQ_FLAG_NO_BALANCING  0x0400  // Don't move to other CPUs

// IRQ return values
typedef enum {
    IRQ_NONE = 0,        // Not our interrupt
    IRQ_HANDLED,         // Interrupt handled
    IRQ_WAKE_THREAD      // Wake threaded handler
} irq_return_t;

// IRQ handler types
typedef irq_return_t (*irq_handler_t)(uint32_t irq, void* dev_id);
typedef irq_return_t (*irq_thread_fn_t)(uint32_t irq, void* dev_id);

// IRQ action structure (linked list of handlers for shared IRQs)
typedef struct irq_action {
    irq_handler_t handler;
    irq_thread_fn_t thread_fn;
    void* dev_id;
    const char* name;
    uint32_t flags;
    struct irq_action* next;
    void* thread;            // Thread structure for threaded IRQs
    uint64_t count;          // Number of times invoked
    uint64_t unhandled;      // Number of unhandled interrupts
} irq_action_t;

// IRQ chip operations (hardware-specific)
typedef struct irq_chip {
    const char* name;
    void (*enable)(uint32_t irq);
    void (*disable)(uint32_t irq);
    void (*ack)(uint32_t irq);
    void (*mask)(uint32_t irq);
    void (*unmask)(uint32_t irq);
    void (*eoi)(uint32_t irq);
    int (*set_affinity)(uint32_t irq, uint32_t cpu_mask);
    int (*set_type)(uint32_t irq, uint32_t type);
    int (*set_wake)(uint32_t irq, bool enable);
} irq_chip_t;

// IRQ trigger types
#define IRQ_TYPE_NONE          0x00000000
#define IRQ_TYPE_EDGE_RISING   0x00000001
#define IRQ_TYPE_EDGE_FALLING  0x00000002
#define IRQ_TYPE_EDGE_BOTH     0x00000003
#define IRQ_TYPE_LEVEL_HIGH    0x00000004
#define IRQ_TYPE_LEVEL_LOW     0x00000008

// IRQ descriptor (per-IRQ state)
typedef struct irq_desc {
    uint32_t irq_num;
    irq_chip_t* chip;
    irq_action_t* action;
    uint32_t status;
    uint32_t depth;          // Nested disable depth
    uint32_t wake_depth;
    uint32_t cpu_affinity;
    uint64_t count;          // Total invocations
    uint64_t unhandled;
    void* lock;              // spinlock_t
} irq_desc_t;

// IRQ status flags
#define IRQ_STATUS_ENABLED     0x0001
#define IRQ_STATUS_PENDING     0x0002
#define IRQ_STATUS_MASKED      0x0004
#define IRQ_STATUS_INPROGRESS  0x0008
#define IRQ_STATUS_DISABLED    0x0010
#define IRQ_STATUS_WAKEUP      0x0020
#define IRQ_STATUS_SPURIOUS    0x0040

// MSI/MSI-X support
typedef struct {
    uint32_t entry;          // Entry number
    uint32_t vector;         // Interrupt vector
    void* address;           // MSI address
    uint32_t data;           // MSI data
} msi_entry_t;

typedef struct {
    bool enabled;
    bool is_msix;            // true = MSI-X, false = MSI
    uint32_t num_vectors;
    msi_entry_t* entries;
    void* mask_base;         // MSI-X mask table
} msi_info_t;

// IRQ core API
int irq_init(void);

// IRQ handler registration
int request_irq(uint32_t irq, irq_handler_t handler, uint32_t flags,
               const char* name, void* dev_id);
int request_threaded_irq(uint32_t irq, irq_handler_t handler,
                        irq_thread_fn_t thread_fn, uint32_t flags,
                        const char* name, void* dev_id);
void free_irq(uint32_t irq, void* dev_id);

// IRQ control
void disable_irq(uint32_t irq);
void disable_irq_nosync(uint32_t irq);
void enable_irq(uint32_t irq);
void synchronize_irq(uint32_t irq);

// IRQ affinity (bind to CPU)
int irq_set_affinity(uint32_t irq, uint32_t cpu_mask);
uint32_t irq_get_affinity(uint32_t irq);

// IRQ chip management
int irq_set_chip(uint32_t irq, irq_chip_t* chip);
irq_chip_t* irq_get_chip(uint32_t irq);
int irq_set_chip_data(uint32_t irq, void* data);
void* irq_get_chip_data(uint32_t irq);

// IRQ descriptor access
irq_desc_t* irq_to_desc(uint32_t irq);
bool irq_has_action(uint32_t irq);

// Generic IRQ handler (called from low-level interrupt code)
void generic_handle_irq(uint32_t irq);

// MSI/MSI-X API
int msi_enable(struct device* dev);
void msi_disable(struct device* dev);
int msix_enable(struct device* dev, uint32_t num_vectors);
void msix_disable(struct device* dev);
int msi_alloc_vectors(struct device* dev, uint32_t num_vectors);
void msi_free_vectors(struct device* dev);
uint32_t msi_get_vector(struct device* dev, uint32_t index);

// MSI-X specific
int msix_set_entry(struct device* dev, uint32_t entry, uint32_t vector);
void msix_mask_entry(struct device* dev, uint32_t entry);
void msix_unmask_entry(struct device* dev, uint32_t entry);

// IRQ statistics and debugging
void irq_print_stats(void);
uint64_t irq_get_count(uint32_t irq);
uint64_t irq_get_unhandled_count(uint32_t irq);
bool irq_is_spurious(uint32_t irq);

// IRQ tracing and profiling
void irq_trace_enable(void);
void irq_trace_disable(void);
uint64_t irq_get_latency(uint32_t irq);
uint64_t irq_get_handler_time(uint32_t irq);

// Threaded IRQ support
bool irq_is_threaded(uint32_t irq);
void irq_wake_thread(uint32_t irq);

// Per-CPU IRQs
int request_percpu_irq(uint32_t irq, irq_handler_t handler, const char* name, void* dev_id);
void free_percpu_irq(uint32_t irq, void* dev_id);
void enable_percpu_irq(uint32_t irq);
void disable_percpu_irq(uint32_t irq);

// Deferred interrupt work
typedef void (*irq_work_fn_t)(void* data);

typedef struct {
    irq_work_fn_t func;
    void* data;
    uint32_t flags;
    void* next;  // For work queue
} irq_work_t;

void irq_work_queue(irq_work_t* work);
void irq_work_sync(irq_work_t* work);

// Software interrupts (softirq) for deferred processing
#define SOFTIRQ_TIMER    0
#define SOFTIRQ_NET_TX   1
#define SOFTIRQ_NET_RX   2
#define SOFTIRQ_BLOCK    3
#define SOFTIRQ_TASKLET  4
#define SOFTIRQ_SCHED    5
#define SOFTIRQ_HRTIMER  6
#define SOFTIRQ_RCU      7
#define NR_SOFTIRQS      8

typedef void (*softirq_fn_t)(void* data);

void softirq_init(void);
void raise_softirq(uint32_t nr);
void open_softirq(uint32_t nr, softirq_fn_t func, void* data);
void do_softirq(void);

// Tasklets (built on softirqs)
typedef struct tasklet {
    void (*func)(uint64_t data);
    uint64_t data;
    uint32_t state;
    struct tasklet* next;
} tasklet_t;

void tasklet_init(tasklet_t* t, void (*func)(uint64_t), uint64_t data);
void tasklet_schedule(tasklet_t* t);
void tasklet_hi_schedule(tasklet_t* t);
void tasklet_kill(tasklet_t* t);

// IRQ polling (for debugging stuck interrupts)
void irq_poll_enable(void);
void irq_poll_disable(void);

#endif
