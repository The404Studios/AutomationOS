/*
 * Networking syscall helpers (proposed SYS_NET_* surface).
 * ========================================================
 *
 * These three handlers match the kernel's syscall_handler_t shape so the
 * integrator can wire them with a single line each in the dispatcher. They are
 * fully self-contained here (no edits to the shared syscall glue are needed to
 * COMPILE this file). To ACTIVATE them the integrator adds, in
 * kernel/include/syscall.h:
 *
 *     #define SYS_NET_SEND   41
 *     #define SYS_NET_RECV   42
 *     #define SYS_NET_INFO   43
 *
 * and in the syscall dispatcher (kernel/core/syscall/syscall.c or wherever the
 * jump table lives):
 *
 *     case SYS_NET_SEND: return sys_net_send(a1,a2,a3,a4,a5,a6);
 *     case SYS_NET_RECV: return sys_net_recv(a1,a2,a3,a4,a5,a6);
 *     case SYS_NET_INFO: return sys_net_info(a1,a2,a3,a4,a5,a6);
 *
 * ABI:
 *   sys_net_send(user_buf, len)   -> bytes sent (>0) | -EINVAL | -EFAULT
 *   sys_net_recv(user_buf, len)   -> frame len (>0) | 0 (none) | -errno
 *   sys_net_info(user net_info_t) -> 0 | -errno   (fills mac/ip/gateway)
 *
 * Scope: kernel/net/netsyscall.c (new tree).
 */

#include "../include/net.h"
#include "../include/types.h"
#include "../include/mem.h"      /* copy_from_user / copy_to_user, COPY_*  */
#include "../include/errno.h"    /* canonical negative errno (EINVAL/EFAULT/ENOTSUP) */

/* Bound user transfers to one Ethernet frame. */
#define NET_SYS_MAX_FRAME  ETH_MAX_FRAME

int64_t sys_net_send(uint64_t buf, uint64_t len, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!net_up()) return ENOTSUP;
    if (buf == 0 || len == 0 || len > NET_SYS_MAX_FRAME) return EINVAL;

    uint8_t kframe[NET_SYS_MAX_FRAME];
    if (copy_from_user(kframe, (const void*)buf, (size_t)len) != COPY_SUCCESS) {
        return EFAULT;
    }

    int sent = net_send(kframe, (uint16_t)len);
    return (sent < 0) ? (int64_t)EINVAL : (int64_t)sent;
}

int64_t sys_net_recv(uint64_t buf, uint64_t len, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!net_up()) return ENOTSUP;
    if (buf == 0 || len == 0) return EINVAL;

    uint16_t cap = (len > NET_SYS_MAX_FRAME) ? NET_SYS_MAX_FRAME : (uint16_t)len;

    uint8_t kframe[NET_SYS_MAX_FRAME];
    int n = net_recv(kframe, cap);
    if (n <= 0) {
        return (int64_t)n;   /* 0 = nothing pending, <0 = error */
    }

    if (copy_to_user((void*)buf, kframe, (size_t)n) != COPY_SUCCESS) {
        return EFAULT;
    }
    return (int64_t)n;
}

int64_t sys_net_info(uint64_t out_info, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!net_up()) return ENOTSUP;
    if (out_info == 0) return EINVAL;

    net_info_t info;
    if (net_get_mac(info.mac) != 0) return ENOTSUP;
    info._pad[0] = 0;
    info._pad[1] = 0;
    info.ip      = net_get_ip();
    info.gateway = NET_QEMU_GATEWAY;

    if (copy_to_user((void*)out_info, &info, sizeof(info)) != COPY_SUCCESS) {
        return EFAULT;
    }
    return 0;
}
