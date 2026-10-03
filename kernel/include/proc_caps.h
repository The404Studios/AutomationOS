#ifndef PROC_CAPS_H
#define PROC_CAPS_H

#include "types.h"

/*
 * PCAP-0 -- minimal, ENFORCED per-process privilege mask.
 *
 * Why this exists: the older capability.h / mac.h / seccomp.h framework is not
 * compiled or wired to any syscall, so until now ANY ring-3 process could send
 * and sniff raw Ethernet frames, reprogram the IP/gateway/DNS, and (with FW-0)
 * rewrite the firewall. A packet filter that any app can switch off is not a
 * filter, and an automation agent with no kernel-enforced boundary has only
 * userspace policy between it and the machine.
 *
 * Model: process_t.cap_denied is a bitmask of PCAP_* privileges the process has
 * given up. ZERO means "nothing denied" (everything permitted) -- the default
 * for init and everything it spawns, so adding this changes no behaviour until
 * something drops. Drops are MONOTONIC (SYS_CAP_DROP can only add bits) and are
 * INHERITED by every child (spawn/fork/thread). An automation host drops the
 * dangerous privileges on itself first, and every tool it launches inherits the
 * reduced set no matter what the tool's code tries to do.
 */
#define PCAP_NET_RAW    (1ULL << 0)   /* SYS_NET_SEND / SYS_NET_RECV (raw Ethernet frames) */
#define PCAP_NET_ADMIN  (1ULL << 1)   /* SYS_NET_CONFIG, SYS_FW_CTL writes, WLAN control    */
#define PCAP_SYS_ADMIN  (1ULL << 2)   /* reserved: mount/persist/power/module-class calls  */
#define PCAP_PROC_CTL   (1ULL << 3)   /* reserved: signalling/inspecting foreign processes */
#define PCAP_ALL        (PCAP_NET_RAW | PCAP_NET_ADMIN | PCAP_SYS_ADMIN | PCAP_PROC_CTL)

/* 1 = the CURRENT process may use `cap`; 0 = it has dropped it. Kernel-internal
 * callers (no current process) are always permitted. */
int proc_cap_check(uint64_t cap);

/* SYS_CAP_DROP(mask) -> the new denied mask. Only ever adds bits. */
int64_t sys_cap_drop(uint64_t mask, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6);
/* SYS_CAP_QUERY() -> the current denied mask. */
int64_t sys_cap_query(uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6);

#endif /* PROC_CAPS_H */
