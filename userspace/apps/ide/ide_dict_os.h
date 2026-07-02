/*
 * ide_dict_os.h -- AutomationOS API dictionary for the Semantic-LEGO IDE
 *                  autocomplete engine (ide_complete.c).
 * =========================================================================
 *
 * THE REAL kernel ABI surface, as autocomplete entries: every syscall with its
 * exact NUMBER, argument meanings, return value, and a ready-to-insert snippet.
 *
 * >>> THIS DICTIONARY *IS* THE HEADER FOR ON-DEVICE CODING. <<<
 * The on-device compiler (cc_*.c) has NO #include and NO #define -- preprocessor
 * lines are skipped -- and NO inline asm. A program reaches the kernel through
 * three compiler BUILTINS (cc_expr.c:719-773), no header and no libc required:
 *     syscall(n, a1, a2, a3)   -- ANY syscall; n->rax, a1->rdi, a2->rsi, a3->rdx,
 *                                 result returned in rax. *** 3 ARGS MAX. ***
 *     sys_write(fd, buf, len)  -- shorthand for SYS_WRITE (3)
 *     sys_exit(code)           -- shorthand for SYS_EXIT (0); crt0 also calls
 *                                 this with main()'s return value.
 * Because the builtin passes only 3 args (rdi/rsi/rdx), syscalls that need a
 * 4th-6th argument (e.g. msgsnd, msgrcv, prlimit, epoll_ctl, thread_create with
 * a stack, mmap) are NOT reachable on-device through the builtin -- those are
 * marked "[host build: needs >3 args]". On a HOST gcc build you instead use an
 * inline-asm helper that loads r10/r8/r9 (see userspace/apps/screenshot/screenshot.c's
 * sc()). NOTE: cpu1hello.c's sc() uses inline asm, which the on-device cc rejects;
 * on-device always prefer the syscall() builtin.
 *
 * Snippets here use the on-device-safe builtin form: syscall(<number>, ...).
 * The numeric literal is required (no SYS_* macro exists on-device).
 *
 * ===================== ON-DEVICE TYPE-LAYOUT WARNING ========================
 * On-device cc makes EVERY non-char type 8 bytes (cc_type.c:322-342), and array
 * indexing scales by element size. So:
 *   - You CANNOT declare the kernel's fb_acquire_t {u64; u32; u32; u32; u32}
 *     correctly on-device (its u32 fields would each take 8 bytes -> wrong
 *     offsets). Read it into `long fbi[3]` and decode by hand (see os_fb_open).
 *   - You CANNOT address a 32-bit framebuffer with `unsigned int* p; p[i]` --
 *     that strides 8 bytes. Use `char*` and write 4 bytes per pixel (see
 *     os_putpixel / os_fillrect). These are the "compositor tools" done RIGHT.
 *
 * SOURCES (verified): kernel/include/syscall.h (numbers + comments),
 * userspace/lib/syscall.h (userspace mirror), userspace/include/wl_proto.h
 * (compositor protocol), kernel/core/syscall/handlers.c (sys_read_event,
 * sys_random, sys_clip_*), kernel/ipc/notify.c (sys_notify_post),
 * userspace/apps/screenshot/screenshot.c + beep.c (real usage).
 *
 * ASCII only. Integration: shares IdeDictEntry (ide_dict_c.h) and exposes
 * IDE_DICT_OS[] + IDE_DICT_OS_COUNT. See ide_dict_c.h for the full glue.
 */
#ifndef IDE_DICT_OS_H
#define IDE_DICT_OS_H

#ifndef IDE_DICT_ENTRY_DEFINED
#define IDE_DICT_ENTRY_DEFINED
typedef struct {
    const char* word;
    const char* sig;
    const char* doc;
    const char* snippet;
    char        kind;
} IdeDictEntry;
#endif

