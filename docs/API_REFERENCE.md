# AutomationOS API Reference

**Version:** 0.1.0  
**Phase:** 1 - Core Foundation (+ networking / WiFi / audio control planes)  
**Last Updated:** 2026-06-21

> The canonical, authoritative source of truth for syscall numbers and ABI
> structs is the kernel header `kernel/include/syscall.h` (numbers) plus the
> UAPI headers `kernel/include/uapi/*.h` (structs). This document is a
> hand-maintained companion -- when in doubt, the headers win. Every number
> below was cross-checked against `kernel/include/syscall.h`.

---

## Table of Contents

1. [Memory Management API](#memory-management-api)
2. [Process Management API](#process-management-api)
3. [System Call API](#system-call-api)
4. [WiFi Control Plane (SYS_WLAN_*)](#wifi-control-plane-sys_wlan_)
5. [Audio Mixer (SYS_AUDIO_*)](#audio-mixer-sys_audio_)
6. [Driver API](#driver-api)
7. [Architecture-Specific API](#architecture-specific-api)
8. [Kernel Library API](#kernel-library-api)
9. [Userspace libc API](#userspace-libc-api)

---

## Memory Management API

### Physical Memory Manager (PMM)

**Header:** `kernel/include/mem.h`

#### `pmm_init()`

Initialize the physical memory manager with the boot memory map.

```c
void pmm_init(memory_map_entry_t* mmap, uint32_t mmap_count);
```

**Parameters:**
- `mmap`: Pointer to array of memory map entries from bootloader
- `mmap_count`: Number of entries in memory map

**Description:**
Initializes the buddy allocator with usable memory regions. Must be called early in kernel initialization.

**Example:**
```c
pmm_init(boot_info->memory_map, boot_info->memory_map_count);
```

---

#### `pmm_alloc_page()`

Allocate a single physical page (4KB).

```c
void* pmm_alloc_page(void);
```

**Returns:**
- Pointer to physical page address
- NULL on allocation failure (triggers kernel panic)

**Description:**
Allocates a 4KB page using the buddy allocator. Pages are allocated from the lowest available order.

**Example:**
```c
void* page = pmm_alloc_page();
if (page == NULL) {
    // Panic: out of memory
}
```

---

#### `pmm_free_page()`

Free a previously allocated physical page.

```c
void pmm_free_page(void* page);
```

**Parameters:**
- `page`: Physical address of page to free (must be page-aligned)

**Description:**
Returns a page to the free list. Adjacent free pages are coalesced using the buddy algorithm.

**Example:**
```c
pmm_free_page(page);
```

---

#### `pmm_get_total_memory()`

Get total physical memory in bytes.

```c
uint64_t pmm_get_total_memory(void);
```

**Returns:** Total memory in bytes

---

#### `pmm_get_used_memory()`

Get currently allocated memory in bytes.

```c
uint64_t pmm_get_used_memory(void);
```

**Returns:** Used memory in bytes

---

#### `pmm_get_free_memory()`

Get available free memory in bytes.

```c
uint64_t pmm_get_free_memory(void);
```

**Returns:** Free memory in bytes

**Example:**
```c
uint64_t free_mb = pmm_get_free_memory() / (1024 * 1024);
kprintf("Free memory: %u MB\n", (uint32_t)free_mb);
```

---

### Virtual Memory Manager (VMM)

**Header:** `kernel/include/mem.h`

#### `vmm_init()`

Initialize the virtual memory manager.

```c
void vmm_init(void);
```

**Description:**
Sets up 4-level paging structures (PML4 → PDPT → PD → PT). Maps kernel in higher half and sets up identity mapping for low memory.

**Example:**
```c
vmm_init();
```

---

#### `vmm_map_page()`

Map a virtual page to a physical page.

```c
void* vmm_map_page(void* virt, void* phys, uint32_t flags);
```

**Parameters:**
- `virt`: Virtual address (must be page-aligned)
- `phys`: Physical address (must be page-aligned)
- `flags`: Page flags (see below)

**Returns:**
- Virtual address on success
- NULL on failure

**Flags:**
- `PAGE_PRESENT` (0x01): Page is present
- `PAGE_WRITE` (0x02): Page is writable
- `PAGE_USER` (0x04): Accessible from user mode

**Example:**
```c
void* virt = (void*)0x400000;
void* phys = pmm_alloc_page();
vmm_map_page(virt, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
```

---

#### `vmm_unmap_page()`

Unmap a virtual page.

```c
void vmm_unmap_page(void* virt);
```

**Parameters:**
- `virt`: Virtual address to unmap (must be page-aligned)

**Description:**
Removes the mapping for a virtual page. Does not free the physical page.

---

#### `vmm_get_physical()`

Get physical address for a virtual address.

```c
void* vmm_get_physical(void* virt);
```

**Parameters:**
- `virt`: Virtual address

**Returns:**
- Physical address
- NULL if not mapped

---

### Kernel Heap

**Header:** `kernel/include/mem.h`

#### `heap_init()`

Initialize the kernel heap.

```c
void heap_init(void);
```

**Description:**
Sets up the slab allocator for dynamic kernel memory allocation.

---

#### `kmalloc()`

Allocate memory from kernel heap.

```c
void* kmalloc(size_t size);
```

**Parameters:**
- `size`: Number of bytes to allocate

**Returns:**
- Pointer to allocated memory
- NULL on failure (triggers kernel panic)

**Example:**
```c
char* buffer = kmalloc(1024);
// Use buffer...
kfree(buffer);
```

---

#### `kfree()`

Free heap-allocated memory.

```c
void kfree(void* ptr);
```

**Parameters:**
- `ptr`: Pointer returned by `kmalloc()` (or NULL)

**Description:**
Returns memory to the heap. Passing NULL is safe (no-op).

---

## Process Management API

### Process Table

**Header:** `kernel/include/sched.h`

#### `process_init()`

Initialize the process subsystem.

```c
void process_init(void);
```

**Description:**
Initializes the process table and allocates the idle process (PID 0).

---

#### `process_create()`

Create a new process.

```c
process_t* process_create(const char* name, void* entry_point);
```

**Parameters:**
- `name`: Process name (max 63 characters)
- `entry_point`: Function to execute

**Returns:**
- Pointer to process control block (PCB)
- NULL on failure

**Description:**
Allocates a new PCB, assigns a PID, sets up kernel and user stacks, and initializes the CPU context.

**Example:**
```c
process_t* init = process_create("init", &init_main);
scheduler_add_process(init);
```

---

#### `process_destroy()`

Destroy a process and free its resources.

```c
void process_destroy(process_t* proc);
```

**Parameters:**
- `proc`: Process to destroy

**Description:**
Frees kernel stack, user stack, page tables, and PCB. Removes from scheduler.

---

#### `process_get_by_pid()`

Look up a process by PID.

```c
process_t* process_get_by_pid(uint32_t pid);
```

**Parameters:**
- `pid`: Process ID

**Returns:**
- Pointer to PCB
- NULL if not found

---

#### `process_get_current()`

Get currently running process.

```c
process_t* process_get_current(void);
```

**Returns:** Current process PCB

---

#### `process_set_current()`

Set the current process (internal use).

```c
void process_set_current(process_t* proc);
```

---

### Scheduler

**Header:** `kernel/include/sched.h`

#### `scheduler_init()`

Initialize the scheduler.

```c
void scheduler_init(void);
```

**Description:**
Sets up the ready queue and configures the timer interrupt for preemptive scheduling.

---

#### `scheduler_add_process()`

Add a process to the ready queue.

```c
void scheduler_add_process(process_t* proc);
```

**Parameters:**
- `proc`: Process to add

**Description:**
Marks the process as READY and adds it to the round-robin queue.

---

#### `scheduler_remove_process()`

Remove a process from the ready queue.

```c
void scheduler_remove_process(process_t* proc);
```

**Parameters:**
- `proc`: Process to remove

---

#### `schedule()`

Scheduler tick function (called from timer interrupt).

```c
void schedule(void);
```

**Description:**
Decrements current process time slice. If expired, picks next process and performs context switch.

**Note:** This is called automatically from the PIT interrupt handler. Do not call directly.

---

#### `scheduler_pick_next()`

Select the next process to run (round-robin).

```c
process_t* scheduler_pick_next(void);
```

**Returns:** Next process to run

---

### Context Switching

**Header:** `kernel/include/sched.h`

#### `context_switch()`

Perform a context switch between processes.

```c
void context_switch(process_t* from, process_t* to);
```

**Parameters:**
- `from`: Current process (context will be saved)
- `to`: Next process (context will be restored)

**Description:**
Low-level assembly function that saves/restores CPU registers and switches page tables (CR3).

**Note:** Internal function - called by scheduler. Do not call directly.

---

## System Call API

### System Call Interface

**Header:** `kernel/include/syscall.h` (numbers 0-200; `MAX_SYSCALLS == 256`)  
**Dispatch table + registration:** `kernel/core/syscall/syscall.c`

The syscall surface is much larger than the handful detailed below; the header
is the authoritative list. Beyond the POSIX-ish core (exit/fork/read/write/
open/close/waitpid/execve/getpid/sleep, 0-9) it spans: IPC (SHM/MSG, 18-25),
signals (`rt_sigaction`/`rt_sigprocmask`/`rt_sigreturn`/`sigpending`, 107-110),
threads (79-81), futex (70), epoll (73-75) and `poll`/`select` (111-112),
sockets (51-58, 76-78) and raw frames / net config / route+ARP table dumps
(59, 68-69, 89-91), block + persistent diskfs I/O (49-50, 94-95), the typed
agent-rail channels (`CHANNEL-0`, 96-106), framebuffer/RTC/entropy/clipboard/
notifications, PCI/battery introspection, the
[WiFi control plane](#wifi-control-plane-sys_wlan_) (113-117, plus `SYS_WLAN_DIAG` at 124) and the
[audio mixer](#audio-mixer-sys_audio_) (118-123). A few numbers are *gated*: e.g.
`SYS_CPU1_OFFLOAD` (83) is only registered under `SMP_FOUNDATION`; on the default
build the slot is empty and the call returns `ENOTSUP`. Unregistered numbers
likewise return `ENOTSUP`.

#### `syscall_init()`

Initialize the system call subsystem.

```c
void syscall_init(void);
```

**Description:**
Sets up the syscall handler table and configures the `syscall` instruction (MSRs).

---

#### `syscall_dispatch()`

Dispatch a system call to its handler.

```c
int64_t syscall_dispatch(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);
```

**Parameters:**
- `syscall_num`: System call number (0-255)
- `arg1` - `arg6`: System call arguments

**Returns:** System call return value (or error code)

**Note:** Internal function - called from `syscall_entry` in assembly.

---

### System Call Handlers

#### `sys_exit()`

Terminate the current process.

```c
int64_t sys_exit(uint64_t status, ...);
```

**Parameters:**
- `status`: Exit code

**Returns:** Does not return

**Syscall Number:** `SYS_EXIT` (0)

---

#### `sys_fork()`

Create a child process.

```c
int64_t sys_fork(uint64_t arg1, ...);
```

**Returns:**
- Child PID in parent process
- 0 in child process
- Negative error code on failure

**Syscall Number:** `SYS_FORK` (1)

---

#### `sys_read()`

Read from a file descriptor.

```c
int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count, ...);
```

**Parameters:**
- `fd`: File descriptor (0 = stdin)
- `buf`: Buffer to read into
- `count`: Maximum bytes to read

**Returns:**
- Number of bytes read
- Negative error code on failure

**Syscall Number:** `SYS_READ` (2)

---

#### `sys_write()`

Write to a file descriptor.

```c
int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count, ...);
```

**Parameters:**
- `fd`: File descriptor (1 = stdout, 2 = stderr)
- `buf`: Buffer to write from
- `count`: Number of bytes to write

**Returns:**
- Number of bytes written
- Negative error code on failure

**Syscall Number:** `SYS_WRITE` (3)

**Example:**
```c
const char* msg = "Hello, world!\n";
sys_write(1, (uint64_t)msg, strlen(msg), 0, 0, 0);
```

---

#### `sys_getpid()`

Get current process ID.

```c
int64_t sys_getpid(void);
```

**Returns:** Current process PID

**Syscall Number:** `SYS_GETPID` (8)

---

### Error Codes

The canonical negative-errno set lives in `kernel/include/errno.h` (included by
`syscall.h`). The illustrative subset:

```c
#define ESUCCESS    0   // Success
#define ENOTSUP    -1   // Not supported
#define EINVAL     -2   // Invalid argument
#define EBADF      -3   // Bad file descriptor
#define ENOMEM     -4   // Out of memory
#define ESRCH      -5   // No such process
```

**Convention:** syscalls return `0` / a positive value on success and a
**negative** errno on failure. `syscall.c` deliberately avoids the positive
`compat/errno.h` values so that, e.g., `-EINVAL` can never be confused with a
22-byte success.

---

## WiFi Control Plane (SYS_WLAN_*)

**Numbers:** `kernel/include/syscall.h` (113-117, plus `SYS_WLAN_DIAG` at 124)  
**ABI structs:** `kernel/include/uapi/wlan.h` (every struct carries an
`*_ABI_SIZE` constant and a `_Static_assert`, so ABI drift is a compile error)  
**Handlers:** `kernel/net/wlansyscall.c`

These are *thin* handlers: each locates the default WiFi interface
(`netif_get_wifi_default()`), validates the user struct, then calls through
`netif_t.wifi` -- the `wifi_ops` swap seam declared in `kernel/include/wifi.h`.
The handlers never touch a driver directly. The active backend behind the seam
is either the simulated one (`kernel/drivers/net/wireless/sim/wifisim.c`, built
with `WIFI_SIM=1`) or the from-scratch Intel iwlwifi DVM driver under
`kernel/drivers/net/wireless/intel/iwlwifi/` (built with `IWLWIFI`, brought up
post-desktop by `sbin/iwlup`, never at boot). When no WiFi interface is
registered (a wired-only or no-NIC build), every handler returns `ENOTSUP`.

| # | Name | Handler | Payload |
|---|------|---------|---------|
| 113 | `SYS_WLAN_SCAN` | `sys_wlan_scan` | out: `uapi_wlan_bss_t[]` |
| 114 | `SYS_WLAN_CONNECT` | `sys_wlan_connect` | in: `uapi_wlan_connect_t*` |
| 115 | `SYS_WLAN_STATUS` | `sys_wlan_status` | out: `uapi_wlan_status_t*` |
| 116 | `SYS_WLAN_DISCONNECT` | `sys_wlan_disconnect` | (none) |
| 117 | `SYS_WLAN_SET_KEY` | `sys_wlan_set_key` | in: `uapi_wlan_setkey_t*` |
| 124 | `SYS_WLAN_DIAG` | `sys_wlan_diag` | out: `uapi_wlan_diag_t*` |

> **Note (historical).** `SYS_WLAN_DIAG` was briefly assigned 118, which collided
> with `SYS_AUDIO_VOLUME`. It was moved to the next free slot, **124**, so the two
> syscalls now have distinct numbers and the audio mixer at 118 is fully reachable.

#### `SYS_WLAN_SCAN` (113)

```c
int64_t sys_wlan_scan(uint64_t out_ptr, uint64_t max_entries, ...);
```

Triggers a passive scan, then marshals up to `min(max_entries, 16)` results into
the caller's `uapi_wlan_bss_t` array. **Returns** the number of BSSes written
(`>= 0`), `EINVAL` for a NULL pointer / zero count, `ENOTSUP` if the backend has
no `scan_results`, or `EFAULT` on a bad user buffer. At most `WLAN_SCAN_CAP` (16)
rows are marshalled per call.

```c
// uapi/wlan.h  (WLAN_BSS_ABI_SIZE == 48)
typedef struct {
    uint8_t  bssid[6];     // AP MAC
    uint8_t  ssid[32];     // NOT NUL-terminated; use ssid_len
    uint8_t  ssid_len;     // 0..32
    uint8_t  security;     // UAPI_WLAN_SEC_OPEN/WPA2/WPA3
    uint16_t channel;
    int16_t  signal;       // dBm (negative)
    uint16_t capability;
    uint8_t  _pad[2];
} uapi_wlan_bss_t;
```

#### `SYS_WLAN_CONNECT` (114)

```c
int64_t sys_wlan_connect(uint64_t req_ptr, ...);
```

Joins an SSID. The kernel clamps `ssid_len` to 32 and NUL-terminates the
passphrase before handing it to the backend. **Returns** `0` on success, `EIO`
if the backend's `connect` fails, `EINVAL`/`EFAULT`/`ENOTSUP` otherwise.

```c
// uapi/wlan.h  (WLAN_CONNECT_ABI_SIZE == 104)
typedef struct {
    uint8_t  ssid[32];
    uint8_t  ssid_len;
    uint8_t  security;     // UAPI_WLAN_SEC_*
    uint8_t  bssid[6];     // optional pin; all-zero = any BSSID
    char     passphrase[64];  // empty for OPEN
} uapi_wlan_connect_t;
```

#### `SYS_WLAN_STATUS` (115)

```c
int64_t sys_wlan_status(uint64_t out_ptr, ...);
```

Fills the current association state and signal. **Returns** `0` on success.

```c
// uapi/wlan.h  (WLAN_STATUS_ABI_SIZE == 44)
typedef struct {
    uint8_t  state;        // UAPI_WLAN_ST_* (DOWN..CONNECTED/FAILED)
    int8_t   rssi;         // dBm
    uint8_t  bssid[6];
    uint8_t  ssid[32];
    uint8_t  ssid_len;
    uint8_t  _pad[3];
} uapi_wlan_status_t;
```

State enum (`UAPI_WLAN_ST_*`): `0 DOWN`, `1 SCANNING`, `2 AUTHENTICATING`,
`3 ASSOCIATING`, `4 ASSOCIATED`, `5 4WAY`, `6 CONNECTED`, `7 FAILED`.

#### `SYS_WLAN_DISCONNECT` (116)

```c
int64_t sys_wlan_disconnect(...);   // no payload
```

**Returns** `0` on success, `EIO`/`ENOTSUP` otherwise.

#### `SYS_WLAN_SET_KEY` (117)

```c
int64_t sys_wlan_set_key(uint64_t req_ptr, ...);
```

The supplicant (`sbin/wpasupp`) installs a pairwise (PTK) or group (GTK) key
after the 4-way / SAE handshake. `key_len > 32` is rejected with `EINVAL`.

```c
// uapi/wlan.h  (WLAN_SETKEY_ABI_SIZE == 44)
typedef struct {
    uint32_t key_idx;
    uint32_t pairwise;     // 1 = pairwise (PTK), 0 = group (GTK)
    uint32_t key_len;      // 16 (CCMP/GCMP-128) or 32
    uint8_t  key[32];
} uapi_wlan_setkey_t;
```

#### `SYS_WLAN_DIAG` (124)

```c
int64_t sys_wlan_diag(uint64_t out_ptr, ...);
```

Copies the radio bring-up diagnostics snapshot (maintained by the active backend
and surfaced from `kernel/net/wifidiag.c`) to userspace, so the Network Manager
(`userspace/apps/netman`) can show *where* iwlwifi bring-up stopped on real
hardware -- no serial cable needed. Rendered as the live "Radio:" line in netman.

```c
// uapi/wlan.h  (WLAN_DIAG_ABI_SIZE == 120)
typedef struct {
    uint8_t  present;      // 1 if a wifi backend is active
    uint8_t  family;       // UAPI_WLAN_FAM_* (UNKNOWN/1000/5000/6000/6000G2)
    uint8_t  rf_kill;      // 1 if HW RF-kill asserted (CSR_GP_CNTRL bit 27)
    uint8_t  alive;        // 1 if firmware reached runtime ALIVE
    uint8_t  nvm_ok;       // 1 if MAC/channels read OK
    uint8_t  stage;        // UAPI_WLAN_STAGE_* (bring-up stage reached)
    uint8_t  mac[6];       // radio MAC (once NVM read)
    uint16_t n_channels;   // channels enumerated
    int16_t  last_scan_bss;// last scan result count (-1 = none/error)
    char     card[40];     // card friendly name
    char     msg[64];      // last status / failure-step line
} uapi_wlan_diag_t;
```

Bring-up stage enum (`UAPI_WLAN_STAGE_*`): `0 NONE`, `1 NOCARD`, `2 DETECTED`
(card found + BAR0 mapped), `3 TRANS_OK` (APM + DMA rings), `4 ALIVE` (firmware
runtime), `5 NVM_OK` (MAC + channels), `6 REGISTERED` (`wlan0` live behind the
seam), `7 SCANNED`, `8 FAILED` (see `msg`).

---

## Audio Mixer (SYS_AUDIO_*)

**Numbers:** `kernel/include/syscall.h` (118-123)  
**Handlers:** `kernel/core/syscall/syscall.c`  
**Driver:** Intel HDA (`kernel/drivers/hda_stream.c`, `kernel/drivers/audio/`),
codec comm via the Immediate Command Interface; gated on at build time with
`HDA_ENABLE=1`.

A thin, guarded gateway from ring 3 to the HDA driver's volume / mute / tone
controls. The driver is fire-and-forget (it does not retain the last
volume/mute), so the syscall layer mirrors those in two `static` globals for
`SYS_AUDIO_STATUS`. Handlers degrade safely: with no HDA controller/codec they
return `ENODEV` cleanly rather than hanging, so they are safe no-ops on
audioless boots. The consumer is the Sound Manager app
(`userspace/apps/soundman`).

| # | Name | Handler | Notes |
|---|------|---------|-------|
| 118 | `SYS_AUDIO_VOLUME` | `sys_audio_volume` | clamps to 0..100 -> `hda_set_volume` |
| 119 | `SYS_AUDIO_MUTE` | `sys_audio_mute` | arg1 = 0\|1 |
| 120 | `SYS_AUDIO_OUTPUTS` | (reserved) | unregistered -> `ENOTSUP` |
| 121 | `SYS_AUDIO_SELECT` | (reserved) | unregistered -> `ENOTSUP` |
| 122 | `SYS_AUDIO_TEST` | `sys_audio_test` | arg1 = freq Hz, arg2 = ms (cap 2000) |
| 123 | `SYS_AUDIO_STATUS` | `sys_audio_status` | out: `audio_status_t*` |

> **SYS_AUDIO_VOLUME (118) is fully reachable.** `syscall.h` gives the audio mixer
> the contiguous block 118-123, with no overlap. (`SYS_WLAN_DIAG` was briefly
> assigned 118 too; it has since been moved to **124** to free the audio slot, so
> there is no longer any collision.) A `syscall(118, volume)` from `soundman` lands
> in `sys_audio_volume` as intended.

#### `SYS_AUDIO_VOLUME` (118)

```c
int64_t sys_audio_volume(uint64_t vol /*0..100*/, ...);
```

Clamps `vol` to 0..100 and calls `hda_set_volume` on codec 0. **Returns** the
driver result (`>= 0` ok), `ENODEV` if no controller/codec.

#### `SYS_AUDIO_MUTE` (119)

```c
int64_t sys_audio_mute(uint64_t mute /*0|1*/, ...);
```

Calls `hda_set_mute` on codec 0. **Returns** `>= 0` ok, `ENODEV` if absent.

#### `SYS_AUDIO_TEST` (122)

```c
int64_t sys_audio_test(uint64_t freq_hz, uint64_t ms, ...);
```

Plays a test tone via `audio_play_tone`. Blocks for the (capped at 2000 ms)
duration, same model as `SYS_BEEP` (45).

#### `SYS_AUDIO_STATUS` (123)

```c
int64_t sys_audio_status(uint64_t user_ptr, ...);
```

Fills an `audio_status_t` via `copy_to_user`. **Returns** `0` on success,
`EINVAL` for a NULL pointer, `EFAULT` on a bad buffer.

```c
// syscall.h  (8 bytes, naturally aligned)
typedef struct {
    uint8_t  present;      // 1 = an HDA controller + codec is up
    uint8_t  volume;       // last volume set via SYS_AUDIO_VOLUME (0..100)
    uint8_t  muted;        // 1 = muted (last SYS_AUDIO_MUTE)
    uint8_t  _pad;
    uint32_t codec_vendor; // codec vendor/device id (HDA GET_PARAMETER VENDOR_ID)
} audio_status_t;
```

> Also relevant: `SYS_BEEP` (45) plays a tone via the HDA driver
> (`audio_beep` / `audio_play_tone`), independent of the mixer surface above.

---

## Driver API

### Serial Driver

**Header:** `kernel/include/drivers.h`

#### `serial_init()`

Initialize the serial console (COM1).

```c
void serial_init(void);
```

**Configuration:**
- Port: COM1 (0x3F8)
- Baud rate: 38400
- Data bits: 8
- Parity: None
- Stop bits: 1

---

#### `serial_putchar()`

Write a character to the serial port.

```c
void serial_putchar(char c);
```

**Parameters:**
- `c`: Character to write

---

#### `serial_write()`

Write a string to the serial port.

```c
void serial_write(const char* str, size_t len);
```

**Parameters:**
- `str`: String to write
- `len`: Length of string

---

### PS/2 Keyboard Driver

**Header:** `kernel/include/drivers.h`

#### `ps2_init()`

Initialize the PS/2 keyboard controller.

```c
void ps2_init(void);
```

---

#### `ps2_getchar()`

Read a character from the keyboard (blocking).

```c
char ps2_getchar(void);
```

**Returns:** ASCII character

**Description:**
Blocks until a key is pressed. Handles scancode translation to ASCII.

---

### Framebuffer Driver

**Header:** `kernel/include/drivers.h`

#### `framebuffer_init()`

Initialize the framebuffer.

```c
void framebuffer_init(void* fb_addr, uint32_t width, uint32_t height, uint32_t pitch);
```

**Parameters:**
- `fb_addr`: Physical address of framebuffer
- `width`: Width in pixels
- `height`: Height in pixels
- `pitch`: Bytes per scanline

---

#### `framebuffer_clear()`

Clear the screen to a solid color.

```c
void framebuffer_clear(uint32_t color);
```

**Parameters:**
- `color`: RGB color (0xRRGGBB)

---

#### `framebuffer_putchar()`

Draw a character at (x, y).

```c
void framebuffer_putchar(char c, uint32_t x, uint32_t y, uint32_t color);
```

**Parameters:**
- `c`: Character to draw
- `x`: X coordinate (character cells)
- `y`: Y coordinate (character cells)
- `color`: RGB color

---

### Timer Driver (PIT)

**Header:** `kernel/include/drivers.h`

#### `pit_init()`

Initialize the Programmable Interval Timer.

```c
void pit_init(uint32_t frequency);
```

**Parameters:**
- `frequency`: Timer frequency in Hz (e.g., 100 for 10ms ticks)

---

#### `timer_get_ticks()`

Get the number of timer ticks since boot.

```c
uint64_t timer_get_ticks(void);
```

**Returns:** Tick count

---

#### `timer_get_frequency()`

Get the configured timer frequency.

```c
uint32_t timer_get_frequency(void);
```

**Returns:** Frequency in Hz

---

#### `timer_sleep()`

Sleep for a specified number of milliseconds.

```c
void timer_sleep(uint32_t ms);
```

**Parameters:**
- `ms`: Milliseconds to sleep

**Description:**
Busy-wait loop. Not suitable for long delays.

---

## Architecture-Specific API

### x86_64 Functions

**Header:** `kernel/include/x86_64.h`

#### `gdt_init()`

Initialize the Global Descriptor Table.

```c
void gdt_init(void);
```

---

#### `idt_init()`

Initialize the Interrupt Descriptor Table.

```c
void idt_init(void);
```

---

#### Port I/O

Read/write to I/O ports.

```c
uint8_t inb(uint16_t port);
void outb(uint16_t port, uint8_t value);
uint16_t inw(uint16_t port);
void outw(uint16_t port, uint16_t value);
uint32_t inl(uint16_t port);
void outl(uint16_t port, uint32_t value);
```

**Example:**
```c
outb(0x3F8, 'A');  // Write 'A' to COM1
```

---

#### CPU Control

```c
void cli(void);     // Disable interrupts
void sti(void);     // Enable interrupts
void hlt(void);     // Halt CPU until interrupt
```

---

#### Control Registers

```c
uint64_t read_cr0(void);
void write_cr0(uint64_t value);
uint64_t read_cr2(void);  // Page fault address
uint64_t read_cr3(void);  // Page directory
void write_cr3(uint64_t value);
uint64_t read_cr4(void);
void write_cr4(uint64_t value);
```

---

## Kernel Library API

### String Functions

**Header:** `kernel/lib/string.c`

```c
void* memset(void* s, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
int memcmp(const void* s1, const void* s2, size_t n);
size_t strlen(const char* s);
char* strcpy(char* dest, const char* src);
int strcmp(const char* s1, const char* s2);
```

---

### Kernel Printf

**Header:** `kernel/include/kernel.h`

#### `kprintf()`

Formatted kernel output.

```c
int kprintf(const char* format, ...);
```

**Supported Format Specifiers:**
- `%s` - String
- `%c` - Character
- `%d`, `%i` - Signed integer
- `%u` - Unsigned integer
- `%x` - Hexadecimal (lowercase)
- `%X` - Hexadecimal (uppercase)
- `%p` - Pointer

**Example:**
```c
kprintf("[KERNEL] Initialized %d subsystems\n", count);
kprintf("[MEM] Free memory: %p\n", free_mem);
```

---

### Panic and Assertions

**Header:** `kernel/include/kernel.h`

#### `kernel_panic()`

Trigger a kernel panic (unrecoverable error).

```c
void kernel_panic(const char* message) __attribute__((noreturn));
```

**Parameters:**
- `message`: Panic message

**Example:**
```c
if (critical_failure) {
    kernel_panic("Critical subsystem failure");
}
```

---

#### `ASSERT()`

Runtime assertion macro.

```c
#define ASSERT(cond) do { \
    if (!(cond)) kernel_panic("Assertion failed: " #cond); \
} while(0)
```

**Example:**
```c
ASSERT(ptr != NULL);
ASSERT(size > 0 && size < MAX_SIZE);
```

---

## Userspace libc API

### System Call Wrappers

**Header:** `userspace/libc/syscall.h`

```c
int exit(int status);
pid_t fork(void);
ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
pid_t getpid(void);
void sleep(unsigned int ms);
```

**Example:**
```c
#include <syscall.h>

int main(void) {
    write(1, "Hello, world!\n", 14);
    return 0;
}
```

---

### Standard I/O

**Header:** `userspace/libc/stdio.h`

```c
int printf(const char* format, ...);
int putchar(int c);
int puts(const char* s);
char* gets(char* s);
```

---

### String Functions

**Header:** `userspace/libc/string.h`

```c
size_t strlen(const char* s);
char* strcpy(char* dest, const char* src);
int strcmp(const char* s1, const char* s2);
void* memset(void* s, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
```

---

## Usage Examples

### Example 1: Allocating and Mapping Memory

```c
// Allocate a physical page
void* phys_page = pmm_alloc_page();

// Map it to user space
void* virt_addr = (void*)0x400000;
vmm_map_page(virt_addr, phys_page, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

// Use the memory
memset(virt_addr, 0, PAGE_SIZE);

// Clean up
vmm_unmap_page(virt_addr);
pmm_free_page(phys_page);
```

---

### Example 2: Creating a Process

```c
void init_main(void) {
    printf("Init process started\n");
    
    // Fork and exec shell
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        execve("/bin/shell", NULL, NULL);
    } else {
        // Parent process
        waitpid(pid, NULL, 0);
    }
}

// In kernel
process_t* init = process_create("init", &init_main);
scheduler_add_process(init);
```

---

### Example 3: Writing a Simple Driver

```c
#include "drivers.h"

static uint16_t io_base = 0x2F8;  // COM2

void com2_init(void) {
    outb(io_base + 1, 0x00);  // Disable interrupts
    outb(io_base + 3, 0x80);  // Enable DLAB
    outb(io_base + 0, 0x03);  // Set divisor
    outb(io_base + 1, 0x00);
    outb(io_base + 3, 0x03);  // 8n1
    outb(io_base + 2, 0xC7);  // Enable FIFO
    kprintf("[COM2] Initialized\n");
}

void com2_write(char c) {
    while ((inb(io_base + 5) & 0x20) == 0);
    outb(io_base, c);
}
```

---

## API Conventions

### Naming

- **Kernel functions:** `subsystem_action()` (e.g., `pmm_alloc_page`)
- **System calls:** `sys_name()` (e.g., `sys_write`)
- **Driver functions:** `driver_action()` (e.g., `serial_init`)
- **Internal helpers:** `_function()` or `__function()` (leading underscore)

### Return Values

- **Success:** 0 or positive value (e.g., bytes written, PID)
- **Error:** Negative error code (e.g., `EINVAL`, `ENOMEM`)
- **Pointers:** NULL on failure, valid pointer on success

### Error Handling

```c
// Kernel code
void* ptr = kmalloc(size);
if (ptr == NULL) {
    return ENOMEM;  // Propagate error
}

// Or panic for critical errors
ASSERT(ptr != NULL);
```

---

## Thread Safety & Concurrency Model

The **default** build is cooperative + single-core, so most kernel APIs run
without contention. Two concurrency tiers exist as **gated** builds, validated
separately:

- **`PREEMPT=1`** -- preemptive scheduling (timer-driven preemption).
- **`SMP=1`** -- multi-core, with per-CPU data, spinlocks/run-queue locks, and
  IPI-based TLB shootdown on the validated paths.

Under the default build:
- **Safe:** PMM allocation (protected by implicit spinlocks)
- **Single-threaded by construction:** process table, scheduler queue, driver
  globals (no concurrent kernel-mode execution without `PREEMPT`/`SMP`)

When building `SMP=1`/`PREEMPT=1`, treat the process table, scheduler queue, and
driver globals as shared state guarded by their respective locks; see the SMP
hardening notes in `docs/SMP_HARDENING.md`.

---

## Performance Notes

- **Fast paths:** `pmm_alloc_page()`, `vmm_map_page()`, system calls
- **Slow paths:** `process_create()` (allocates stacks), `context_switch()` (TLB flush)
- **Avoid:** Frequent allocation/deallocation, excessive system calls in tight loops

---

**End of API Reference**
