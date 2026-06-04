#include "include/types.h"
#include "include/kernel.h"

// perf_calibrate_cpu_freq now provided by the real kernel/lib/perf.c.
// syscall_msr_init moved to arch/x86_64/syscall_init.c

int pid_namespace_create(void) { return 0; }
int mount_namespace_create(void) { return 0; }
int net_namespace_create(void) { return 0; }
int ipc_namespace_create(void) { return 0; }
int uts_namespace_create(void) { return 0; }
void pid_namespace_destroy(void* ns) { (void)ns; }
void mount_namespace_destroy(void* ns) { (void)ns; }
void net_namespace_destroy(void* ns) { (void)ns; }
void ipc_namespace_destroy(void* ns) { (void)ns; }
void uts_namespace_destroy(void* ns) { (void)ns; }
void mount_namespace_clone(void* ns) { (void)ns; }
void uts_namespace_set_hostname(void* ns, const char* name) { (void)ns; (void)name; }
void uts_namespace_set_domainname(void* ns, const char* name) { (void)ns; (void)name; }

/* Single-CPU / no-preemption stubs. The real cpu_id()/scheduler_tick() live in
 * the SMP files (smp.c, scheduler_smp.c) which are intentionally NOT compiled --
 * their large static DMA/.bss bloat overlaps the GRUB-placed initrd (see the
 * note in scripts/quick_build.sh). The kernel is cooperative + single-core
 * today, so these are correct: CPU 0, no preemptive tick. vfs_sync_all() is a
 * no-op flush (called from the panic path). Defining them here lets the kernel
 * link without dragging in the deferred SMP subsystem. */
/* Under SMP_SCHED the REAL cpu_id() (xAPIC id -> logical id) lives in
 * kernel/arch/x86_64/ap_boot.c. #ifndef it out here so the link has exactly one
 * definition. Without SMP_SCHED (default + plain SMP=1 coprocessor build) this
 * single-CPU stub stands. */
#ifndef SMP_SCHED
uint32_t cpu_id(void) { return 0; }
#endif
void     scheduler_tick(void) { }
void     vfs_sync_all(void) { }
