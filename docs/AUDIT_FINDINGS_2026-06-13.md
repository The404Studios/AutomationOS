# AutomationOS — Audit Findings (os-quality-sweep, 2026-06-13)

Exhaustive 15-subsystem audit, adversarially verified. **98 confirmed findings** (2 P0, 28 P1, 45 P2, 23 P3). **6 fixed this session** (marked ✅); the rest are a prioritized backlog. 3 subsystems (toolchain-cc, ipc-channels, web-security) were rate-limited mid-run and not fully audited — re-run recommended.

**Fixed 2026-06-13: 7** — see ✅ entries (all in P0/P1).

**Fixed 2026-06-14 — kernel safety batch (7 more P1s; 5 commits `231189e..547346c` on
brick/smp-thread-inherit-0, UNPUSHED).** These were ADVERSARIALLY RE-VERIFIED by a 10-agent sweep
first — and that paid off: ~several original findings were impact-inflated or mischaracterized (libc
`exit()` "stdout loss" was FALSE — printf flushes via an fd-sink; `sys_map_file` "kernel-heap
disclosure" was FALSE — it's actually an unprivileged ring-3→ring-0 panic-DoS; the `input.c`
ring-race was a FALSE-POSITIVE — already `volatile`). All 7 below are **FIXED + regression-smoked**
(kernel builds/boots; SIGFULL 8/8 + POLLSELECT level&edge 6/6 still PASS — no regression) but are
**NOT exhaustively stress-proven**: there are no dedicated tests yet for **epoll exhaustion/reclaim,
fork VMA inheritance under real child memory access, sys_map_file misalignment/DoS resistance,
copy_user_string live-CR3 behavior, or HDA boot timing across machines**.
- `fix(epoll)` `231189e` — epoll instance leak (close/exit reclaim, mirrors sock_cleanup_process) +
  removal of the dead event-ring/notify scaffolding (−~130 KB static).
- `fix(mem)` `36c38e7` — copy_user_string walks the live CR3 (was stale active_pml4) + sys_map_file
  rejects non-page-aligned inode->data with EINVAL (stops the ASSERT_ALWAYS panic).
- `fix(sched)` `d00eadf` — fork() deep-copies the parent VMA list (child no longer mis-killed on a
  lazy demand fault).
- `fix(input)` `17700f3` — poll/select/epoll stdin readiness via non-consuming ps2_input_pending().
- `fix(drivers)` `547346c` — hda_msleep gates the no-process/IF=0 boot path onto timer_sleep
  (was a multi-minute serial-logging spin when an HDA controller is present).
DEFERRED from this batch (verified real but not landed): compositor F1/F2/F3 → T410 session (visual +
perf-vs-correctness conflict); FAT32 OOB+detect / NVMe PRP → unreachable (mount stubbed / nvme_init
never called); NIC-IRQ RX → arch-blocked (needs a softirq layer); libc signal()/sigset/lseek/exit →
latent or impact-FP; fork fd-table → low-impact + fix-sensitive.


## P0 (2)

