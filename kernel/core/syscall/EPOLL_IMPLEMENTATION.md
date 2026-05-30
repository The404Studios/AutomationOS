# Epoll Implementation for AutomationOS

## Overview

This document describes the Linux-compatible epoll implementation for AutomationOS, providing scalable event-driven I/O multiplexing.

## Architecture

### Components

1. **Epoll Core** (`kernel/core/syscall/epoll.c`)
   - Epoll instance management
   - Watch list tracking
   - Edge-triggered event detection
   - Wait queue integration

2. **Syscalls** (syscall numbers 73-75)
   - `SYS_EPOLL_CREATE` (73): Create epoll instance
   - `SYS_EPOLL_CTL` (74): Add/modify/remove watched fds
   - `SYS_EPOLL_WAIT` (75): Block until events ready

3. **Userspace API** (`userspace/lib/epoll.h`)
   - Convenient wrappers for epoll syscalls
   - Linux-compatible interface

## Design Decisions

### Edge-Triggered Semantics

**Why edge-triggered?**
- **Scalability**: O(1) wakeup cost (only processes waiting on active epoll instances wake up)
- **Efficiency**: Applications are forced to drain all available data, reducing syscall overhead
- **Simplicity**: Kernel doesn't track "level" state, only state changes

**Trade-offs**:
- Applications must use non-blocking I/O and loop until EAGAIN
- Easier to miss events if not careful (but better performance when done right)

### Data Structures

```c
typedef struct epoll_instance {
    bool used;
    spinlock_t lock;
    epoll_watch_t watches[EPOLL_MAX_WATCHES];    // 256 max watches per instance
    ready_event_t ready_events[EPOLL_MAX_EVENTS]; // 128 event ring buffer
    int ready_head, ready_tail, ready_count;
    wait_queue_t wait_queue;                      // blocked processes
} epoll_instance_t;
```

**Why a ring buffer for ready events?**
- Fixed-size allocation (no dynamic memory in wait path)
- O(1) enqueue/dequeue
- Bounded memory per epoll instance (~3KB per instance)

**Why a watch array instead of hash table?**
- Simplicity: linear scan is fast for typical workloads (<100 watches)
- No dynamic allocation
- Good cache locality
- Easy to extend to hash table later if needed

### File Descriptor Encoding

Epoll fds are encoded as `0x10000 + instance_index` to distinguish from regular socket fds (0..15).

**Why this encoding?**
- Avoids collision with socket fd space
- Simple validation: `if (fd >= 0x10000 && fd < 0x10000 + MAX_INSTANCES)`
- No need for separate fd table

### Integration with Socket Layer

Currently simplified: `epoll_poll_socket()` always reports `EPOLLIN` for demonstration.

**Future integration**:
```c
// In kernel/net/socket.c, after receiving data:
void sock_input_notify(int sockfd) {
    epoll_notify_socket(sockfd, EPOLLIN);
}
```

This would wake epoll instances watching that socket.

## Scalability Analysis

### Memory Footprint

Per epoll instance:
- Instance struct: ~3KB
- 256 watches × 24 bytes = 6KB
- 128 ready events × 16 bytes = 2KB
- **Total: ~11KB per instance**

System-wide (64 instances max):
- **Total: ~704KB**

