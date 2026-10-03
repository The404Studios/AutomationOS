#ifndef FIREWALL_H
#define FIREWALL_H

#include "types.h"
#include "uapi/fw.h"

/*
 * FW-0 kernel packet filter. Chokepoints (kernel/net/net.c):
 *   net_recv() -> fw_ingress()   every frame the NIC hands up, BEFORE ARP/ICMP/TCP/UDP
 *   net_send() -> fw_egress()    every frame the stack or a raw sender puts on the wire
 * Both take the full Ethernet frame and return FW_VERDICT_*. Non-IPv4 (ARP etc.) is
 * never filtered here. Loopback never reaches either hook.
 *
 * Any future netif->tx data path (WiFi K1) MUST call fw_egress()/fw_ingress() too, or
 * it becomes a bypass.
 */
#define FW_VERDICT_ACCEPT 0
#define FW_VERDICT_DROP   1

void fw_init(void);
int  fw_ingress(const uint8_t* frame, uint16_t len);
int  fw_egress(const uint8_t* frame, uint16_t len);

/* In-kernel boot self-test (pure logic over crafted frames, no NIC). Prints
 * "FW-SELFTEST: PASS ..." / FAIL. Returns 0 on pass. */
int  fw_selftest(void);

/* SYS_FW_CTL(user uapi_fw_req_t*) */
int64_t sys_fw_ctl(uint64_t req_ptr, uint64_t a2, uint64_t a3,
                   uint64_t a4, uint64_t a5, uint64_t a6);

#endif /* FIREWALL_H */
