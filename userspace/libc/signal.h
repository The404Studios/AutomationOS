// userspace/libc/signal.h - Signal handling

#ifndef SIGNAL_H
#define SIGNAL_H

#ifndef NULL
#define NULL ((void*)0)
#endif

typedef unsigned long size_t;   /* used by the sigaltstack ss_size field */
typedef int sig_atomic_t;
typedef unsigned long sigset_t;

/* AUDIT FIX (gap-audit-40): file-scope forward decl so sigtimedwait()'s prototype
 * (.h) and definition (.c) refer to the SAME struct timespec instead of two
 * parameter-scope structs -> resolves the pre-existing conflicting-types error.
 * The full definition lives in time.h. */
struct timespec;

// Signal numbers (POSIX)
#define SIGHUP    1   // Hangup
#define SIGINT    2   // Interrupt (Ctrl+C)
#define SIGQUIT   3   // Quit
#define SIGILL    4   // Illegal instruction
#define SIGTRAP   5   // Trace/breakpoint trap
#define SIGABRT   6   // Abort
#define SIGBUS    7   // Bus error
#define SIGFPE    8   // Floating point exception
#define SIGKILL   9   // Kill (cannot be caught)
#define SIGUSR1   10  // User-defined signal 1
#define SIGSEGV   11  // Segmentation fault
#define SIGUSR2   12  // User-defined signal 2
#define SIGPIPE   13  // Broken pipe
#define SIGALRM   14  // Alarm clock
#define SIGTERM   15  // Termination
#define SIGSTKFLT 16  // Stack fault
#define SIGCHLD   17  // Child status changed
#define SIGCONT   18  // Continue
#define SIGSTOP   19  // Stop (cannot be caught)
#define SIGTSTP   20  // Terminal stop
#define SIGTTIN   21  // Background read from tty
#define SIGTTOU   22  // Background write to tty
#define SIGURG    23  // Urgent data on socket
#define SIGXCPU   24  // CPU time limit exceeded
#define SIGXFSZ   25  // File size limit exceeded
#define SIGVTALRM 26  // Virtual timer expired
#define SIGPROF   27  // Profiling timer expired
#define SIGWINCH  28  // Window size change
#define SIGIO     29  // I/O possible
#define SIGPWR    30  // Power failure
#define SIGSYS    31  // Bad system call

#define _NSIG     32  // Number of signals

// Special signal handlers
#define SIG_DFL ((void (*)(int))0)   // Default action
#define SIG_IGN ((void (*)(int))1)   // Ignore signal
#define SIG_ERR ((void (*)(int))-1)  // Error return

// Signal action flags
#define SA_NOCLDSTOP  1   // Don't send SIGCHLD when children stop
#define SA_NOCLDWAIT  2   // Don't create zombie on child death
#define SA_SIGINFO    4   // Invoke signal-catching function with three arguments
#define SA_ONSTACK    0x08000000  // Use signal stack
#define SA_RESTART    0x10000000  // Restart syscall on signal return
#define SA_NODEFER    0x40000000  // Don't block signal while handling
#define SA_RESETHAND  0x80000000  // Reset handler to SIG_DFL on entry

// Signal set operations
#define SIG_BLOCK     0   // Block signals
#define SIG_UNBLOCK   1   // Unblock signals
#define SIG_SETMASK   2   // Set signal mask

// siginfo_t structure
typedef struct {
    int si_signo;     // Signal number
    int si_code;      // Signal code
    int si_errno;     // Error number
    int si_pid;       // Sending process ID
    unsigned int si_uid;  // Real user ID of sending process
    void* si_addr;    // Address of faulting instruction
    int si_status;    // Exit value or signal
    long si_band;     // Band event
} siginfo_t;

// Signal action structure
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t*, void*);
    };
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

// Stack for signal handling
typedef struct {
    void* ss_sp;      // Stack pointer
    int ss_flags;     // Flags
    size_t ss_size;   // Stack size
} stack_t;

// Signal stack flags
#define SS_ONSTACK  1
#define SS_DISABLE  2

// Minimum signal stack size
#define MINSIGSTKSZ 2048
#define SIGSTKSZ    8192

// ============================================================================
// SIGNAL FUNCTIONS
// ============================================================================

// Signal handler installation (simple interface)
typedef void (*sighandler_t)(int);
sighandler_t signal(int signum, sighandler_t handler);

// Signal handler installation (advanced interface)
int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact);

// Send signal to process
int kill(int pid, int sig);

// Send signal to current process
int raise(int sig);

// Signal set manipulation
int sigemptyset(sigset_t* set);
int sigfillset(sigset_t* set);
int sigaddset(sigset_t* set, int signum);
int sigdelset(sigset_t* set, int signum);
int sigismember(const sigset_t* set, int signum);

// Signal mask manipulation
int sigprocmask(int how, const sigset_t* set, sigset_t* oldset);
int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset);

// Wait for signal
int sigsuspend(const sigset_t* mask);
int sigwait(const sigset_t* set, int* sig);
int sigwaitinfo(const sigset_t* set, siginfo_t* info);
int sigtimedwait(const sigset_t* set, siginfo_t* info,
                 const struct timespec* timeout);

// Signal stack
int sigaltstack(const stack_t* ss, stack_t* old_ss);

// Pending signals
int sigpending(sigset_t* set);

// Signal names
extern const char* const sys_siglist[_NSIG];
char* strsignal(int sig);

#endif