### [✅ FIXED] Signal handler RIP is unvalidated -> SYSRET non-canonical #GP in ring 0 (privilege-escalation primitive on Intel/T410)
- **signals** · `kernel/core/signal/kill.c:380, 396-397, 438` · conf=0.85
- sys_rt_sigaction (kill.c:396-397) stores an arbitrary user-supplied `handler` and `restorer` with no range/canonical check. deliver_pending_signals (kill.c:380) then sets `f->rip = h`, and sys_rt_sigreturn (kill.c:438) sets `f->rip = uc.rip` from a ucontext read off the user stack (the SIG_UC_MAGIC check does not constrain rip). The only return-to-user path for a delivered handler is syscall.asm's `pop rcx (=f->rip); pop rsp (=user RSP); o64 sysret` (syscall.asm:90-95 / 202-207). On Intel CPUs `SYSRET` with a non-canonical value in RCX raises #GP that is delivered in RING 0, at which point RSP
- **fix:** Validate handler/restorer/uc.rip as canonical user addresses before they can reach sysret. In sys_rt_sigaction: reject handler (when !=0 && !=1) and restorer unless < 0x0000800000000000ULL (the user/kernel split already used in exec.c:375). In sys_rt_sigreturn and deliver_pending_signals, likewise verify the rip about to be installed is < 0x0000800000000000ULL (and ideally also that uc.rsp is a us

### [✅ FIXED] sys_rt_sigreturn restores user-controlled RFLAGS without masking IOPL -> ring3 gains direct port I/O (privilege escalation)
- **syscall-boundary** · `kernel/core/signal/kill.c:437-441` · conf=0.88
- sys_rt_sigreturn copies a sig_ucontext_t straight off the user stack (copy_from_user at line 431) and then sets the saved-frame RFLAGS from the fully user-controlled uc.rflags: `f->rflags = (uc.rflags | 0x202ULL)`. The only validation is the SIG_UC_MAGIC check, but the magic (0x5347524554554B) is a known constant any ring-3 program can write. f->rflags is the gpframe field that syscall.asm pops into R11 on return (`pop r11; or r11, 0x200; ... o64 sysret`). In 64-bit SYSRET, RFLAGS is loaded from R11 and the IOPL bits (12-13) are NOT masked by SYSRET nor by the asm (which only ORs in IF=0x200).
- **fix:** Sanitize uc.rflags exactly like sys_fork/enter_usermode before storing it: `f->rflags = (uc.rflags & ~0x100ULL /*TF*/ & ~0x3000ULL /*IOPL*/ & ~0x4000ULL /*NT*/) | 0x202ULL; f->rflags &= ~0x400ULL /*DF*/;`. Keep only the standard user-settable arithmetic/status flags and force IF=1, IOPL=0. Apply the same IOPL clear in deliver_pending_signals line 384 for defense in depth.


## P1 (28)

### [☐ deferred] Two divergent app lists in build_all.sh (canary-check vs copy-to-initrd) with no single source of truth -- already drifted (derby et al. ship unchecked)
- **boot-build** · `scripts/build_all.sh:581,600,650,658,668-677,683,689,697,703` · conf=0.97
- The fs:0x28 canary loop (line 581 `for e in comp init ... gametest`) and the set of apps actually copied into the initrd (the line-600 `for e in ...` loop plus the dozens of direct `cp /tmp/X.elf` lines and the line 658/685/689/697 loops) are maintained as TWO independent, hand-written name lists. A diff proves they have already diverged: derby, nicup, cpu1hello, bklstorm, batchdemo, threadprobe, gunzip, imageviewer, cwatchdog are copied into the image but are absent from the canary list. derby is not inert -- compositor_m8.c:913 registers a dock icon `{ "Db", "sbin/derby", ... }`, so a real u
- **fix:** Make the staged set the single source of truth: build one APPS_SBIN/APPS_BIN variable (name->dest) once, drive BOTH the copy and the canary loop from it. Minimal version: after staging, derive the canary list from the actual files (`for f in /tmp/ird/sbin/* /tmp/ird/bin/*; do objdump -d "$f" | grep -c fs:0x28; done`) so no binary can ship unchecked, and add a guard that every built /tmp/*.elf inte

### [☐ deferred] fs:0x28 canary check is print-only and never fails the build -- a stack-canary regression ships silently
- **boot-build** · `scripts/build_all.sh:580-584` · conf=0.95
- The canary loop computes `n=$(objdump -d /tmp/$e.elf | grep -c "fs:0x28" || true)` and merely `echo "  $e=$n"`. There is no aggregation and no nonzero-exit, so even with `set -e` (line 4) the build always succeeds regardless of canary counts. The whole point of the check -- catching a userspace binary that re-acquired the fs:0x28 stack-protector reads that #GP at ring 3 (these freestanding apps have no GS/TLS canary slot) -- is defeated: a regression that reintroduces the canary in, say, the IDE or compositor would print `ide=12` and the ISO would still be produced and flashed. Contrast the ad
- **fix:** Accumulate a failure flag: `tot=$((tot+n)); [ "$n" != 0 ] && { echo "FATAL: $e has $n fs:0x28 canary refs"; bad=1; }` and `[ "$bad" = 1 ] && exit 1` after the loop. Driving the list from the staged files (see prior finding) closes the coverage gap at the same time.

### [☐ deferred] Window animations / fade-in / toast add no damage -> a concurrent client commit clips the in-flight animation
- **compositor** · `userspace/compositor/compositor_m8.c:1581-1586` · conf=0.72
- anim_tick keeps the frame dirty while any window is animating (PH_OPENING/CLOSING/MINIMIZING/RESTORING/SNAPPING or fade_alpha<255, lines 1581-1584) and while a toast fades (1586) by calling ONLY mark_dirty() â€” it never calls damage_add() nor forces g_full_damage_cooldown. Contrast lines 1597 and 1614, where the right-dock fan and hover-magnify (which draw OUTSIDE any window's commit rect) DO force the cooldown to avoid being clipped by the narrow scissor. An animating window or a toast is also drawn anywhere on screen (render_window_anim/render_window_snapping/render_toast). handle_create se
- **fix:** In anim_tick, mirror the rdock-fan fix: in the loop at 1581-1584 call win_footprint(win,...) + damage_add(...) for each window with phase!=PH_NONE || fade_alpha<255 (and damage_add the toast rect when g_toast_dur_ms>0), OR simply set g_full_damage_cooldown=FULL_DAMAGE_COOLDOWN_FRAMES whenever any window is animating/fading or a toast is live, exactly as lines 1597/1614 do for the dock.

### [☐ deferred] Per-second clock pulse forces a full-screen recomposite (defeats the damage scissor on an idle desktop)
- **compositor** · `userspace/compositor/compositor_m8.c:5557-5571` · conf=0.85
- The once-per-second clock pulse (the one perpetual 'animation' the gate must service so HH:MM:SS advances) calls mark_dirty() at 5564 but never damage_add(); refresh_rtc/refresh_net_status/refresh_battery add no damage either. So on an otherwise-idle desktop g_dmg_any stays 0, and the scissor gate at 5604 (if scene && g_dmg_any && ... ) falls to the else branch -> scissor_reset_full() -> composite() re-rasterizes the ENTIRE scene (full wallpaper via render_desktop, every window, both docks, panel) once per second. The B6 damage scissor was built specifically to avoid this; present_diff then st
- **fix:** On the per-second pulse, damage_add() just the clock-pill rect (and the net/battery indicators it refreshes): roughly x=[w - clk_w - 17, w-7], y=[(PANEL_H-FONT_H)/2-2, ...+FONT_H+4], extended left to cover the net/battery labels. Then the gate narrows the scissor to that strip and the idle desktop repaints only the clock.

### [☐ deferred] Hover-gate marks dirty without damage -> full recomposite on every mouse move over chrome / during a drag
- **compositor** · `userspace/compositor/compositor_m8.c:4658-4673` · conf=0.8
- pump_input's smooth-mouse gate (4658-4673) was narrowed (GUI-LAT-1) so a bare-canvas move only slides the cursor sprite, but when the cursor is over the top panel, bottom dock, right-dock strip, or a window titlebar â€” or a menu/dialog is open, a snap preview/toast is live, or a button is held (drag) â€” it calls mark_dirty() (4673) with NO damage_add(). handle_mouse does the same (mark_dirty at 5015-5017 for any click/drag/menu in flight; the drag move at 5125-5127 updates dw->x/dw->y but adds no damage). Result: g_dmg_any=0 -> the gate at 5604 uses a full-screen scissor -> composite() re-ra
- **fix:** When the gate decides hover==1, damage_add() only the reactive region(s) actually involved: the panel band (0..PANEL_H), the dock band (H-DOCK_H..H), the right-dock strip (W-RDOCK_W..W), or the hovered window's titlebar footprint. For a drag, damage_add() the union of the window's OLD and NEW footprints (win_footprint before and after updating dw->x/y). Leave the full-screen fallback only for menu

### [☐ deferred] FB-uncached-on-T410 blocker is unsolved: MTRR-WC is defeated by a firmware UC MTRR and no PAT path exists
- **drivers-fb** · `kernel/drivers/framebuffer.c:115-222` · conf=0.82
- fb_enable_write_combining() programs a free variable-range MTRR to mark the framebuffer Write-Combining, but the function's own comment (lines 33-35 and the runtime log at 220-221) admits the fatal limitation: per the Intel SDM MTRR overlap rule, when the firmware has already placed an MTRR (or the default type) marking the FB region UC, 'UC WINS' and the WC MTRR has NO effect. On the ThinkPad T410 the firmware maps the linear FB UC precisely this way (stated in the header comment lines 16-20), so on the actual target hardware this code runs, logs success, and changes nothing â€” the composito
- **fix:** Implement the B5 PAT path: at boot, program IA32_PAT (MSR 0x277) to place WC in a spare PAT index (e.g. PA4), then map the FB pages with the corresponding PAT/PCD/PWT bit combination in vmm_map_page (kernel.c:657) instead of bare PAGE_WRITE. Keep the MTRR attempt as a secondary mechanism but stop claiming success when a firmware UC MTRR overlaps â€” read back the effective type and log HONESTLY wh

### [☐ deferred] hda_msleep busy-spins its full 4,000,000-iteration cap at boot (IF=0, no current process) -> multi-second boot CPU-hog
- **drivers-fb** · `kernel/drivers/hda.c:22-38` · conf=0.88
- hda_msleep() waits by spinning on timer_get_ticks() until a deadline, with the comment claiming sys_yield 'lets other IF=1 tasks run (IRQ0 ticks during them).' This assumption is violated at boot: hda_init() is called at kernel.c:942, but interrupts are only enabled (sti()) at kernel.c:1699. So during the entire HDA bring-up, IF=0 => IRQ0 (PIT) never fires => timer_get_ticks() NEVER advances, so the `while (timer_get_ticks() < end)` condition can only be broken by the iteration cap. Worse, hda_init runs in the boot thread before any process exists, so process_get_current() returns NULL and sys
- **fix:** Detect the pre-sti / no-scheduler boot phase and fall back to a tick-independent bounded delay (e.g. the same outb(0x80) ISA-cycle io-delay used in pit.c timer_sleep's IF=0 path, or a calibrated TSC spin) instead of yielding on a frozen tick. Simplest: if pit_rflags_if()==0 OR process_get_current()==NULL, busy-delay via TSC/io-port for `ms` and return, rather than spinning 4M times calling a sysca

### [☐ deferred] poll()/select() on stdin (fd 0) can never report readable â€” no non-consuming keyboard probe wired
- **drivers-fb** · `kernel/core/syscall/poll.c:81` · conf=0.85
- fd_poll_state() returns 0 for fd==0 with the comment 'stdin: no non-consuming probe wired yet.' This means any program that does poll(stdin)/select(stdin) to wait for keyboard input will block until timeout and then report stdin NOT readable even when keystrokes are queued â€” i.e. interactive readiness-driven programs (shells, editors using select on stdin) cannot work. The capability to answer the probe already exists and is non-consuming: ps2.c maintains kb_read_pos/kb_write_pos (a ring) and can also peek the 8042 OUTPUT_FULL status bit; either `kb_read_pos != kb_write_pos` or `(inb(PS2_STA
- **fix:** Add a non-consuming `int ps2_input_pending(void)` in ps2.c returning `(kb_read_pos != kb_write_pos) || (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)`, declare it in a header, and in fd_poll_state() change the fd==0 case to `return ps2_input_pending() ? POLLIN : 0;`. (If stdin is bound to a stdio channel, also check channel readability, mirroring sys_read's CHANNEL-0 branch at handlers.c:650.)

### [☐ deferred] FAT32: total_clusters not validated against FAT buffer size -> OOB heap read in fat32_get_next_cluster
- **filesystem** · `kernel/fs/fat32.c:57-68, 611, 618-619` · conf=0.85
- fs_data->fat is allocated as fat_size_32 * bytes_per_sector bytes (line 618), i.e. (fat_size_32*bytes_per_sector/4) uint32 entries. fat32_get_next_cluster() bounds the index only with `cluster >= fs_data->total_clusters` (line 58), but total_clusters is computed independently as data_sectors / sectors_per_cluster (line 611) from different untrusted boot-sector fields. A crafted/corrupt image where total_clusters exceeds the number of FAT entries actually allocated makes `fs_data->fat[cluster]` (line 62) read past the kmalloc'd FAT buffer â€” an out-of-bounds kernel heap read whose value then s
- **fix:** After computing total_clusters in fat32_mount, clamp/validate it against the FAT capacity: `uint32_t fat_entries = fat_size_bytes / 4; if (fs_data->total_clusters > fat_entries) fs_data->total_clusters = fat_entries;` (or reject the mount). Then the existing `cluster >= total_clusters` guard also bounds the FAT array access.

### [☐ deferred] FAT32: fat32_detect() inspects an uninitialized buffer (never reads the boot sector)
- **filesystem** · `kernel/fs/fat32.c:489-519` · conf=0.9
- fat32_detect() kmallocs a 512-byte boot_sector buffer (line 491) and then immediately tests boot_sector->signature / fat_size_16 / fat_size_32 (lines 501,507,512) WITHOUT ever calling block_read into it. The buffer contains heap garbage, so detection succeeds or fails at random regardless of the actual on-disk content. The comments at 496-498 even acknowledge it is a placeholder. Any auto-detection-driven mount of FAT32 is therefore unreliable.
- **fix:** Mirror ext2_detect: `block_device_t* dev = block_get_device(source); if (!dev) { kfree(boot_sector); return -1; } if (!block_read(dev, 0, 1, boot_sector)) { kfree(boot_sector); return -1; }` before validating signature/fields. (ext2_detect at ext2.c:518-551 is the correct template.)

### [☐ deferred] NVMe: nvme_build_prp_list only handles transfers <= 2 pages -> bad PRP / DMA corruption for >8KB I/O
- **filesystem** · `kernel/drivers/storage/nvme.c:690-700, 898, 931` · conf=0.85
- nvme_build_prp_list sets prp1=phys and, when size>PAGE_SIZE, prp2=phys+PAGE_SIZE treated as a literal DATA pointer (line 698, with an explicit 'TODO: Implement PRP list for multi-page transfers'). Per the NVMe spec prp2 is only a data pointer when the transfer spans exactly two pages; for transfers spanning >2 pages prp2 must point to a PRP list. nvme_read/nvme_write accept count as uint16_t (up to 65535 sectors) and call nvme_build_prp_list(phys,(count+1)*512,...) (lines 898,931). Any transfer larger than 8KB (count>=16 for 512B sectors) therefore programs an incorrect prp2, causing the contr
- **fix:** Build a real PRP list: for size>2 pages, allocate a PRP-list page, fill it with the per-page physical addresses, and set prp2 to that page's physical address; OR clamp nvme_read/nvme_write to reject count whose byte size exceeds 2*PAGE_SIZE until the PRP list is implemented.

### [☐ deferred] signal.c is a stub: signal()/sigaction() never register handlers with the kernel (the new SYS_RT_SIGACTION syscall is never called)
- **libc** · `userspace/libc/signal.c:51-98` · conf=0.97
- signal() (line 51) and sigaction() (line 72) only write into the static signal_handlers[] table (lines 62, 92) and explicitly note 'this would use a syscall ... For now, this is just a userspace-only implementation' (lines 64-66, 93-94). They never invoke SYS_RT_SIGACTION (107), which the kernel fully implements in kernel/core/signal/kill.c:sys_rt_sigaction (sig, handler, restorer). Consequence: a program that does signal(SIGUSR1, h) and then receives SIGUSR1 (via kill from another process) hits the kernel's deliver_pending_signals with p->sig_handlers[SIGUSR1]==0 (SIG_DFL) and is TERMINATED i
- **fix:** Add raw wrappers and a restorer trampoline in signal.c. (1) Provide __libc_sigrestorer: `__attribute__((naked)) static void __libc_sigrestorer(void){ __asm__ volatile("mov $109,%rax; syscall; ud2"); }` (mirrors sigtest's restorer; 109==SYS_RT_SIGRETURN). (2) In sigaction()/signal(), after updating the local table for SIG_ERR-on-bad-arg compatibility, call the kernel: `syscall6(107, signum, (long)h

### [☐ deferred] libc sigset_t uses bit (1<<(sig-1)); kernel uses bit (1<<sig) â€” off-by-one that breaks any future sigprocmask wiring and is already inconsistent with strsignal
- **libc** · `userspace/libc/signal.c:150-176` · conf=0.9
- sigaddset/sigdelset/sigismember (lines 155, 165, 175) operate on bit (1UL << (signum - 1)). But the kernel signal mask/pending bitsets use bit (1ull << sig) directly: see kernel/core/signal/kill.c lines 243/325/332/414 (e.g. `s &= ~((1ull<<SIGKILL)|(1ull<<SIGSTOP))`) and kernel/include/sched.h:511 ('sig_mask bit (1ull<<s)'), and userspace/apps/sigtest/sigtest.c:83 which builds `set=(1UL<<SIGUSR1)`. So a sigset_t built by libc sigaddset(set, SIGUSR1) sets bit 9, but the kernel interprets bit 9 as a different signal â€” every mask/pending value would be shifted by one once sigprocmask is wired t
- **fix:** Change the three set ops (and sigfillset's mask if you want SIGKILL/SIGSTOP semantics) to use (1UL << signum) to match the kernel and sigtest. Keep the 1..31 range checks. After this, wiring sigprocmask (next finding) to SYS_RT_SIGPROCMASK passes the sigset_t through unchanged.

### [☐ deferred] exit() does not flush stdio buffers or run atexit handlers â€” buffered stdout is lost on normal return; atexit() is dead code
- **libc** · `userspace/libc/syscall.c:26-30` · conf=0.92
- exit() (syscall.c:26) calls SYS_EXIT immediately with no cleanup. stdout is line-buffered (stdio.c:15, _IOLBF) with an 8KB buffer, so any printf output not terminated by '\n' sits unflushed in stdout_buffer and is discarded when the process exits â€” silent output loss. Separately, atexit()/the helper __call_atexit_handlers() (stdlib.c:736, 746) are fully written but NEVER called from any exit path, so registered handlers never run. start.asm also exits main's return value via a raw SYS_EXIT (lines 16-18), bypassing libc exit() entirely, so even programs that return normally never flush. The R
- **fix:** Make exit() do cleanup before SYS_EXIT: call __call_atexit_handlers() then fflush(NULL) (flush all streams) then SYS_EXIT. To also cover `return from main`, change start.asm to `call exit` instead of issuing the raw SYS_EXIT (mov rdi,rax; call exit). Declare __call_atexit_handlers/fflush as extern in syscall.c or move exit() into a C file that includes stdio/stdlib. Keep _exit() as the no-cleanup 

### [☐ deferred] lseek() is a hard-coded stub returning -1, silently breaking fseek/ftell/rewind/ungetc/append, even though the kernel has full vfs_lseek
- **libc** · `userspace/libc/syscall.c:81-88` · conf=0.9
- lseek() (syscall.c:81) always returns -1 with the comment 'lseek syscall doesn't exist yet in kernel'. But the kernel implements vfs_lseek (kernel/fs/vfs.c:913) for ramfs/fat32; the only thing missing is a SYS_LSEEK number in kernel/include/syscall.h. Because libc lseek fails: fopen(path,"a") append mode calls lseek(fd,0,SEEK_END) (stdio.c:467) which fails, so appends start at offset 0; fseek (stdio.c:664), ftell (stdio.c:674), rewind, fsetpos, and ungetc (stdio.c:795) all silently fail. Any ported tool that does random-access file I/O is broken. README line 27 marks fseek/ftell/rewind as full
- **fix:** Two-part: (kernel dependency) add `#define SYS_LSEEK <free-slot>` and register a sys_lseek that calls vfs_lseek(fd, offset, whence). (libc) replace the stub body with `return (off_t)syscall6(SYS_LSEEK, fd, offset, whence, 0,0,0);`. If adding a kernel syscall is out of scope for this pass, at minimum document the stub loudly and make fopen("a") and fseek return failure that callers can detect (they

### [✅ FIXED] copy_to_user silently EFAULTs on still-CoW user buffers (same class as the signals bug, but unfixed everywhere except kill.c)
- **memory** · `kernel/core/mem/vmm.c:733` · conf=0.83
- copy_to_user calls user_range_is_accessible(dst_addr, n, /*need_write=*/true) at line 733, which (lines 172-219) requires PAGE_WRITE set on EVERY destination page. After fork(), every writable page in the child is demoted to read-only + PTE_COW (handlers.c:215). A userspace WRITE to such a page self-heals via the page-fault handler -> cow_handle_write (vma_region.c:66). But a kernel copy_to_user does NOT fault through that path: it pre-checks writability, sees PAGE_WRITE clear on the CoW page, and returns COPY_EFAULT WITHOUT ever calling cow_handle_write. There is no kernel #PF fixup for the m
- **fix:** Centralize the fix inside copy_to_user (and copy_user_string): when user_range_is_accessible(...,true) fails, before returning EFAULT, loop cow_handle_write(pg) over each destination page [dst & ~0xFFF .. (dst+n-1) & ~0xFFF] and re-run the check; only return EFAULT if it still fails (genuinely unmapped or RO non-CoW). cow_handle_write is a documented no-op on already-writable/non-CoW pages, so thi