static const IdeDictEntry IDE_DICT_OS[] = {
    /* ===================================================================== *
     *  THE ON-DEVICE BUILTINS  (kind 'f')
     * ===================================================================== */
    { "syscall", "syscall(n,a1,a2,a3)", "Compiler builtin: invoke ANY syscall. n->rax, a1->rdi, a2->rsi, a3->rdx; returns rax. 3 ARGS MAX. No header/asm.",
      "syscall(${1:n}, ${2:a1}, ${3:a2}, ${4:a3})$0", 'f' },
    { "sys_write", "sys_write(fd,buf,len)", "Builtin shorthand for SYS_WRITE (3). fd 1=stdout/serial. Returns bytes written.",
      "sys_write(1, ${1:buf}, ${2:len})$0", 'f' },
    { "sys_exit", "sys_exit(code)", "Builtin shorthand for SYS_EXIT (0). Ends the process. crt0 calls it with main()'s return.",
      "sys_exit(${1:0})$0", 'f' },
    { "print_", "print(const char* s)", "No-libc string print to fd 1: measures length then SYS_WRITE. Paste once, call anywhere.",
      "void ${1:print}(const char* s) {\n    long n = 0; while (s[n]) n++;\n    sys_write(1, (long)s, n);\n}$0", 's' },

    /* ===================================================================== *
     *  PROCESS  (kind 'f')
     * ===================================================================== */
    { "SYS_EXIT", "syscall(0, code)", "0: end the process with exit code. Does not return. Use the sys_exit() builtin.",
      "sys_exit(${1:0})$0", 'f' },
    { "SYS_FORK", "syscall(1)", "1: duplicate the process. Returns child pid in the parent, 0 in the child, <0 on error.",
      "long ${1:pid} = syscall(1, 0, 0, 0);   /* SYS_FORK */$0", 'f' },
    { "SYS_WAITPID", "syscall(6, pid, &st)", "6: wait for a child. pid or -1=any; &st receives the exit status. Returns the reaped pid.",
      "long ${1:st} = 0;\nsyscall(6, ${2:pid}, (long)&${1:st}, 0);   /* SYS_WAITPID */$0", 'f' },
    { "SYS_EXECVE", "syscall(7, path)", "7: replace this process image with the program at path. On AutomationOS it spawns path then exits.",
      "syscall(7, (long)\"${1:/bin/app}\", 0, 0);   /* SYS_EXECVE */$0", 'f' },
    { "SYS_GETPID", "syscall(8)", "8: return this process's pid.",
      "long ${1:pid} = syscall(8, 0, 0, 0);   /* SYS_GETPID */$0", 'f' },
    { "SYS_SLEEP", "syscall(9, ms)", "9: block for ms milliseconds (1 tick = 1 ms).",
      "syscall(9, ${1:100}, 0, 0);   /* SYS_SLEEP ms */$0", 'f' },
    { "SYS_YIELD", "syscall(15)", "15: give up the rest of this timeslice. The cooperative-loop heartbeat; call it once per frame.",
      "syscall(15, 0, 0, 0);   /* SYS_YIELD */$0", 'f' },
    { "SYS_SPAWN", "syscall(16, path)", "16: start the ELF at path as a NEW child process. Returns the child pid (>0) or <0.",
      "long ${1:pid} = syscall(16, (long)\"${2:/Desktop/app.elf}\", 0, 0);   /* SYS_SPAWN */$0", 'f' },
    { "SYS_KILL", "syscall(26, pid, sig)", "26: send signal sig to pid (e.g. 9=SIGKILL). Returns 0 or <0.",
      "syscall(26, ${1:pid}, ${2:9}, 0);   /* SYS_KILL */$0", 'f' },
    { "SYS_NICE", "syscall(27, pid, inc)", "27: nudge a process's scheduling priority by inc (higher nice = lower priority).",
      "syscall(27, ${1:pid}, ${2:1}, 0);   /* SYS_NICE */$0", 'f' },
    { "SYS_PROCLIST", "syscall(44, buf, max)", "44: enumerate processes into a user buffer (sysmon). Returns the count written.",
      "long ${1:n} = syscall(44, (long)${2:buf}, ${3:max}, 0);   /* SYS_PROCLIST */$0", 'f' },

    /* ===================================================================== *
     *  FILES & DIRECTORIES  (kind 'f')
     * ===================================================================== */
    { "SYS_READ", "syscall(2, fd, buf, n)", "2: read up to n bytes from fd into buf. Returns bytes read (0 at EOF) or <0.",
      "long ${1:got} = syscall(2, ${2:fd}, (long)${3:buf}, ${4:n});   /* SYS_READ */$0", 'f' },
    { "SYS_WRITE", "syscall(3, fd, buf, n)", "3: write n bytes from buf to fd (1=stdout). Returns bytes written. Or use sys_write().",
      "syscall(3, ${1:1}, (long)${2:buf}, ${3:n});   /* SYS_WRITE */$0", 'f' },
    { "SYS_OPEN", "syscall(4, path, flags)", "4: open path with flags (O_*). Returns an fd (>=0) or <0. mode is arg3 for O_CREAT.",
      "long ${1:fd} = syscall(4, (long)\"${2:/tmp/f}\", ${3:0x241}, 0644);   /* SYS_OPEN O_WRONLY|O_CREAT|O_TRUNC */$0", 'f' },
    { "O_FLAGS", "open flags", "O_RDONLY=0 O_WRONLY=1 O_RDWR=2 O_CREAT=0x40 O_TRUNC=0x200 O_APPEND=0x400. OR them for SYS_OPEN flags.",
      "/* O_RDONLY 0, O_WRONLY 1, O_RDWR 2, O_CREAT 0x40, O_TRUNC 0x200, O_APPEND 0x400 */\n${1:0x241}$0", 'm' },
    { "SYS_CLOSE", "syscall(5, fd)", "5: close a file descriptor. Returns 0 or <0.",
      "syscall(5, ${1:fd}, 0, 0);   /* SYS_CLOSE */$0", 'f' },
    { "SYS_STAT", "syscall(33, path, buf)", "33: fill buf with file status for path. Returns 0 or <0.",
      "syscall(33, (long)\"${1:/path}\", (long)${2:&st}, 0);   /* SYS_STAT */$0", 'f' },
    { "SYS_UNLINK", "syscall(34, path)", "34: delete the file at path. Returns 0 or <0.",
      "syscall(34, (long)\"${1:/tmp/f}\", 0, 0);   /* SYS_UNLINK */$0", 'f' },
    { "SYS_RENAME", "syscall(35, old, new)", "35: move/rename old -> new. Returns 0 or <0.",
      "syscall(35, (long)\"${1:old}\", (long)\"${2:new}\", 0);   /* SYS_RENAME */$0", 'f' },
    { "SYS_MKDIR", "syscall(67, path, mode)", "67: create a directory (recursive) with mode (e.g. 0755). Returns 0 or <0.",
      "syscall(67, (long)\"${1:/Desktop/dir}\", 0755, 0);   /* SYS_MKDIR */$0", 'f' },
    { "SYS_OPENDIR", "syscall(30, path)", "30: open a directory for reading. Returns a dirfd. Pair with SYS_READDIR/SYS_CLOSEDIR.",
      "long ${1:dfd} = syscall(30, (long)\"${2:/}\", 0, 0);   /* SYS_OPENDIR */$0", 'f' },
    { "SYS_READDIR", "syscall(31, dfd, ent)", "31: read one directory entry into ent. Returns >0 with an entry, 0 at end.",
      "syscall(31, ${1:dfd}, (long)${2:&ent}, 0);   /* SYS_READDIR */$0", 'f' },
    { "SYS_CLOSEDIR", "syscall(32, dfd)", "32: close a directory handle.",
      "syscall(32, ${1:dfd}, 0, 0);   /* SYS_CLOSEDIR */$0", 'f' },
    { "SYS_MAP_FILE", "syscall(17, path, &a, &n)", "17: map an initrd file zero-copy into userspace; fills addr and size out-pointers.",
      "long ${1:addr} = 0, ${2:size} = 0;\nsyscall(17, (long)\"${3:/file}\", (long)&${1:addr}, (long)&${2:size});   /* SYS_MAP_FILE */$0", 'f' },
    { "SYS_PERSIST_READ", "syscall(94, name, buf, cap)", "94: read a named diskfs file into buf (survives reboot). Returns bytes read.",
      "long ${1:got} = syscall(94, (long)\"${2:save\\0}\", (long)${3:buf}, ${4:cap});   /* SYS_PERSIST_READ */$0", 'f' },
    { "SYS_PERSIST_WRITE", "syscall(95, name, buf, len)", "95: write buf to a named diskfs file (persists across reboot). Returns bytes written.",
      "syscall(95, (long)\"${1:save}\", (long)${2:buf}, ${3:len});   /* SYS_PERSIST_WRITE */$0", 'f' },
    { "SYS_FSYNC", "syscall(86, fd)", "86: flush a file's data to storage.",
      "syscall(86, ${1:fd}, 0, 0);   /* SYS_FSYNC */$0", 'f' },

    /* ===================================================================== *
     *  MEMORY / FRAMEBUFFER / TIME  (kind 'f')
     * ===================================================================== */
    { "SYS_MMAP", "syscall(37, hint,len,prot)", "37: anonymous memory map (big buffers). 4th arg flags is host-only via builtin. Returns the address.",
      "long ${1:p} = syscall(37, 0, ${2:65536}, ${3:3});   /* SYS_MMAP len, prot RW; flags=arg4 host-only */$0", 'f' },
    { "SYS_MUNMAP", "syscall(38, addr, len)", "38: unmap a previous SYS_MMAP region.",
      "syscall(38, ${1:p}, ${2:len}, 0);   /* SYS_MUNMAP */$0", 'f' },
    { "SYS_FB_ACQUIRE", "syscall(39, &fbinfo)", "39: map the screen framebuffer into this process. Fills {u64 vaddr; u32 w,h,pitch,bpp}=24 bytes. See os_fb_open for the on-device-safe decode.",
      "long ${1:fbi}[3];\nsyscall(39, (long)${1:fbi}, 0, 0);   /* SYS_FB_ACQUIRE */\nchar* ${2:fb} = (char*)${1:fbi}[0];\nlong ${3:pitch} = ${1:fbi}[2] & 0xffffffff;$0", 'f' },
    { "SYS_GET_TICKS_MS", "syscall(40)", "40: milliseconds since boot (monotonic). The basis for timers/animation/FPS pacing.",
      "long ${1:now} = syscall(40, 0, 0, 0);   /* SYS_GET_TICKS_MS */$0", 'f' },
    { "SYS_TIME", "syscall(41)", "41: wall-clock seconds since the Unix epoch (RTC).",
      "long ${1:t} = syscall(41, 0, 0, 0);   /* SYS_TIME (epoch seconds) */$0", 'f' },
    { "SYS_GETTIME", "syscall(42, &rtc)", "42: fill a broken-down time struct (year/mon/day/h/m/s) from the RTC.",
      "syscall(42, (long)${1:&rtc}, 0, 0);   /* SYS_GETTIME */$0", 'f' },

    /* ===================================================================== *
     *  INPUT / DESKTOP / AUDIO  ("compositor tools")  (kind 'f')
     * ===================================================================== */
    { "SYS_READ_EVENT", "syscall(14)", "14: in THIS build, returns ONE keyboard char from the PS/2 buffer (0 if none); the pointer arg is ignored. Mouse/pointer events come via the compositor, not here.",
      "long ${1:c} = syscall(14, 0, 0, 0);   /* SYS_READ_EVENT -> key char, 0 if none */\nif (${1:c}) {\n    $0\n}", 'f' },
    { "SYS_NOTIFY", "syscall(65, buf, len)", "65: post a desktop toast. buf = \"title\\0body\\0\", len = total bytes incl both NULs. Fire-and-forget.",
      "char ${1:msg}[64];\n/* fill msg = title \\0 body \\0, set len */\nsyscall(65, (long)${1:msg}, ${2:len}, 0);   /* SYS_NOTIFY */$0", 'f' },
    { "SYS_NOTIFY_POLL", "syscall(66, buf, max)", "66: dequeue one pending notification into buf (\"title\\0body\\0\"). Returns bytes, 0 if empty.",
      "long ${1:n} = syscall(66, (long)${2:buf}, ${3:max}, 0);   /* SYS_NOTIFY_POLL */$0", 'f' },
    { "SYS_CLIP_SET", "syscall(63, buf, len)", "63: write len bytes to the system clipboard. Returns 0 or <0.",
      "syscall(63, (long)${1:buf}, ${2:len}, 0);   /* SYS_CLIP_SET */$0", 'f' },
    { "SYS_CLIP_GET", "syscall(64, buf, max)", "64: read the clipboard into buf (up to max). Returns bytes copied.",
      "long ${1:n} = syscall(64, (long)${2:buf}, ${3:max}, 0);   /* SYS_CLIP_GET */$0", 'f' },
    { "SYS_BEEP", "syscall(45, freq, ms)", "45: play a tone of freq Hz for ms via PC speaker/HDA. Returns 0, or <0 if no audio.",
      "syscall(45, ${1:880}, ${2:200}, 0);   /* SYS_BEEP freq,ms */$0", 'f' },
    { "SYS_RECOVERY_OVERLAY", "syscall(88, mode, ms)", "88: draw the self-heal fluid-circle recovery animation for ms. mode selects the style.",
      "syscall(88, ${1:1}, ${2:1500}, 0);   /* SYS_RECOVERY_OVERLAY */$0", 'f' },

    /* ===================================================================== *
     *  ENTROPY / POWER / SYSTEM  (kind 'f')
     * ===================================================================== */
    { "SYS_RANDOM", "syscall(43, buf, len)", "43: fill buf with len cryptographically-random bytes (CSPRNG). Returns bytes written.",
      "char ${1:rnd}[16];\nsyscall(43, (long)${1:rnd}, sizeof ${1:rnd}, 0);   /* SYS_RANDOM */$0", 'f' },
    { "SYS_SYSINFO", "syscall(62, &info)", "62: fill a struct with memory/uptime/process-count totals.",
      "syscall(62, (long)${1:&info}, 0, 0);   /* SYS_SYSINFO */$0", 'f' },
    { "SYS_BATTERY", "syscall(93, &out)", "93: fill 4 bytes {present, state, percent, ac} from the embedded controller.",
      "char ${1:bat}[4];\nsyscall(93, (long)${1:bat}, 0, 0);   /* SYS_BATTERY {present,state,percent,ac} */$0", 'f' },
    { "SYS_PERF_REPORT", "syscall(72)", "72: dump kernel performance counters to the serial log.",
      "syscall(72, 0, 0, 0);   /* SYS_PERF_REPORT */$0", 'f' },
    { "SYS_POWEROFF", "syscall(46)", "46: ACPI S5 power off. Does not return on success.",
      "syscall(46, 0, 0, 0);   /* SYS_POWEROFF */$0", 'f' },
    { "SYS_REBOOT", "syscall(47)", "47: reboot the machine. Does not return on success.",
      "syscall(47, 0, 0, 0);   /* SYS_REBOOT */$0", 'f' },

    /* ===================================================================== *
     *  SOCKETS / NET  (kind 'f')  -- gracefully ENOSYS when the NIC is down
     * ===================================================================== */
    { "SYS_SOCKET", "syscall(51, type)", "51: create a socket. type 1=TCP, 2=UDP, 3=RAW. Returns an fd or <0 (-ENOSYS if no NIC).",
      "long ${1:s} = syscall(51, ${2:1}, 0, 0);   /* SYS_SOCKET 1=TCP 2=UDP */$0", 'f' },
    { "SYS_CONNECT", "syscall(52, fd, ip, port)", "52: connect a TCP socket to ip:port (ip host-order, e.g. (10<<24)|(0<<16)|(2<<8)|2). Returns 0/<0.",
      "syscall(52, ${1:s}, ${2:((10<<24)|(0<<16)|(2<<8)|2)}, ${3:80});   /* SYS_CONNECT ip,port */$0", 'f' },
    { "SYS_SEND", "syscall(53, fd, buf, len)", "53: send len bytes on a connected socket. Returns bytes sent.",
      "syscall(53, ${1:s}, (long)${2:buf}, ${3:len});   /* SYS_SEND */$0", 'f' },
    { "SYS_RECV", "syscall(54, fd, buf, len)", "54: receive up to len bytes (non-blocking). Returns bytes, -EAGAIN if none.",
      "long ${1:n} = syscall(54, ${2:s}, (long)${3:buf}, ${4:len});   /* SYS_RECV */$0", 'f' },
    { "SYS_CLOSE_SK", "syscall(55, fd)", "55: close a SOCKET fd (distinct from SYS_CLOSE for files).",
      "syscall(55, ${1:s}, 0, 0);   /* SYS_CLOSE_SK */$0", 'f' },
    { "SYS_SENDTO", "syscall(56, fd, buf, len)", "56: send a UDP datagram. The dest ip/port need args 4/5 -> [host build: needs >3 args].",
      "syscall(56, ${1:s}, (long)${2:buf}, ${3:len});   /* SYS_SENDTO -- dest ip,port are arg4/5 (host) */$0", 'f' },
    { "SYS_BIND", "syscall(76, fd, port)", "76: bind a socket to a local port (server side).",
      "syscall(76, ${1:s}, ${2:8080}, 0);   /* SYS_BIND */$0", 'f' },
    { "SYS_LISTEN", "syscall(77, fd, backlog)", "77: mark a bound socket passive (accepts connections).",
      "syscall(77, ${1:s}, ${2:8}, 0);   /* SYS_LISTEN */$0", 'f' },
    { "SYS_ACCEPT", "syscall(78, fd)", "78: accept one incoming connection. Returns a new connected socket fd.",
      "long ${1:c} = syscall(78, ${2:s}, 0, 0);   /* SYS_ACCEPT */$0", 'f' },
    { "SYS_NET_INFO", "syscall(59, &info)", "59: fill net_info_t (ifname, mac, ip, mask, gw, dns, link). The link-up probe.",
      "syscall(59, (long)${1:&info}, 0, 0);   /* SYS_NET_INFO */$0", 'f' },

    /* ===================================================================== *
     *  THREADS / SYNC / EVENTS  (kind 'f')  -- mostly host-only via builtin
     * ===================================================================== */
    { "SYS_FUTEX", "syscall(70, &w, op, val)", "70: fast userspace mutex. op 0=WAIT (sleep while *w==val), 1=WAKE val waiters.",
      "syscall(70, (long)&${1:w}, ${2:0}, ${3:val});   /* SYS_FUTEX 0=wait 1=wake */$0", 'f' },
    { "SYS_THREAD_CREATE", "syscall(79, entry, arg, stk)", "79: start a thread sharing this address space. The stack pointer is arg3. Returns a tid.",
      "long ${1:tid} = syscall(79, (long)${2:entry}, (long)${3:arg}, (long)${4:stack_top});   /* SYS_THREAD_CREATE */$0", 'f' },
    { "SYS_THREAD_JOIN", "syscall(81, tid, &ret)", "81: block until thread tid exits; copies its return value out.",
      "long ${1:rv} = 0;\nsyscall(81, ${2:tid}, (long)&${1:rv}, 0);   /* SYS_THREAD_JOIN */$0", 'f' },
    { "SYS_EPOLL_CREATE", "syscall(73, hint)", "73: create an epoll instance for event-driven I/O. Returns an epfd.",
      "long ${1:ep} = syscall(73, ${2:16}, 0, 0);   /* SYS_EPOLL_CREATE */$0", 'f' },
    { "SYS_EPOLL_WAIT", "syscall(75, epfd, evs, max)", "75: block for ready events (4th arg timeout_ms is host-only). Returns event count.",
      "long ${1:n} = syscall(75, ${2:ep}, (long)${3:evs}, ${4:max});   /* SYS_EPOLL_WAIT; timeout=arg4 host */$0", 'f' },

    /* ===================================================================== *
     *  TYPED-AGENT CHANNEL RAIL  (kind 'f')  -- CHANNEL-0 / AGENT-RPC-0
     * ===================================================================== */
    { "SYS_CH_CREATE", "syscall(96, kind, cap)", "96: create a shared-ring channel; returns a handle. The in-OS typed-agent rail.",
      "long ${1:ch} = syscall(96, ${2:0}, ${3:4096}, 0);   /* SYS_CH_CREATE */$0", 'f' },
    { "SYS_CH_WRITE", "syscall(97, ch, buf, len)", "97: write bytes to a channel handle. Returns bytes written.",
      "syscall(97, ${1:ch}, (long)${2:buf}, ${3:len});   /* SYS_CH_WRITE */$0", 'f' },
    { "SYS_CH_READ", "syscall(98, ch, buf, len)", "98: read bytes from a channel handle. Returns bytes read.",
      "long ${1:n} = syscall(98, ${2:ch}, (long)${3:buf}, ${4:len});   /* SYS_CH_READ */$0", 'f' },
    { "SYS_CH_WAIT", "syscall(99, ch, mask)", "99: poll a channel for READABLE/WRITABLE/CLOSED readiness.",
      "long ${1:rdy} = syscall(99, ${2:ch}, ${3:1}, 0);   /* SYS_CH_WAIT */$0", 'f' },
    { "SYS_CH_CLOSE", "syscall(100, ch)", "100: close/release a channel handle.",
      "syscall(100, ${1:ch}, 0, 0);   /* SYS_CH_CLOSE */$0", 'f' },

    /* ===================================================================== *
     *  COMPOSITOR / WINDOWING  -- the SysV-IPC "Wayland-lite" path
     *  (kind 'm'/'s')  [host build: msgsnd/msgrcv need 4-5 args]
     * ===================================================================== */
    { "SYS_SHMGET", "syscall(18, key, size, flg)", "18: create/get a shared-memory segment. key 0=IPC_PRIVATE, flg IPC_CREAT|0666=0x3B6. Returns shmid.",
      "long ${1:shmid} = syscall(18, 0, ${2:w*h*4}, 0x3B6);   /* SYS_SHMGET IPC_PRIVATE|CREAT|0666 */$0", 'f' },
    { "SYS_SHMAT", "syscall(19, shmid, 0, 0)", "19: attach a shm segment into this address space. Returns the mapped address.",
      "long ${1:px} = syscall(19, ${2:shmid}, 0, 0);   /* SYS_SHMAT -> ARGB32 pixel buffer */$0", 'f' },
    { "SYS_SHMDT", "syscall(20, addr)", "20: detach a previously attached shm segment.",
      "syscall(20, ${1:px}, 0, 0);   /* SYS_SHMDT */$0", 'f' },
    { "SYS_MSGGET", "syscall(22, key, flg)", "22: create/get a SysV message queue. Compositor inbox key = 0x434F4D50.",
      "long ${1:q} = syscall(22, ${2:0x434F4D50}, 0x3B6, 0);   /* SYS_MSGGET (WL inbox) */$0", 'f' },
    { "SYS_MSGSND", "msgsnd(q,msg,sz,flg)", "23: send a message (1st field is long mtype). Needs 4 args -> [host build only: the 3-arg syscall() builtin can't pass msgflg].",
      "/* [host build] msgsnd(q, &m, sizeof(m)-sizeof(long), 0); needs an r10 helper */$0", 'f' },
    { "SYS_MSGRCV", "msgrcv(q,msg,sz,typ,flg)", "24: receive a message of type typ. Needs 5 args -> [host build only: not reachable via the 3-arg builtin].",
      "/* [host build] msgrcv(q, &m, sizeof(m)-sizeof(long), WL_EVT, 0); needs r10/r8 helper */$0", 'f' },
    { "WL_PROTO", "wl_proto.h keys", "Compositor protocol (userspace/include/wl_proto.h): inbox key 0x434F4D50; reply key 0x52000000+pid; reqs CREATE=1 COMMIT=2 DESTROY=3; evts CREATED=1 POINTER=2 KEY=3.",
      "/* WL: inbox=0x434F4D50  reply=0x52000000+pid\n   REQ_CREATE 1  REQ_COMMIT 2  REQ_DESTROY 3\n   EVT_CREATED 1  EVT_POINTER 2  EVT_KEY 3 */$0", 'm' },
    { "os_window_open", "wl_client window (host)", "Open a real compositor WINDOW. [host build: link userspace/lib/wl/wl_client.c + bitfont.c]. The on-device path is the full-screen framebuffer (os_fb_open).",
      "/* [host build] link wl_client.c */\nif (wl_connect() != 0) return 1;\nwl_window* ${1:win} = wl_create_window(${2:400}, ${3:260}, \"${4:title}\");\nfor (;;) {\n    /* draw ARGB32 into ${1:win}->pixels (stride ${1:win}->stride bytes/row) */\n    $0\n    wl_commit(${1:win});\n    int k,a,b,c;\n    while (wl_poll_event(${1:win}, &k, &a, &b, &c)) { /* k=WL_EVENT_* */ }\n}", 's' },

    /* ===================================================================== *
     *  HEADLINE "COMPOSITOR TOOLS"  (kind 's')  -- on-device-SAFE, copy-ready
     *  Full-screen framebuffer graphics through the syscall() builtin. These
     *  obey the on-device 8-byte-int / char*-pixel rules. NO header, NO libc.
     * ===================================================================== */
    { "os_fb_open", "acquire + decode FB", "Map the screen and decode the 24-byte fb_acquire_t by hand (its u32 fields can't be a struct on-device). Gives fb base + width/height/pitch.",
      "long ${1:fbi}[3];                    /* {vaddr | w:h | pitch:bpp} = 24 bytes */\n"
      "syscall(39, (long)${1:fbi}, 0, 0);   /* SYS_FB_ACQUIRE */\n"
      "char* ${2:fb}     = (char*)${1:fbi}[0];\n"
      "long  ${3:width}  =  ${1:fbi}[1]        & 0xffffffff;\n"
      "long  ${4:height} = (${1:fbi}[1] >> 32) & 0xffffffff;\n"
      "long  ${5:pitch}  =  ${1:fbi}[2]        & 0xffffffff;   /* bytes per row */\n$0", 's' },
    { "os_putpixel", "putpx(fb,pitch,x,y,c)", "Write ONE ARGB32 pixel the on-device-correct way: char* base + 4 byte stores (an unsigned int* would stride 8 bytes here).",
      "void ${1:putpx}(char* fb, long pitch, long x, long y, unsigned long c) {\n"
      "    char* p = fb + y * pitch + x * 4;\n"
      "    p[0] = (char)c;          /* B */\n"
      "    p[1] = (char)(c >> 8);   /* G */\n"
      "    p[2] = (char)(c >> 16);  /* R */\n"
      "    p[3] = (char)(c >> 24);  /* A */\n}$0", 's' },
    { "os_fillrect", "fillrect(fb,pitch,..,c)", "Fill a w x h rectangle at (x,y) with ARGB color c. Pure char* loop -- safe on-device, fast enough for UI.",
      "void ${1:fillrect}(char* fb, long pitch, long x, long y, long w, long h, unsigned long c) {\n"
      "    for (long j = 0; j < h; j++)\n"
      "        for (long i = 0; i < w; i++) {\n"
      "            char* p = fb + (y + j) * pitch + (x + i) * 4;\n"
      "            p[0] = (char)c; p[1] = (char)(c >> 8);\n"
      "            p[2] = (char)(c >> 16); p[3] = (char)(c >> 24);\n"
      "        }\n}$0", 's' },
    { "os_clear", "clear screen to color", "Paint the whole framebuffer one color. Call os_fb_open first for fb/width/height/pitch.",
      "for (long ${1:y} = 0; ${1:y} < ${2:height}; ${1:y}++)\n"
      "    for (long ${3:x} = 0; ${3:x} < ${4:width}; ${3:x}++) {\n"
      "        char* p = ${5:fb} + ${1:y} * ${6:pitch} + ${3:x} * 4;\n"
      "        p[0] = (char)${7:0x1E}; p[1] = (char)${8:0x1E}; p[2] = (char)${9:0x2E}; p[3] = (char)0xFF;\n"
      "    }$0", 's' },
    { "os_app", "full-screen app skeleton", "A complete on-device graphical app: acquire FB, then loop {read key, draw, yield}. Quit on 'q'. THE compositor-tools starting point.",
      "int main(void) {\n"
      "    long fbi[3];\n"
      "    syscall(39, (long)fbi, 0, 0);            /* SYS_FB_ACQUIRE */\n"
      "    char* fb    = (char*)fbi[0];\n"
      "    long  width = fbi[1] & 0xffffffff;\n"
      "    long  height= (fbi[1] >> 32) & 0xffffffff;\n"
      "    long  pitch = fbi[2] & 0xffffffff;\n"
      "    for (;;) {\n"
      "        long c = syscall(14, 0, 0, 0);       /* SYS_READ_EVENT -> key char */\n"
      "        if (c == 'q') break;\n"
      "        /* --- draw a frame into fb (use os_fillrect/os_putpixel) --- */\n"
      "        $0\n"
      "        syscall(15, 0, 0, 0);                /* SYS_YIELD */\n"
      "    }\n"
      "    return 0;\n}", 's' },
    { "os_timer", "delay_ms(ms)", "Spin a wall-clock delay by polling SYS_GET_TICKS_MS and yielding -- the safe way to wait without a blocking sleep.",
      "void ${1:delay_ms}(long ms) {\n"
      "    long end = syscall(40, 0, 0, 0) + ms;    /* SYS_GET_TICKS_MS */\n"
      "    while (syscall(40, 0, 0, 0) < end)\n"
      "        syscall(15, 0, 0, 0);                /* SYS_YIELD */\n}$0", 's' },
    { "os_blit_text", "text needs bitfont", "There is NO kernel glyph/text syscall. On-device, draw your own blocks/bitmaps; for real glyphs link userspace/lib/font/bitfont.c (host build) and call font_draw_string(buf,stride,w,h,x,y,str,argb).",
      "/* on-device: no font syscall -- draw blocks, or [host build] link bitfont.c:\n"
      "   font_draw_string(${1:fb}, stride_px, w, h, ${2:x}, ${3:y}, \"${4:text}\", ${5:0xFFEEEEFF}); */$0", 's' },
};
#define IDE_DICT_OS_COUNT ((int)(sizeof(IDE_DICT_OS) / sizeof(IDE_DICT_OS[0])))

#endif /* IDE_DICT_OS_H */
