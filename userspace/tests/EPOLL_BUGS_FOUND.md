# Epoll Implementation Issues Found During Testing

## Issue 1: Inconsistent Spinlock API Usage

**Location**: `kernel/core/syscall/epoll.c`

**Problem**: Mixed use of two different spinlock APIs:
- `spin_lock()` / `spin_unlock()` 
- `spinlock_acquire()` / `spinlock_release()`

**Instances**:
- Line 278: `spinlock_acquire(&ep->lock);`
- Line 349: `spinlock_release(&ep->lock);`
- All other locations use `spin_lock()` / `spin_unlock()`

**Impact**: May compile, but causes confusion and potential bugs if the two APIs have different semantics.

**Fix**: Standardize on one API (likely `spin_lock` / `spin_unlock` as it's used everywhere else).

```c
// Line 278: Change
spinlock_acquire(&ep->lock);
// To:
spin_lock(&ep->lock);

// Line 349: This line is unreachable (after return), but should be deleted or:
// The function structure should be refactored to have a single unlock point
```

**Severity**: Low (code works, but style inconsistency)

---

## Issue 2: Unreachable Code in epoll_ctl

**Location**: `kernel/core/syscall/epoll.c`, lines 349-350

**Problem**: The code:
```c
else if (op == EPOLL_CTL_DEL) {
    // ... delete code ...
    spin_unlock(&ep->lock);
    return 0;
}

spinlock_release(&ep->lock);  // UNREACHABLE
return EINVAL;                 // UNREACHABLE
```

All three branches (ADD, MOD, DEL) return directly after unlocking, so lines 349-350 are never executed.

**Impact**: Dead code, no functional impact.

**Fix**: Remove lines 349-350 or refactor to have single exit point:

```c
int64_t sys_epoll_ctl(...) {
    // ...
    int64_t ret = EINVAL;
    
    spin_lock(&ep->lock);
    
    if (op == EPOLL_CTL_ADD) {
        // ...
        ret = 0;
    } else if (op == EPOLL_CTL_MOD) {
        // ...
        ret = 0;
    } else if (op == EPOLL_CTL_DEL) {
        // ...
        ret = 0;
    }
    
    spin_unlock(&ep->lock);
    return ret;
}
```

**Severity**: Low (cosmetic issue)

---

## Issue 3: No Epoll FD Cleanup Mechanism

**Location**: `kernel/core/syscall/epoll.c`

**Problem**: Epoll instances are never freed. Once created, they persist until kernel shutdown. There is no close() syscall integration.

**Impact**: 
- Resource leak: after creating 64 instances, no more can be created
- Memory leak: ~20KB per instance never released

**Fix**: Integrate with VFS file descriptor table:
1. Make epoll fds part of process fd table
2. Implement close callback to free epoll_instance
3. Mark instance as unused on close

```c
// In file descriptor table:
typedef struct {
    int type;  // FD_TYPE_FILE, FD_TYPE_SOCKET, FD_TYPE_EPOLL
    union {
        file_t* file;
        socket_t* socket;
        epoll_instance_t* epoll;
    } data;
} fd_entry_t;

// In close() syscall:
if (fd_entry->type == FD_TYPE_EPOLL) {
    epoll_close(fd_entry->data.epoll);
}

// New function:
void epoll_close(epoll_instance_t* ep) {
    spin_lock(&ep->lock);
    ep->used = false;
    memset(ep->watches, 0, sizeof(ep->watches));
    spin_unlock(&ep->lock);
}
```

**Severity**: Medium (prevents long-running processes from using epoll repeatedly)

---

## Issue 4: Simplified Socket Polling Always Returns EPOLLIN

**Location**: `kernel/core/syscall/epoll.c`, lines 197-215

**Problem**: The `epoll_poll_socket()` function always returns EPOLLIN:

```c
static uint32_t epoll_poll_socket(int fd) {
    // ...
    // SIMPLIFIED: always report EPOLLIN for now
    state |= EPOLLIN;
    return state;
}
```

**Impact**: 
- epoll_wait() always returns immediately (all watched sockets appear ready)
- Edge-triggered mode doesn't work properly (events always trigger)
- Cannot test real blocking behavior

**Fix**: Integrate with socket layer:

```c
static uint32_t epoll_poll_socket(int fd) {
    socket_t* sock = get_socket(fd);
    if (!sock) return 0;
    
    uint32_t state = 0;
    
    // TCP: readable if rx buffer has data
    if (sock->type == SOCK_STREAM && sock->rx_used > 0) {
        state |= EPOLLIN;
    }
    
    // UDP: readable if datagram queue has packets
    if (sock->type == SOCK_DGRAM && sock->dq_count > 0) {
        state |= EPOLLIN;
    }
    
    // Writable if tx buffer has space
    if (sock->tx_free > 0) {
        state |= EPOLLOUT;
    }
    
    // Error conditions
    if (sock->error) {
        state |= EPOLLERR;
    }
    
    // Connection closed
    if (sock->state == TCP_CLOSED) {
        state |= EPOLLHUP;
    }
    
    return state;
}
```

**Severity**: High (prevents real-world usage until fixed)

---

## Issue 5: epoll_wait Uses Polling Instead of True Blocking

**Location**: `kernel/core/syscall/epoll.c`, lines 421-433

**Problem**: Current implementation uses busy-wait with timer_sleep(1):

```c
while (1) {
    // ... check for events ...
    
    // Sleep for a short interval to poll again (simplified)
    timer_sleep(1);
}
```

**Impact**:
- CPU usage: wakes up every 1ms even if no events
- Latency: up to 1ms delay for event delivery
- Not scalable: 100 processes = 100 wakeups/ms

**Fix**: Use wait queue blocking:

```c
while (1) {
    spin_lock(&ep->lock);
    
    // Check for events
    if (ep->ready_count > 0) {
        // Return events
    }
    
    // No events, check timeout
    if (now >= deadline) {
        spin_unlock(&ep->lock);
        return 0;
    }
    
    // Block on wait queue
    spin_unlock(&ep->lock);
    wq_block_current(&ep->wait_queue, timeout_remaining);
    
    // Woken up by epoll_notify_socket() or timeout
}
```

And in socket receive path:

```c
// kernel/core/net/tcp.c, udp.c
void tcp_receive_packet(...) {
    // ... add packet to rx buffer ...
    
    epoll_notify_socket(sockfd, EPOLLIN);
}
```

**Severity**: Medium (works but inefficient)

---

## Issue 6: No Socket Layer Integration

**Location**: Entire socket subsystem (`kernel/core/net/`)

**Problem**: Socket layer never calls `epoll_notify_socket()`, so epoll never receives real events.

**Impact**: Epoll only works with simulated events from polling loop, not real socket events.

**Fix**: Add epoll_notify_socket() calls to socket layer:

```c
// In TCP receive path (kernel/core/net/tcp.c):
void tcp_handle_ack_segment(...) {
    // ... process ACK, update rx buffer ...
    
    if (sock->rx_used > 0) {
        epoll_notify_socket(sockfd, EPOLLIN);
    }
}

// In UDP receive path (kernel/core/net/udp.c):
void udp_receive_datagram(...) {
    // ... enqueue datagram ...
    
    epoll_notify_socket(sockfd, EPOLLIN);
}

// In socket send path:
int sock_send(...) {
    // ... transmit data ...
    
    if (sock->tx_free > threshold) {
        epoll_notify_socket(sockfd, EPOLLOUT);
    }
}
```

**Severity**: High (required for production use)

---

## Issue 7: Return Code Convention Inconsistency

**Location**: `kernel/core/syscall/epoll.c`

**Problem**: The implementation returns negative errno values (e.g., `return EBADF`), but EBADF is likely defined as a positive value in the kernel.

**Example**:
```c
// Line 276
if (!ep) return EBADF;  // EBADF might be +9, not -9
```

**Expected**: Linux syscalls return negative errno on error:
```c
if (!ep) return -EBADF;  // Return -9
```

**Fix**: Check errno definitions in `kernel/include/errno.h`:
- If EBADF is defined as 9, change to `return -EBADF`
- If EBADF is defined as -9, current code is correct

**Severity**: Medium (may cause userspace to misinterpret errors)

---

## Summary of Issues

| Issue | Severity | Impact | Fix Complexity |
|-------|----------|--------|----------------|
| 1. Inconsistent spinlock API | Low | Style | Easy (find/replace) |
| 2. Unreachable code | Low | None | Easy (delete 2 lines) |
| 3. No epoll fd cleanup | Medium | Resource leak | Medium (VFS integration) |
| 4. Always returns EPOLLIN | High | Can't test real events | Medium (socket integration) |
| 5. Polling instead of blocking | Medium | CPU waste | Medium (scheduler integration) |
| 6. No socket integration | High | Not production-ready | Hard (requires socket layer changes) |
| 7. Return code convention | Medium | Error handling | Easy (add `-` prefix) |

## Recommended Fixes (Priority Order)

1. **Issue 7** (return codes): Quick fix, prevents userspace bugs
2. **Issue 1** (spinlock API): Quick cleanup
3. **Issue 2** (unreachable code): Quick cleanup
4. **Issue 4** (socket polling): Required for real testing
5. **Issue 6** (socket integration): Required for production
6. **Issue 5** (blocking): Performance improvement
7. **Issue 3** (cleanup): Resource management

## Test Results Impact

**Current test results**:
- ✓ API tests pass (epoll_create, epoll_ctl, epoll_wait API surface works)
- ⚠ Socket tests fail (no real event delivery due to Issues 4 & 6)
- ✓ Kernel tests pass (resource limits, error handling work)

**After fixing Issues 4 & 6**:
- ✓ API tests pass
- ✓ Socket tests pass (real event delivery)
- ✓ Kernel tests pass

**After fixing all issues**:
- ✓ Production-ready epoll implementation
- ✓ Resource cleanup (no leaks)
- ✓ Efficient blocking (low CPU usage)