### [☐ deferred] RX is driven only by synchronous sock_poll(); NIC IRQs are permanently masked (the RX bottleneck)
- **networking** · `kernel/drivers/net/e1000.c:1027, 1071, 1216-1260; kernel/net/socket.c:365-391; kernel/core/syscall/poll.c:37-45` · conf=0.85
- e1000_init() and e1000_pch_deferred_bringup() both end with mmio_write32(E1000_IMC, 0xFFFFFFFF) masking ALL device interrupts (line 1027/1071), and there is no e1000 ISR registered anywhere. The ONLY thing that pulls frames out of the RX ring is e1000_rx_poll(), reached exclusively through net_recv()->sock_poll(), which is called synchronously from blocking syscalls (tcp_connect/tcp_send/tcp_recv loops, sock_selftest) and from poll_pump()/sys_poll/sys_select. Consequences: (1) between syscalls nothing drains the 64-descriptor RX ring, so a burst >64 frames (or any traffic while the CPU is in u
- **fix:** Add an e1000 RX interrupt path: in setup, program RDTR/RADV and IMS = RXT0|RXDMT0|RXO|LSC; register an IRQ handler (via the kernel's IRQ table using dev->interrupt_line / MSI) that reads ICR, drains DD descriptors into the same demux net_input()/ipv4_demux uses, and wakes any waiter (e.g. signal g_poll_wobj). Keep sock_poll() as a fallback drain. Until then, at minimum document the per-call single

### [☐ deferred] tcp_send congestion-window in_flight accounting is heuristic and can desync; pipelining is also serialized by synchronous TX
- **networking** · `kernel/net/tcp.c:599-730 (esp. 613-645, 705-716)` · conf=0.75
- tcp_send claims a 4-segment pipeline but the bookkeeping is approximate and self-inconsistent. (1) It only ever arms retransmit for the LAST segment (tcp_arm_retransmit at 708 overwrites the previous slot), so if any segment except the tail is lost, it is never retransmitted by tcp_tick â€” true loss recovery is broken for all but the final in-flight segment (the code admits this at 705-707 but it is a real reliability bug, not just a perf note). (2) acked_segs is estimated from (snd_una-old_una)/TCP_MSS (623), but segments can be < MSS (final chunk, window-limited chunks), so in_flight can be
- **fix:** Track outstanding bytes (snd_nxt - snd_una) instead of a segment count; cap by min(cwnd_bytes, snd_wnd). For real loss recovery, keep a small retransmit queue of unacked segments (the sock_t single rt_data slot is the root limit â€” a ring of a few segment descriptors, heap-backed like tcp_ooo, would allow retransmitting any lost segment). For TX throughput, use e1000_transmit_batch (already imple

### [✅ FIXED] poll/select/epoll copy_to_user to a CoW user buffer returns spurious EFAULT (no cow_handle_write pre-resolve)
- **poll-select** · `kernel/core/syscall/poll.c:132-134 (poll), 206-208 (select); kernel/core/syscall/epoll.c:399-403 (epoll_wait)` · conf=0.86
- All three syscalls write their results back with copy_to_user (poll.c:132 writes kfds, select.c:206-208 write out_r/out_w/out_e, epoll.c:399 writes the event array). copy_to_user -> user_range_is_accessible(dst,n,need_write=true) (vmm.c:733,173-175) REQUIRES PAGE_WRITE on every destination page and returns COPY_EFAULT otherwise. After a fork() with CoW enabled, the caller's output buffer (the pollfd array / fd_set / epoll_event array on the user stack or heap) is mapped read-only + PAGE_COW until first written. Since the kernel writes it on the child's behalf before the child ever touches it, 
- **fix:** Before each result-writing copy_to_user, pre-resolve CoW for the destination range exactly as kill.c does: for (uint64_t pg = (dst & ~0xFFFULL); pg <= ((dst + len - 1) & ~0xFFFULL); pg += 0x1000) cow_handle_write(pg);  cow_handle_write is a no-op on already-writable/non-CoW pages (cow.c:147-148) so it is safe to call unconditionally; a genuinely unmapped page is still caught by copy_to_user's own 

### [☐ deferred] epoll instances leak permanently â€” no close/destroy path and no process-exit reclamation
- **poll-select** · `kernel/core/syscall/epoll.c:241-257 (create sets used=true), 119 (only epoll_init clears used); kernel/core/syscall/handlers.c:863-888 (sys_close)` · conf=0.83
- sys_epoll_create marks g_epoll_instances[idx].used=true and returns the encoded fd 0x10000+idx, but nothing ever sets ep->used=false again after init. sys_close (handlers.c:872) rejects fd>=MAX_FDS(1024), and an epoll fd is >=0x10000=65536, so close(epfd) returns EBADF and never frees the instance. There is also no owner_pid on epoll_instance_t and no epoll hook in the process-exit cleanup (grep shows epoll_notify_socket/epoll_add_ready are the only epoll externs, neither is a cleanup), unlike sockets which have sock_cleanup_process. EPOLL_MAX_INSTANCES is 64 and SYSTEM-WIDE. Therefore any pro
- **fix:** Add sys_epoll_close()/recognize the 0x10000 range in sys_close: validate via epoll_from_fd, then under g_epoll_lock set ep->used=false and reset watches/ready ring. Add owner_pid to epoll_instance_t (set from process_get_current in epoll_create) and an epoll_cleanup_process(pid) called from the process teardown path (mirror sock_cleanup_process) to reclaim instances on exit.

### [☐ deferred] epoll ready-event ring, wait_queue, epoll_add_ready() and epoll_notify_socket() are dead/contradictory code never used by epoll_wait
- **poll-select** · `kernel/core/syscall/epoll.c:82-88 + 169-192 (ring + add_ready), 421-439 (notify), 354-415 (wait re-scans instead)` · conf=0.8
- sys_epoll_wait (378-414) computes readiness by re-scanning every watch with fd_poll_state() each iteration and sleeps via poll_sleep_slice (a timer-only wait). It NEVER reads ep->ready_events/ready_count/ready_head/ready_tail and NEVER blocks on ep->wait_queue. Consequently the entire ring-buffer machinery (ready_events[128] per instance x 64 instances), epoll_add_ready(), wq_wake_one on ep->wait_queue, and epoll_notify_socket() are dead code. epoll_notify_socket is in fact never called anywhere in the tree (grep: only its own definition). This contradicts the file's own header ('O(1) wakeup: 
- **fix:** Either (a) delete the ring/wait_queue/epoll_add_ready/epoll_notify_socket scaffolding and update the header to describe the actual pump+re-scan model, or (b) commit to the event-driven design: have epoll_wait drain ready_events and block on ep->wait_queue, and call epoll_notify_socket from the TCP/UDP RX completion path with fd_poll_state-derived bits so there is a single last_state writer. Do not

### [☐ deferred] fork() does not copy the parent's VMA list -> forked child crashes on lazy stack growth / any demand fault
- **process** · `kernel/core/syscall/handlers.c:359-416` · conf=0.92
- sys_fork() creates the child via process_create() (which memset-zeroes the PCB, so child->vma_list == NULL at process.c:224) and then calls fork_copy_user_pages(child) to copy only the currently-MAPPED user pages. It never copies the parent's per-process VMA list. The page-fault handler (kernel/core/mem/vma_region.c:72-75) resolves a not-present user fault by calling vma_find(cur, fault_addr) and returns 0 (genuine segfault -> process killed) when no VMA covers the address. Because exec installs the user stack as a LAZY VMA_ANON GROWSDOWN region (exec.c:688, LAZY_ANON_STACK=1 pre-faults only t
- **fix:** After fork_copy_user_pages(child) succeeds in sys_fork(), deep-copy the parent's VMA list into the child: walk parent->vma_list and vma_add(child, node) for each entry (the descriptors are value-copied by vma_add). A helper like vma_copy_list(parent, child) in vma_rbtree.c is the natural home. Ensure file-backed VMAs keep the same file_ptr/off/sz so the child can re-fault code pages identically; a

### [☐ deferred] fork() does not inherit the parent's file-descriptor table -> child loses all open fds
- **process** · `kernel/core/syscall/handlers.c:356-416` · conf=0.85
- The per-process open-file table is process_t.fd_table[1024] (sched.h:277), routed by cur_fdt() in vfs.c:321. process_create() memset-zeroes the whole PCB (process.c:224), so the fork child's fd_table starts entirely NULL, and sys_fork() never copies the parent's table or bumps any inode/dentry/file refcounts. This violates POSIX fork semantics: a child must inherit copies of the parent's descriptors sharing the underlying open-file/inode. Concretely, a process that open()s a file (fd>=3) then fork()s gives the child a child where fd is EBADF, so the standard 'open in parent, read in child' and
- **fix:** In sys_fork(), after creating the child, iterate fd 3..VFS_MAX_FDS-1 of the parent's fd_table; for each live vfs_file_t*, either share it with a bumped reference (dup semantics: vfs_inode_get/dentry ref + a new file struct or shared file with a refcount) or, minimally, shallow-duplicate the vfs_file_t and take an inode reference so vfs_close_all_fds on either process balances. Mirror vfs_fd_free's

### [☐ deferred] schedule_from_irq starves a woken RESUME_CRETURN task behind a non-syscalling RESUME_IRETQ CPU burner
- **scheduler** · `kernel/core/sched/scheduler.c:3116-3134` · conf=0.8
- The IRQ preemption path can only resume a successor whose resume_mode==RESUME_IRETQ (it was itself interrupted in ring 3). When scheduler_pick_next() returns a RESUME_CRETURN task (a task that BLOCKED in a syscall and was then re-readied by a waker, OR a kernel thread, OR a task that just SYS_YIELDed), the reject branch at 3116 re-enqueues `next`, refills `current->time_slice`, and returns with the frame untouched so iretq resumes `current`. If `current` is a ring-3 CPU burner that never yields/syscalls/blocks (e.g. `for(;;);`), it is RESUME_IRETQ and keeps winning the CPU every quantum: each 
- **fix:** Convert a RESUME_CRETURN successor into a RESUME_IRETQ resume from the IRQ instead of rejecting it: when next->resume_mode==RESUME_CRETURN and next was interrupted at a kernel continuation, you cannot iretq into it directly, but you CAN force a switch by saving current as RESUME_IRETQ (already done in step 5) and then performing a cooperative context_switch_asm(current,next) from the IRQ tail is u

### [✅ FIXED] SIG_DFL terminate/stop via pending path does not schedule() â€” process resumes in ring 3 after being marked TERMINATED
- **signals** · `kernel/core/signal/kill.c:299-315` · conf=0.83
- signal_default_action() handles the catchable-default terminate and stop dispositions by setting p->state = PROCESS_TERMINATED (line 311) or PROCESS_BLOCKED (line 301) and calling scheduler_remove_process(p) â€” but it then just returns. Its caller deliver_pending_signals() returns into syscall.asm, which unconditionally executes `o64 sysret` (syscall.asm:95/207). So when the signal targets the CURRENTLY running process (the common case: a process running in ring 3 takes a SIG_DFL SIGINT/SIGQUIT/SIGABRT/SIGUSR1/SIGTSTP at its next syscall return), the process is marked dead/stopped and pulled 
- **fix:** When signal_default_action performs a terminate or stop on the current process, do not allow the sysret to resume it. Simplest: have deliver_pending_signals (or signal_default_action) detect p == process_get_current() after a terminate/stop disposition and call schedule() (it will not return for a TERMINATED current, mirroring sys_exit). Alternatively, return a status up to syscall.asm and re-chec

### [✅ FIXED] Threads do not inherit signal dispositions â€” thread_create zeroes handlers/mask/restorer
- **signals** · `kernel/core/sched/process.c:455` · conf=0.8
- thread_create() memsets the new PCB to zero (process.c:455) and never copies the parent's sig_handlers[], sig_mask, or sig_restorer, so every new thread starts with all dispositions = SIG_DFL, empty mask, and no restorer. In POSIX, signal dispositions (handlers) are process-wide and shared by all threads; only the blocked mask is per-thread. Because sys_kill resolves a single PCB by pid/tid (kill.c:78) and signal state lives per-PCB, sending a catchable signal to a thread of a process that installed a handler runs the SIG_DFL action (frequently terminate) instead of the handler â€” and because
- **fix:** In thread_create, after the memset, copy the parent's signal dispositions: memcpy(t->sig_handlers, parent->sig_handlers, sizeof t->sig_handlers); t->sig_restorer = parent->sig_restorer; and inherit the mask (t->sig_mask = parent->sig_mask) per POSIX (new thread starts with the creator's mask). Longer term, hoist sig_handlers/sig_restorer into the shared address-space/thread-group object so sigacti

### [☐ deferred] copy_user_string validates pages against stale active_pml4 (kernel_pml4), not live CR3 -> false EFAULT for mmap'd path strings; potential ring-0 fault on identity-shadowed VAs
- **syscall-boundary** · `kernel/core/mem/vmm.c:131-135, 832-841` · conf=0.82
- copy_user_string checks each page via user_page_is_accessible() -> paging_get_pte(), which walks `active_pml4` (paging.c:677). During a syscall nobody calls paging_set_target(), so active_pml4 == kernel_pml4, NOT the caller's CR3. The bulk copy_from_user/copy_to_user path was DELIBERATELY rewritten (vmm.c:140-147) to walk the live CR3 via read_cr3() precisely because the paging_get_pte view 'false-rejected valid user pages ... breaking boot'; copy_user_string was left on the old broken primitive. Consequences: (a) any path string stored in mmap'd memory is at VA >= VMM_ANON_VA_BASE = 0x1000000
- **fix:** Replace the per-page user_page_is_accessible() call in copy_user_string with the same live-CR3 walk used by user_range_is_accessible (read_cr3() & ~0xFFF, manual PML4->PT descent, require PAGE_PRESENT|PAGE_USER). Simplest: add a one-page helper user_page_ok_live(va, need_write=false) factored out of user_range_is_accessible and call it at each page boundary.

### [☐ deferred] sys_map_file maps arbitrary VFS inode->data physical pages into userspace at a fixed VA with no overlap/identity check (kernel-heap disclosure + silent clobber)
- **syscall-boundary** · `kernel/core/syscall/handlers.c:1341-1375` · conf=0.6
- sys_map_file does vfs_path_lookup(kpath) for ANY file (not just initrd), then maps inode->data's physical address directly into the caller at the HARDCODED base_va = 0x50000000 with PAGE_PRESENT|PAGE_USER (lines 1369-1375). Two problems: (1) inode->data is treated as a raw physical address assuming initrd backing, but a ramfs/VFS file's data may be a kmalloc'd KERNEL HEAP buffer; mapping that PAGE_USER exposes the kernel heap page (and, rounded up to page granularity, adjacent heap bytes) read-only to ring 3 â€” an information disclosure. The comment 'Initrd-backed files point directly into id
- **fix:** Restrict sys_map_file to initrd-backed inodes only (verify the inode source/type, or require inode->data to fall within the known initrd physical range) and reject otherwise with EINVAL. Replace the fixed base_va with a per-AS allocated VA (e.g. via the vma anon cursor) returned to the caller, or at minimum verify the target range is currently unmapped before mapping. Bound file_size to a sane win


## P2 (45)

- **boot-build** `scripts/build_all.sh:750-762` — build_all.sh ships a STALE default kernel with no freshness/existence guard (only the SMP path is guarded)
- **boot-build** `scripts/build_all.sh:351` — gen_img_fixtures.py rewrites the git-tracked source header b2_img_fixtures.h on every build (byte-identity / dirty-tree 
- **boot-build** `kernel/kernel.c:395,404,268-283,700-701` — boot_mark() progress markers for ACPI/SMP are dead no-ops -- they run before the framebuffer is live, so the pre-FB T410
- **compositor** `userspace/compositor/compositor_m8.c:1137-1146` — cz_text scissors only X, never Y â€” chrome text bypasses the vertical damage scissor
- **compositor** `userspace/compositor/compositor_m8.c:3606-3619` — present_diff scans the whole framebuffer every presented frame, ignoring the available damage rect
- **compositor** `userspace/compositor/compositor_m8.c:2299-2303` — render_window_anim titlebar fill ignores the active scissor (inconsistent with the fade-path titlebar)
- **drivers-fb** `kernel/drivers/hda_wav.c:244-247` — hda_play_wav playback-completion loop is a broken non-millisecond hlt() spin that hard-hangs when IF=0
- **drivers-fb** `kernel/drivers/input/input.c:165-175` — Producer-consumer race on unlocked, non-volatile input/evdev ring indices (IRQ advances the consumer's head on overflow)
- **drivers-fb** `kernel/drivers/pit.c:346-392` — timer_sleep busy-hlt path never reschedules (CPU-hog for callers) and the IF=0 io-delay fallback is wildly inaccurate
- **drivers-fb** `kernel/drivers/framebuffer.c:115-211` — SMP: framebuffer WC MTRR is programmed only on the BSP; APs run with the FB at the default (WB) type
- **drivers-fb** `kernel/drivers/framebuffer.c:522-561` — No SFENCE after WC framebuffer bulk stores â€” last frame may linger in WC buffers (latency/tearing once WC is active)
- **drivers-fb** `kernel/drivers/ps2.c:719-723` — ps2.c mouse position clamps to a hardcoded 800x600, ignoring the real (e.g. 1280x800 T410) resolution
- **drivers-fb** `kernel/drivers/input/evdev.c:197-204` — evdev_read has no blocking support (TODO) â€” blocking readers busy-spin in userspace
- **filesystem** `kernel/fs/fat32.c:592-611, 136, 385` — FAT32: bytes_per_cluster uncapped -> multi-MB kmalloc per cluster read; data_sectors underflow
- **filesystem** `kernel/drivers/storage/nvme.c:67-82` — NVMe: enumeration finds only one controller (dead PCI iteration TODO)
- **filesystem** `kernel/drivers/storage/nvme.c:587-643` — NVMe: nvme_wait_for_completion silently consumes other commands' completions
- **filesystem** `kernel/drivers/storage/ahci.c:256-316` — AHCI: per-port DMA pages (cmd_list/rx_fis/bounce/cmd_tables) leak on init-failure paths
- **filesystem** `kernel/fs/ext2.c:61-119, 431-513, 556-736` — Missing tests: no untrusted/corrupt-image parse tests for ext2 or fat32
- **libc** `userspace/libc/signal.c:107-123` — sigprocmask/sigpending/raise/sigsuspend are no-ops or in-process; the kernel syscalls (108/110) exist and are unused
- **libc** `userspace/libc/time.c:486-507` — nanosleep() loses all sub-second precision: rounds tv_nsec to whole seconds despite SYS_SLEEP being millisecond-resoluti
- **libc** `userspace/libc/stdlib.c:727-729` — abort() does not raise SIGABRT and skips stdio flush â€” just exit(134)
- **libc** `userspace/libc/stdlib.c:206-219` — strtol/strtoul/atoi accumulate without overflow detection â€” no clamp to LONG_MAX/MIN, no ERANGE
- **libc** `userspace/libc/malloc.c:50-53` — malloc/free global free-list + global tcache have no locking, but real threads (thread_create) and a PREEMPT=1 build now
- **libc** `userspace/libc/stdio.c:807-828` — sscanf/scanf/fscanf are stubs returning EOF â€” many ported tools parse input with sscanf
- **memory** `kernel/core/mem/cow.c:42` — CoW refcount table still sized for 4GB while the PMM bitmap was grown to 16GB â€” CoW silently disabled for frames above
- **memory** `kernel/arch/x86_64/tlb.c:193` — Lazy TLB shootdown never IPIs a remote CPU running userspace -> stale-TLB use-after-free window after munmap/unmap on SM
- **memory** `kernel/core/mem/vmm.c:656` — vmm_protect saves/restores the SOFTWARE active_pml4 target using the HARDWARE CR3 value
- **networking** `kernel/net/socket.c:370-380 (with kernel/drivers/net/e1000.c:1258 and kernel/net/net.c:618-626)` — sock_poll drain loop aborts on the first malformed/runt frame, under-draining good frames behind it
- **networking** `kernel/net/net.c:124-143; kernel/net/socket.c:110-158 (resolve_mac)` — ARP cache is 16 entries with crude slot-0 eviction; eviction of the gateway re-triggers an in-syscall ARP wait on every 
- **networking** `kernel/net/net.c:56-95, 362-433` — IPv4 fragment reassembler uses a per-byte holes[] bitmap of 65535 bools per slot (16 slots) â€” ~2MB .bss and O(n) per f
- **networking** `kernel/net/net_testrig.c, kernel/net/net_features_test.c, tests/tcp_server_test.c:n/a` — No automated test exercises TCP OOO reassembly, SYN side-table eviction/flood, retransmit, or full-ring RX drop
- **poll-select** `kernel/core/syscall/poll.c:62-74` — fd-namespace overlap: a live socket index (0..31) silently shadows a same-numbered vfs file fd (3..31) in fd_poll_state
- **poll-select** `kernel/core/syscall/poll.c:110-129 (poll), 179-203 (select); kernel/core/syscall/epoll.c:378-414 (epoll_wait)` — poll/select/epoll_wait blocking loops are not signal-interruptible (no EINTR) â€” a pending catchable signal cannot brea
- **poll-select** `kernel/core/syscall/epoll.c:390-403` — epoll_wait edge-trigger: w->last_state is committed before copy_to_user, so an EFAULT permanently loses the edge
- **poll-select** `kernel/core/syscall/epoll.c:381-406` — ep->lock (plain busy-wait spinlock, no IRQ disable) is held across copy_to_user in epoll_wait
- **poll-select** `kernel/core/syscall/poll.c:107,126 (poll), 175,200 (select); kernel/core/syscall/epoll.c:371,411` — T410_POWER_SAVE build (250 Hz PIT) silently inflates all poll/select/epoll timeouts 4x (timer_get_ticks != milliseconds)
- **poll-select** `kernel/core/syscall/poll.c:80` — poll/select on stdin (fd 0) can never report readable â€” interactive poll-on-stdin always times out
- **poll-select** `userspace/apps/pollselftest/pollselftest.c:66-140 (entire test); scripts/pollselect_smoke.sh` — Missing tests: CoW-after-fork EFAULT, epoll-on-socket transitions, epoll instance leak/recycle, select exceptfds, signal
- **process** `kernel/core/syscall/handlers.c:43-76` — sys_exit() has no thread-group (exit-group) semantics -> sibling threads keep running after exit()
- **scheduler** `kernel/core/sched/scheduler.c:2387-2393` — scheduler_yield_requeue keeps the RACE-001 unlock/relock gap that scheduler_add_process was fixed to remove
- **scheduler** `kernel/core/syscall/poll.c:110-129, 179-203` — Infinite poll()/select()/epoll_wait() are a fixed 5 ms busy re-poll, not an event-driven block
- **signals** `userspace/libc/signal.c:51-98, 183-194, 269-277, 107-123` — libc signal API is a no-op stub disconnected from the working kernel signal syscalls
- **signals** `userspace/libc/signal.c:155, 165, 175` — libc sigset_t bit convention (1<<(signum-1)) disagrees with kernel (1<<signum) mask/pending words
- **signals** `kernel/core/signal/kill.c:246-250` — No EINTR semantics: a signal that aborts a blocking syscall lets it return its normal value, not -EINTR
- **syscall-boundary** `kernel/core/syscall/epoll.c:390-404` — epoll_wait advances watch->last_state before copy_to_user, losing the edge event on a transient EFAULT

## P3 (23)

- **boot-build** `kernel/arch/x86_64/boot.asm:189-194` — boot.asm BSS clear (rep stosq) can leave up to 7 bytes of .bss uncleared if __bss_end is not 8-byte aligned
- **boot-build** `kernel/arch/x86_64/boot.asm:164-184` — Initrd .bss-rescue copies to a hardcoded 16 MiB with no bounds check against kernel size or initrd size
- **filesystem** `kernel/fs/vfs.c:301-317` — VFS: stale design-note claims fds leak at process exit (already fixed)
- **filesystem** `kernel/fs/ext2.c:316-333` — ext2/fat32 read paths kmalloc+free a block buffer on every block/cluster iteration
- **libc** `userspace/libc/README.md:27-46` — README and inline comments are stale: fseek/ftell/rewind and atexit advertised as working; getenv/setenv marked stub but
- **libc** `userspace/libc/signal.h:138-139` — signal.h declares sigtimedwait() with 'struct timespec*' but never declares the type, creating a parameter-scope struct 
- **memory** `kernel/core/mem/vmm.c:170` — user_range_is_accessible walks page tables via RAW physical pointers, not PHYS_TO_DIRECT â€” inconsistent with the harde
- **memory** `kernel/include/mem.h:11` — DIRECT_MAP header comment in mem.h describes the OLD aliased PML4[256] design that paging.c explicitly replaced (and lab
- **networking** `kernel/net/tcp.c:856-872` — Unreachable TCP_SYN_RCVD handler â€” dead code after the SYN side-table rewrite
- **networking** `kernel/net/udp.c:54-55, 72-78` — net_icmp_port_unreachable declared and intended but never defined or called â€” UDP port-unreachable is silently dropped
- **networking** `kernel/net/socket.c:57-61 (used by sock_socket:454 and tcp_connect via sock_alloc_port)` — Ephemeral local-port allocator never checks for in-use ports; demux can misroute on wrap/collision
- **networking** `kernel/net/socket.c:293-321` — sock_find_tcp can return a connected child instead of the LISTEN socket for a fresh SYN on the same local_port
- **networking** `kernel/net/tcp.c:355-414` — synq_on_ack promotes on ack-number alone, without validating the segment seq against rcv_nxt
- **networking** `kernel/net/tcp.c:48 (documented), 767-788, 1031-1062` — TCP has no TIME_WAIT 2MSL and no CLOSE_WAIT timeout â€” half-closed/closing sockets can linger as used until process exi
- **poll-select** `kernel/core/syscall/poll.c:194 (select uses POLLPRI); fd_poll_state 62-82 never sets POLLPRI` — select() exceptfds is effectively dead â€” fd_poll_state never asserts POLLPRI
- **poll-select** `kernel/core/syscall/poll.c:168-172` — select() timeout conversion can overflow int64 and be misread as infinite; tv_usec not range-checked
- **process** `userspace/apps/forktest/forktest.c:54-80` — forktest does not exercise lazy stack growth or fd inheritance, masking the two fork bugs
- **process** `kernel/fs/vfs.c:301-317` — Stale vfs.c design comment claims a global fd table with no per-process cleanup (contradicts the implemented per-process
- **scheduler** `kernel/core/sched/scheduler.c:2960, 2984, 3045, 3131, 3141` — process_t.need_resched is written by schedule_from_irq but never read (dead PCB state)
- **scheduler** `kernel/core/sched/scheduler.c:1923-1924` — BLOCKED-state scheduler invariant does not model the legitimate SIGSTOP/SIGTSTP-stopped state
- **signals** `kernel/core/signal/kill.c:292, 427` — g_sig_frame global and sig_pending |= are not SMP-safe once a second CPU runs ring-3 with signals (documented TODO, but 
- **signals** `kernel/core/signal/kill.c:339, 397` — Custom handler with NULL restorer silently terminates the process on first delivery (footgun, undocumented)
- **syscall-boundary** `kernel/core/syscall/poll.c:168-172` — select() timeout computation overflows for large tv_sec, silently turning a long timeout into a non-blocking poll