Per socket:
- No overhead (epoll scans watches, sockets don't track epoll)

### CPU Overhead

**Idle (no events)**:
- `epoll_wait()` blocks on wait queue → 0% CPU
- Only wakes when `epoll_notify_socket()` called

**Active (events arriving)**:
- Poll 256 watches: ~256 iterations (worst case)
- Add ready event: O(1)
- Wake one waiter: O(1)
- **Total: O(num_watches) per event arrival**

**Dequeue events**:
- Copy from ring buffer: O(num_ready_events)
- **Total: O(events_returned)**

### Comparison with Poll/Select

| Operation       | select()     | poll()       | epoll        |
|----------------|-------------|--------------|--------------|
| Add fd         | O(1)        | O(1)         | O(1)         |
| Remove fd      | O(1)        | O(1)         | O(1)         |
| Wait for events| O(n)        | O(n)         | O(1) *       |
| Return events  | O(n)        | O(m)         | O(m)         |
| Memory/fd      | 1 bit       | ~8 bytes     | 0 bytes **   |

\* O(watches) on first poll, O(1) on subsequent (edge-triggered)  
\*\* Sockets don't track epoll membership

## Performance Benchmarks

### Test: 1000 sockets, idle

```bash
make iso
qemu-system-x86_64 -cdrom build/AutomationOS.iso -m 512 -serial stdio
# In VM:
/tests/test_epoll_scalability
```

**Expected results**:
- Create 1000 sockets: < 100ms
- Add to epoll: < 50ms
- epoll_wait (100ms timeout): blocks efficiently
- CPU usage: < 1% (kernel not polling)

### Test: 1000 sockets, 100 events/sec

**Expected results**:
- Latency per wakeup: < 1ms
- Throughput: > 100k events/sec
- CPU usage: ~10% (dominated by network processing, not epoll)

## Known Limitations

1. **Socket integration incomplete**
   - `epoll_poll_socket()` is simplified
   - Real implementation needs hooks in TCP/UDP rx path

2. **No oneshot mode (EPOLLONESHOT)**
   - Would require tracking per-watch "armed" state
   - Easy to add if needed

3. **No level-triggered mode**
   - Edge-triggered only
   - Level-triggered would require storing last polled state

4. **Spinlock in wait path**
   - Current implementation uses spinlock + sleep loop
   - Should integrate with wait_queue blocking primitives

5. **No support for regular files**
   - Only works with sockets (fd < 16 || fd >= 0x10000)
   - Regular files always "ready" (no blocking)

## Future Enhancements

### Priority 1: Complete Socket Integration

Add hooks in `kernel/net/socket.c`:

```c
// After TCP receive:
void tcp_input(...) {
    // ... existing code ...
    if (s->rx_used > 0) {
        epoll_notify_socket(sock_index(s), EPOLLIN);
    }
}

// After UDP enqueue:
void udp_input(...) {
    // ... existing code ...
    if (s->dq_count > 0) {
        epoll_notify_socket(sock_index(s), EPOLLIN);
    }
}
```

### Priority 2: Wait Queue Integration

Replace sleep loop with proper blocking:

```c
int64_t sys_epoll_wait(...) {
    while (1) {
        spinlock_acquire(&ep->lock);
        if (ep->ready_count > 0) {
            // ... return events ...
        }
        spinlock_release(&ep->lock);

        // Block properly (requires scheduler integration)
        wq_block_current(&ep->wait_queue);
    }
}
```

### Priority 3: Level-Triggered Mode

Add flag to track mode per watch:

```c
typedef struct epoll_watch {
    // ... existing fields ...
    bool edge_triggered;  // false = level-triggered
} epoll_watch_t;
```

Then in `epoll_add_ready()`:

```c
if (w->edge_triggered) {
    // Only add if state changed
    if ((new_state & w->events) == w->last_state) return;
} else {
    // Always add if state matches
    if ((new_state & w->events) == 0) return;
}
```

### Priority 4: Oneshot Mode

```c
#define EPOLLONESHOT 0x40000000

// In epoll_add_ready():
if (w->events & EPOLLONESHOT) {
    w->events = 0;  // disarm after one event
}
```

## Testing

### Unit Tests

```bash
cd tests/unit
make test_epoll
./test_epoll
```

### Integration Tests

```bash
cd tests/integration
make test_epoll_scalability
./test_epoll_scalability
```

### Stress Tests

```bash
# 10,000 sockets, 1000 events/sec for 60 seconds
./test_epoll_stress --sockets=10000 --events=1000 --duration=60
```

## References

- [Linux epoll(7) man page](https://man7.org/linux/man-pages/man7/epoll.7.html)
- [The Implementation of epoll (LWN.net)](https://lwn.net/Articles/633422/)
- [Scalable I/O Event Notification Mechanisms](https://people.freebsd.org/~jlemon/papers/kqueue.pdf)

## Author

Implementation by AutomationOS kernel team, 2026.

## License

Same as AutomationOS kernel (see LICENSE).
