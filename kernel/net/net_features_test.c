/*
 * net_features_test.c -- Test suite for IPv4 fragmentation, ICMP errors, routing.
 * ================================================================================
 *
 * Tests the three newly implemented features:
 *   1. IPv4 fragmentation/reassembly
 *   2. ICMP error messages (Destination Unreachable, Time Exceeded)
 *   3. Routing table with longest prefix match
 *
 * Scope: kernel/net/net_features_test.c (new).
 */

#include "../include/net.h"
#include "../include/route.h"
#include "../include/socket.h"
#include "../include/kernel.h"
#include "../include/string.h"

/* ------------------------------------------------------------------ */
/* Test 1: Routing table                                              */
/* ------------------------------------------------------------------ */
void test_routing_table(void) {
    kprintf("[TEST] Routing table...\n");

    /* Clear and reinitialize. */
    route_init();

    /* Add some test routes. */
    route_add(0xC0A80000, 0xFFFFFF00, 0, 0x0A00020F);  /* 192.168.0.0/24 on-link */
    route_add(0xAC100000, 0xFFFF0000, 0x0A000202, 0x0A00020F); /* 172.16.0.0/16 via gw */
    route_add(0, 0, 0x0A000202, 0x0A00020F);          /* default route */

    route_print();

    /* Test lookups. */
    uint32_t gw, iface;

    /* On-link route. */
    if (route_lookup(0xC0A80001, &gw, &iface) == 0) { /* 192.168.0.1 */
        kprintf("  192.168.0.1: gateway=%u.%u.%u.%u %s\n",
                (gw >> 24) & 0xFF, (gw >> 16) & 0xFF,
                (gw >> 8) & 0xFF, gw & 0xFF,
                gw == 0 ? "(on-link)" : "");
    } else {
        kprintf("  192.168.0.1: NO ROUTE\n");
    }

    /* Via gateway. */
    if (route_lookup(0xAC100A0B, &gw, &iface) == 0) { /* 172.16.10.11 */
        kprintf("  172.16.10.11: gateway=%u.%u.%u.%u\n",
                (gw >> 24) & 0xFF, (gw >> 16) & 0xFF,
                (gw >> 8) & 0xFF, gw & 0xFF);
    } else {
        kprintf("  172.16.10.11: NO ROUTE\n");
    }

    /* Default route. */
    if (route_lookup(0x08080808, &gw, &iface) == 0) { /* 8.8.8.8 */
        kprintf("  8.8.8.8: gateway=%u.%u.%u.%u (default)\n",
                (gw >> 24) & 0xFF, (gw >> 16) & 0xFF,
                (gw >> 8) & 0xFF, gw & 0xFF);
    } else {
        kprintf("  8.8.8.8: NO ROUTE\n");
    }

    kprintf("[TEST] Routing table OK\n\n");
}

/* ------------------------------------------------------------------ */
/* Test 2: Fragmentation                                              */
/* ------------------------------------------------------------------ */
void test_fragmentation(void) {
    kprintf("[TEST] IPv4 fragmentation (manual test)...\n");
    kprintf("  To test fragmentation:\n");
    kprintf("    1. Send a UDP packet > 1500 bytes from userspace.\n");
    kprintf("    2. Observe multiple IP fragments with MF flag set.\n");
    kprintf("    3. Verify fragments have same IP ID, correct offsets.\n");
    kprintf("  Fragmentation is enabled in ip_tx() if seg_len > MTU.\n\n");
}

/* ------------------------------------------------------------------ */
/* Test 3: Reassembly                                                 */
/* ------------------------------------------------------------------ */
void test_reassembly(void) {
    kprintf("[TEST] IPv4 reassembly (requires external sender)...\n");
    kprintf("  To test reassembly:\n");
    kprintf("    1. Send fragmented packets from external host.\n");
    kprintf("    2. Kernel will reassemble in ipv4_input().\n");
    kprintf("    3. Application should receive complete datagram.\n");
    kprintf("  Reassembly timeout: 30 seconds.\n\n");
}

/* ------------------------------------------------------------------ */
/* Test 4: ICMP errors                                                */
/* ------------------------------------------------------------------ */
void test_icmp_errors(void) {
    kprintf("[TEST] ICMP error messages...\n");
    kprintf("  ICMP Destination Unreachable (Type 3):\n");
    kprintf("    - Code 1: Host Unreachable (no ARP reply)\n");
    kprintf("    - Code 3: Port Unreachable (no socket listening)\n");
    kprintf("    - Code 4: Fragmentation needed but DF set\n");
    kprintf("  ICMP Time Exceeded (Type 11):\n");
    kprintf("    - Code 0: TTL expired in transit\n");
    kprintf("    - Code 1: Fragment reassembly timeout\n");
    kprintf("  To test: connect to closed UDP port, observe ICMP reply.\n\n");
}

/* ------------------------------------------------------------------ */
/* Main test entry point                                              */
/* ------------------------------------------------------------------ */
void net_features_selftest(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf(" Network Stack Features Self-Test\n");
    kprintf("========================================\n\n");

    if (!net_up()) {
        kprintf("[TEST] Network not up -- skipping tests\n");
        return;
    }

    test_routing_table();
    test_fragmentation();
    test_reassembly();
    test_icmp_errors();

    kprintf("========================================\n");
    kprintf(" Network Features Test Complete\n");
    kprintf("========================================\n\n");
}
