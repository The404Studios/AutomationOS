#ifndef UAPI_FW_H
#define UAPI_FW_H

/*
 * UAPI: kernel packet filter (FW-0) control ABI -- SYS_FW_CTL.
 *
 * RULE (same as uapi/net.h): a struct the kernel copies to/from userspace
 * lives HERE, with an ABI_SIZE constant and a _Static_assert. Kernel and
 * userspace (fwctl) both include this file. Any change is an ABI break.
 *
 * Model: stateful, IPv4, two chains (IN / OUT), first-match-wins rule list per
 * chain, a per-chain default policy, and a connection tracker that lets the
 * RETURN traffic of anything this host initiated back in automatically.
 * Loopback (127/8) is always trusted and never filtered.
 */

#ifdef __KERNEL__
#include "../types.h"
#else
#ifndef _UAPI_STDINT_DEFINED
#define _UAPI_STDINT_DEFINED
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
#endif
typedef int int32_t;
#endif

#define FW_DIR_IN        0
#define FW_DIR_OUT       1

#define FW_ACT_ACCEPT    0
#define FW_ACT_DROP      1

#define FW_PROTO_ANY     0          /* otherwise the IP protocol number (1/6/17) */

#define FW_RULE_LOG      0x01       /* serial-log (rate-limited) when this rule matches */

#define FW_MAX_RULES     64

/* SYS_FW_CTL operations. Everything except STATUS/GET is privileged
 * (CAP_NET_ADMIN -- see kernel/include/proc_caps.h). */
#define FW_OP_STATUS       0        /* fill req.stats                                  */
#define FW_OP_GET          1        /* req.index -> req.rule + req.hits                */
#define FW_OP_ADD          2        /* append req.rule to its chain; returns index     */
#define FW_OP_INSERT       3        /* insert req.rule at req.index                    */
#define FW_OP_DEL          4        /* delete rule req.index                           */
#define FW_OP_FLUSH        5        /* delete every rule (policies unchanged)          */
#define FW_OP_SET_POLICY   6        /* req.arg = dir | (action << 8)                   */
#define FW_OP_ENABLE       7        /* req.arg = 0 (off) | 1 (on)                      */
#define FW_OP_SET_PING     8        /* req.arg = 0/1: answer inbound ICMP echo         */
#define FW_OP_RESET        9        /* flush + default policies + conntrack cleared    */

/* One rule. IP addresses/masks are HOST byte order; mask 0 = any address.
 * Ports are host order, inclusive ranges; 0..65535 = any. Only meaningful for
 * TCP/UDP. */
#define FW_RULE_ABI_SIZE 32
typedef struct {
    uint8_t  dir;               /* FW_DIR_*                         */
    uint8_t  action;            /* FW_ACT_*                         */
    uint8_t  proto;             /* FW_PROTO_ANY or 1/6/17           */
    uint8_t  flags;             /* FW_RULE_*                        */
    uint8_t  _pad[4];
    uint32_t src, src_mask;
    uint32_t dst, dst_mask;
    uint16_t sport_lo, sport_hi;
    uint16_t dport_lo, dport_hi;
} uapi_fw_rule_t;

#define FW_STATS_ABI_SIZE 80
typedef struct {
    uint32_t enabled;
    uint32_t rules;
    uint32_t ct_entries;
    uint8_t  policy_in;         /* FW_ACT_*                         */
    uint8_t  policy_out;
    uint8_t  allow_ping;
    uint8_t  _pad;
    uint64_t in_accept, in_drop;
    uint64_t out_accept, out_drop;
    uint64_t ct_hits;           /* inbound packets admitted as return traffic */
    uint64_t ct_new;            /* conntrack entries created                  */
    uint64_t ct_evicted;        /* entries recycled under table pressure      */
    uint64_t bad_drop;          /* sanity drops (spoof/flag-scan/fragment)    */
} uapi_fw_stats_t;

#define FW_REQ_ABI_SIZE 136
typedef struct {
    uint32_t op;                /* FW_OP_*                          */
    int32_t  index;
    uint32_t arg;
    uint32_t _pad;
    uapi_fw_rule_t rule;        /* ADD/INSERT in, GET out           */
    uint64_t hits;              /* GET out: times this rule matched */
    uapi_fw_stats_t stats;      /* STATUS out                       */
} uapi_fw_req_t;

_Static_assert(sizeof(uapi_fw_rule_t)  == FW_RULE_ABI_SIZE,  "uapi_fw_rule_t ABI drift");
_Static_assert(sizeof(uapi_fw_stats_t) == FW_STATS_ABI_SIZE, "uapi_fw_stats_t ABI drift");
_Static_assert(sizeof(uapi_fw_req_t)   == FW_REQ_ABI_SIZE,   "uapi_fw_req_t ABI drift");

#endif /* UAPI_FW_H */
