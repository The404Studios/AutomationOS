# CHANNEL-0 Design — A Capability-Backed Shared-Ring Channel Primitive

**Status:** approved (2026-06-08).
**Branch:** `brick/channel-0` (off `t410-recovery`).
**One-liner:** Replace AutomationOS's ad-hoc StdIO (fd1/2 hardcoded to serial) with **one** primitive — a capability handle to a shared ring buffer — that becomes the single rail for console StdIO, GUI/service IPC, AI tool calls, async batched I/O, and (later) networking.

> **Not** a Unix `/dev/tty` clone. **Not** syscall-per-byte StdIO. **Not** AI scraping terminal text.
> The kernel provides handles + rings + wake/completion + permission bits. Userspace provides terminal
> behavior, shell behavior, the agent runtime, network/file services, and GUI policy.

---

## 0. Why (the current reality this replaces)

- `sys_write(fd1/fd2)` is hardcoded to `serial_write` + a poke at the legacy VGA text buffer `0xb8000` (`kernel/core/syscall/handlers.c:731-744`). There is **no per-process fd table**, no `/dev/tty`, no controlling terminal — just a debug console.
- On the graphical desktop the VGA text buffer is off-screen (framebuffer is in graphics mode), so external programs' stdout is **serial-only and invisible** — the root cause of "`sed`/`cc`/`make` run but show nothing."
- GUI input is a **compositor event stream** (`WL_EVENT_KEY` over the wl protocol), not a file.
- The PTY ring buffers (`kernel/drivers/pty/pty.c`) exist but nobody holds them; `/dev/ptmx` is a `TODO` (`kernel.c:1128`).

We are compositor-first → philosophically closer to Windows (API/GUI) than Unix (everything-is-a-tty-file). So we build **our own** model.

## 1. The model (kernel-dumb, userspace-policy)

| Kernel provides | Userspace provides |
|---|---|
| handles (capabilities) | terminal behavior (echo, VT parse, Ctrl-C→kill) |
| shared ring buffers | shell behavior |
| wake / signal / completion | agent runtime (typed tool calls) |
| permission bits (rights) | network service, file service, GUI policy |

The "terminal device" is **not a kernel object** — it is a userspace **holder** of a channel master. The terminal grid is one holder; the **AI agent is another**. Same primitive, different policy.

## 2. The primitive — API (userspace view)

```c
handle_t ch_create(uint32_t flags, uint32_t capacity);          /* -> handle or <0 */
int      ch_write (handle_t ch, const void* buf, size_t len);   /* bytes written / -errno */
int      ch_read  (handle_t ch, void* buf, size_t len);         /* bytes read / 0=empty / -errno */
int      ch_wait  (handle_t ch, uint32_t events, uint64_t timeout_ms); /* bounded */
int      ch_close (handle_t ch);
/* capability passing (later): */
handle_t ch_dup     (handle_t ch, uint32_t new_rights);          /* rights <= current */
int      ch_transfer(handle_t ch, int pid, uint32_t rights);     /* hand a handle to another proc */
```

`flags`: `CH_BYTE` (stdout/stderr/stdin, terminal text) vs `CH_MSG` (typed packets). `CH_NONBLOCK`.
`events` for `ch_wait`: `CH_READABLE | CH_WRITABLE | CH_CLOSED`.

## 3. Internals (kernel)

A **channel object** (allocated from the kernel heap, refcounted):

```
channel_t {
    ring  to_master;     /* producer: child/slave  -> consumer: holder/master */
    ring  to_slave;      /* producer: holder/master -> consumer: child/slave   */
    u32   flags;         /* CH_BYTE | CH_MSG | ...                              */
    u16   event_flags;   /* readable/writable/closed, per end                   */
    waitq waiters;       /* cooperative wakeup list                             */
    u32   refcount;
}
ring { u8* buf; u32 cap; u32 head; u32 tail; }  /* power-of-2 cap, SPSC, mask indices */
```

Two SPSC rings (one per direction) so master and slave never share head/tail — lock-free on a single core, and correct if SMP ever lands. The existing `pty.c` ring code can seed this (then `pty.c`/`pty_impl.c` retire).

