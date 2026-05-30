# Service Starter Template

A minimal background daemon template for AutomationOS.
Clone `service.c`, search for every `TODO`, and replace with your logic.

## What a service is (vs a GUI app)

| GUI app | Background service |
|---|---|
| Acquires the framebuffer via `SYS_FB_ACQUIRE` | No framebuffer; no window |
| Driven by input events (`SYS_READ_EVENT`) | Driven by a timer or IPC messages |
| Exits when the user closes the window | Runs until the system shuts down |
| Launched by the app launcher | Spawned by `init` or `servicemanager` at boot |

Services are normal ring-3 ELFs; the only difference is how they are
started and what syscalls they use.

## What this service does

1. Calls `service_init()` — open devices, channels, or files once.
2. Loops `MAX_BEATS` times (default 20), waking every `HEARTBEAT_MS` ms (default 500 ms).
3. Each wake: calls `service_tick()` (your work goes here), writes a log
   line to `/var/log/mysvc.log` via `SYS_OPEN` + `SYS_WRITE`, then
   yields the CPU with `SYS_YIELD`.
4. Calls `service_shutdown()` and exits cleanly with code 0.

## Syscalls used

| Syscall | Number | Purpose |
|---|---|---|
| `SYS_EXIT` | 0 | Clean shutdown |
| `SYS_WRITE` | 3 | Write log line / stdout |
| `SYS_OPEN` | 4 | Open VFS log file |
| `SYS_CLOSE` | 5 | Close log file on exit |
| `SYS_YIELD` | 15 | Cooperatively yield CPU |
| `SYS_GET_TICKS_MS` | 40 | Monotonic ms clock for sleep timing |

## Building (on-device IDE)

1. Open `service.c` in the IDE editor.
2. Press **B** (Build).  The on-device `cc` compiles it to a static ELF64.
3. Press **R** (Run) to test it immediately; output appears in the terminal panel.

The on-device `cc` supports `sys_write(fd, buf, len)` and `sys_exit(code)`
as builtins.  The `aos_syscall3` helper uses `asm volatile("syscall")` which
the IDE's compiler passes through to the assembler unchanged.

## Building with gcc (WSL / cross-toolchain)

```sh
x86_64-elf-gcc \
  -ffreestanding -nostdlib -nostdinc \
  -fno-stack-protector -static -O2 \
  -o mysvc service.c
```

Verify no `fs:0x28` stack-canary references:

```sh
objdump -d mysvc | grep -c 'fs:0x28'
# must print 0
```

## Installing your service

1. Copy the ELF to `/sbin/mysvc` on the disk image.
2. Add a spawn entry in `userspace/init/init.c` (or the service manifest
   read by `servicemanager`) so it is launched at boot:

```c
// In init.c boot sequence:
int svc_pid = spawn("/sbin/mysvc", NULL);
```

3. Rebuild the OS image and reboot.  Your service will appear in the
   process list (`ps` or the system monitor).

## Customisation checklist

- [ ] Rename `SERVICE_NAME`, `LOG_PATH`, `HEARTBEAT_MS`, `MAX_BEATS`
- [ ] Fill in `service_init()` — open your device/file/channel
- [ ] Fill in `service_tick()` — your periodic work
- [ ] Fill in `service_shutdown()` — close handles, flush data
- [ ] Set `MAX_BEATS` to a very large number (or use a flag variable)
      so the service runs indefinitely in production
