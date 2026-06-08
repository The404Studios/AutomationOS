#ifndef UAPI_SYSCALLS_H
#define UAPI_SYSCALLS_H

/*
 * UAPI: Kernel/Userspace ABI contract for syscall numbers.
 *
 * RULE: Both kernel (kernel/include/syscall.h) and userspace apps
 * should use these numbers. Userspace apps that currently hardcode
 * syscall numbers (#define SYS_EXIT 0) should migrate to including
 * this header. Until then, any change here must be mirrored in
 * kernel/include/syscall.h AND grepped across userspace.
 */

#define UAPI_SYS_EXIT           0
#define UAPI_SYS_FORK           1
#define UAPI_SYS_SPAWN          2
#define UAPI_SYS_WRITE          3
#define UAPI_SYS_OPEN           4
#define UAPI_SYS_CLOSE          5
#define UAPI_SYS_READ           6
#define UAPI_SYS_YIELD          7
#define UAPI_SYS_GETPID         8
#define UAPI_SYS_WAITPID        9
#define UAPI_SYS_SLEEP         10
#define UAPI_SYS_FB_ACQUIRE   11
#define UAPI_SYS_MMAP         14
#define UAPI_SYS_MUNMAP       15
#define UAPI_SYS_GETTIME      42
#define UAPI_SYS_TIME         41
#define UAPI_SYS_GET_TICKS_MS 40
#define UAPI_SYS_SYSINFO      62

/* Sockets */
#define UAPI_SYS_SOCKET        51
#define UAPI_SYS_CONNECT       52
#define UAPI_SYS_SEND          53
#define UAPI_SYS_RECV          54
#define UAPI_SYS_CLOSE_SK      55
#define UAPI_SYS_SENDTO        56
#define UAPI_SYS_RECVFROM      57
#define UAPI_SYS_SOCK_POLL     58
#define UAPI_SYS_BIND          76
#define UAPI_SYS_LISTEN        77
#define UAPI_SYS_ACCEPT        78

/* Networking */
#define UAPI_SYS_NET_INFO      59
#define UAPI_SYS_NET_SEND      68
#define UAPI_SYS_NET_RECV      69
#define UAPI_SYS_NET_CONFIG    89
#define UAPI_SYS_ROUTE_TABLE   90
#define UAPI_SYS_ARP_TABLE     91
#define UAPI_SYS_PCI_LIST      92

/* IPC */
#define UAPI_SYS_SHMGET        20
#define UAPI_SYS_SHMAT         21
#define UAPI_SYS_SHMDT         22
#define UAPI_SYS_SHMCTL        23
#define UAPI_SYS_MSGSND        24
#define UAPI_SYS_MSGRCV        25
#define UAPI_SYS_MSGGET        26
#define UAPI_SYS_MSGCTL        27
#define UAPI_SYS_CLIP_SET      63
#define UAPI_SYS_CLIP_GET      64
#define UAPI_SYS_NOTIFY        65
#define UAPI_SYS_NOTIFY_POLL   66

#endif /* UAPI_SYSCALLS_H */
