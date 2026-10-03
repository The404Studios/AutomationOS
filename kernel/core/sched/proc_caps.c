/*
 * proc_caps.c -- PCAP-0 enforcement + the two syscalls. See include/proc_caps.h.
 */
#include "../../include/proc_caps.h"
#include "../../include/sched.h"
#include "../../include/errno.h"

int proc_cap_check(uint64_t cap) {
    process_t* cur = process_get_current();
    if (!cur) return 1;                       /* kernel context: not a ring-3 caller */
    return (cur->cap_denied & cap) == 0;
}

int64_t sys_cap_drop(uint64_t mask, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    process_t* cur = process_get_current();
    if (!cur) return ESRCH;
    cur->cap_denied |= (mask & PCAP_ALL);     /* monotonic: never clears a bit */
    return (int64_t)cur->cap_denied;
}

int64_t sys_cap_query(uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    process_t* cur = process_get_current();
    if (!cur) return ESRCH;
    return (int64_t)cur->cap_denied;
}