**Handle table:** per-process, in `process_t` — a small fixed array `handle_t[CH_MAX_HANDLES]` mapping a handle (index) → `{channel_t*, rights}`. `handle_t` is process-local (like an fd); passing a channel to another process goes through `ch_transfer` (kernel installs it in the target's table), never by sharing the raw integer.

**Rights** (capability bits, checked on every op): `CH_R_READ`, `CH_R_WRITE`, `CH_R_DUP`, `CH_R_TRANSFER`, `CH_R_SIGNAL`, `CH_R_ADMIN`. `ch_dup` can only **narrow** rights. This is the "AI OS" safety lever: the agent receives only the handles + rights it is allowed to use.

**Cooperative-safe (frozen-tick law):** rings are bounded; `ch_wait` is a **bounded** poll-with-yield (timeout-capped), never an unbounded block — a stuck peer times out, never hangs the single core. `ch_read`/`ch_write` are non-blocking (return short / `0` on empty / `EAGAIN` when full).

## 4. Two channel modes — split text from typed

- **Byte channel** (`CH_BYTE`): stdout/stderr/stdin, terminal text, shell output. For **humans**. Raw bytes through the rings; the holder (terminal) renders them through its VT/ANSI parser.
- **Message channel** (`CH_MSG`): framed typed packets. For **agents and services**.

```c
typedef struct {
    uint16_t type;        /* TOOL_RUN, TOOL_RESULT, FILE_EVENT, NET_EVENT, ERROR ... */
    uint16_t flags;
    uint32_t len;         /* payload length */
    uint64_t request_id;  /* correlate request/response */
    uint8_t  payload[];
} msg_packet_t;
```

The agent never scrapes the terminal. It sends `TOOL_RUN { path="/bin/cc", args=["main.c"] }` and receives `TOOL_RESULT { exit_code=0, stdout_handle=…, stderr_handle=… }` — structured, with explicit error semantics.

## 5. The clean OS model this yields

- **GUI app:** wl window / events / pixels; optional service channels. No StdIO.
- **Console app:** `fd0/fd1/fd2` are channel handles (default-unbound → serial debug console, unchanged).
- **Terminal:** userspace holder of a channel master; renders the byte stream to the grid; sends keyboard bytes/events back.
- **AI agent:** userspace holder of a message channel; invokes tools via typed calls; gets structured results.
- **Network stack (later):** exposes NIC RX/TX as handles/channels (poll-mode descriptor rings, DPDK-*shape* not DPDK-*stack*).

One architecture, not four bolted-on subsystems.

## 6. Scope discipline — take the idea, not the complexity

Borrow from `io_uring` (shared submission/completion rings, batching, completion queue) but **deliberately not** its problem surface: `io_uring` was a large share of Linux kernel exploit submissions in 2022 and has been restricted on Android/ChromeOS. So:

**Take:** shared rings · batched ops · a completion queue · handles/capabilities · bounded waits.
**Skip:** a huge opcode surface · kernel worker threads · deep file/socket integration · registered-buffer machinery · any unbounded magic. **Small is better.**

## 7. Build order

| P | Scope | Visible? |
|---|---|---|
| **P0** | handle table in `process_t` + rights checks | infra |
| **P1** | byte-channel ring + `ch_create/read/write/wait/close` syscalls (+ a boot self-test) | infra |
| **P2** | `sys_spawn` binds child `fd0/fd1/fd2` to channel handles (uses the free spawn args) | infra |
| **P3** | terminal owns the channel master, renders stdout to the grid | **yes** |
| **P4** | shell/external programs' output appears **in the terminal window** | **yes** |
| **P5** | message-channel packets (`msg_packet_t` framing on `CH_MSG`) | infra |
| **P6** | agent-runtime typed tool calls (`AGENT-RPC-0`) | agent |
| **P7** | async batch channel (submission/completion) | perf |
| **P8** | network RX/TX exposed as channels | later |

**The immediate win is P1–P4:** `sed`/`cc`/`make` run *inside* the terminal and their output appears in the window — fixing today's "externals vanish to serial."

## 8. Safety / no-regression

- **Additive:** the `ch_*` syscalls are new; nothing changes until a process opts in. With no channel bound, `fd1/2` still go to serial — **default behavior byte-for-byte unchanged**.
- All waits bounded (cooperative single-core); rings bounded; rights checked on every op.
- Retire the dead heavyweight attempt (`pty_impl.c`) once the channel rings land.

## 9. Brick split

- **CHANNEL-0** = the kernel primitive (this spec): handles + rings + wake + rights.
- **TERMINAL-0** = first user: bind `fd0/1/2` to channels; render; the terminal quick-wins.
- **AGENT-RPC-0** = the typed tool layer (later): `msg_packet_t`, `TOOL_RUN`/`TOOL_RESULT`, the agent runtime.

Not Unix tty. Not a Windows console clone. Not terminal scraping. **Capability handles + shared rings + typed messages — the rail everything else rides on.**
