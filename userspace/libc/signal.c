// userspace/libc/signal.c - Signal handling implementation

#include "signal.h"
#include "syscall.h"
#include "string.h"

// Global signal handler table
static sighandler_t signal_handlers[_NSIG];

// Signal names for strsignal
const char* const sys_siglist[_NSIG] = {
    [0] = "Signal 0",
    [SIGHUP] = "Hangup",
    [SIGINT] = "Interrupt",
    [SIGQUIT] = "Quit",
    [SIGILL] = "Illegal instruction",
    [SIGTRAP] = "Trace/breakpoint trap",
    [SIGABRT] = "Aborted",
    [SIGBUS] = "Bus error",
    [SIGFPE] = "Floating point exception",
    [SIGKILL] = "Killed",
    [SIGUSR1] = "User defined signal 1",
    [SIGSEGV] = "Segmentation fault",
    [SIGUSR2] = "User defined signal 2",
    [SIGPIPE] = "Broken pipe",
    [SIGALRM] = "Alarm clock",
    [SIGTERM] = "Terminated",
    [SIGSTKFLT] = "Stack fault",
    [SIGCHLD] = "Child exited",
    [SIGCONT] = "Continued",
    [SIGSTOP] = "Stopped (signal)",
    [SIGTSTP] = "Stopped",
    [SIGTTIN] = "Stopped (tty input)",
    [SIGTTOU] = "Stopped (tty output)",
    [SIGURG] = "Urgent I/O condition",
    [SIGXCPU] = "CPU time limit exceeded",
    [SIGXFSZ] = "File size limit exceeded",
    [SIGVTALRM] = "Virtual timer expired",
    [SIGPROF] = "Profiling timer expired",
    [SIGWINCH] = "Window changed",
    [SIGIO] = "I/O possible",
    [SIGPWR] = "Power failure",
    [SIGSYS] = "Bad system call"
};

// ============================================================================
// SIGNAL HANDLER INSTALLATION
// ============================================================================

// Simple signal handler installation
sighandler_t signal(int signum, sighandler_t handler) {
    if (signum < 1 || signum >= _NSIG) {
        return SIG_ERR;
    }

    // Can't catch SIGKILL or SIGSTOP
    if (signum == SIGKILL || signum == SIGSTOP) {
        return SIG_ERR;
    }

    sighandler_t old_handler = signal_handlers[signum];
    signal_handlers[signum] = handler;

    // In a real implementation, this would use a syscall to register
    // the signal handler with the kernel
    // For now, this is just a userspace-only implementation

    return old_handler;
}

// Advanced signal handler installation
int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact) {
    if (signum < 1 || signum >= _NSIG) {
        return -1;
    }

    // Can't catch SIGKILL or SIGSTOP
    if (signum == SIGKILL || signum == SIGSTOP) {
        return -1;
    }

    // Return old action if requested
    if (oldact) {
        oldact->sa_handler = signal_handlers[signum];
        oldact->sa_mask = 0;
        oldact->sa_flags = 0;
        oldact->sa_restorer = NULL;
    }

    // Set new action if provided
    if (act) {
        signal_handlers[signum] = act->sa_handler;
        // In a real implementation, would also set sa_mask, sa_flags, etc.
        // through a syscall
    }

    return 0;
}

// ============================================================================
// SENDING SIGNALS
// ============================================================================

// Note: kill() is implemented in syscall.c as a direct syscall wrapper

// Send signal to current process
int raise(int sig) {
    if (sig < 0 || sig >= _NSIG) {
        return -1;
    }

    // Call the signal handler directly (simplified implementation)
    if (signal_handlers[sig] && signal_handlers[sig] != SIG_DFL &&
        signal_handlers[sig] != SIG_IGN) {
        signal_handlers[sig](sig);
        return 0;
    }

    // For default handlers, just return success
    // In a real implementation, default handlers would terminate the process,
    // dump core, etc.
    return 0;
}

// ============================================================================
// SIGNAL SET MANIPULATION
// ============================================================================

// Clear all signals from set
int sigemptyset(sigset_t* set) {
    if (!set) {
        return -1;
    }

    *set = 0;
    return 0;
}

// Add all signals to set
int sigfillset(sigset_t* set) {
    if (!set) {
        return -1;
    }

    *set = ~0UL;
    return 0;
}

// Add signal to set
int sigaddset(sigset_t* set, int signum) {
    if (!set || signum < 1 || signum >= _NSIG) {
        return -1;
    }

    *set |= (1UL << (signum - 1));
    return 0;
}

// Remove signal from set
int sigdelset(sigset_t* set, int signum) {
    if (!set || signum < 1 || signum >= _NSIG) {
        return -1;
    }

    *set &= ~(1UL << (signum - 1));
    return 0;
}

// Test if signal is in set
int sigismember(const sigset_t* set, int signum) {
    if (!set || signum < 1 || signum >= _NSIG) {
        return -1;
    }

    return (*set & (1UL << (signum - 1))) ? 1 : 0;
}

// ============================================================================
// SIGNAL MASK MANIPULATION
// ============================================================================

// Change signal mask
int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    // Stub implementation - would need kernel support
    // For now, just return success
    (void)how;
    (void)set;

    if (oldset) {
        *oldset = 0;
    }

    return 0;
}

// Thread-specific signal mask (same as sigprocmask for single-threaded)
int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset) {
    return sigprocmask(how, set, oldset);
}

// ============================================================================
// WAITING FOR SIGNALS
// ============================================================================

// Suspend until signal is received
int sigsuspend(const sigset_t* mask) {
    // Stub implementation
    (void)mask;
    return -1;
}

// Wait for signal in set
int sigwait(const sigset_t* set, int* sig) {
    if (!set || !sig) {
        return -1;
    }

    // Stub implementation
    return -1;
}

// Wait for signal with info
int sigwaitinfo(const sigset_t* set, siginfo_t* info) {
    if (!set) {
        return -1;
    }

    // Stub implementation
    (void)info;
    return -1;
}

// Wait for signal with timeout
int sigtimedwait(const sigset_t* set, siginfo_t* info,
                 const struct timespec* timeout) {
    if (!set) {
        return -1;
    }

    // Stub implementation
    (void)info;
    (void)timeout;
    return -1;
}

// ============================================================================
// SIGNAL STACK
// ============================================================================

// Set alternate signal stack
int sigaltstack(const stack_t* ss, stack_t* old_ss) {
    // Stub implementation
    (void)ss;

    if (old_ss) {
        old_ss->ss_sp = NULL;
        old_ss->ss_flags = SS_DISABLE;
        old_ss->ss_size = 0;
    }

    return 0;
}

// ============================================================================
// PENDING SIGNALS
// ============================================================================

// Get pending signals
int sigpending(sigset_t* set) {
    if (!set) {
        return -1;
    }

    // Stub implementation - no pending signals
    *set = 0;
    return 0;
}

// ============================================================================
// SIGNAL DESCRIPTION
// ============================================================================

// Get signal description string
char* strsignal(int sig) {
    static char unknown_sig[32];

    if (sig < 0 || sig >= _NSIG || !sys_siglist[sig]) {
        // Format unknown signal number
        char* p = unknown_sig;
        const char* prefix = "Unknown signal ";
        while (*prefix) {
            *p++ = *prefix++;
        }

        // Simple integer to string conversion
        if (sig < 0) {
            *p++ = '-';
            sig = -sig;
        }

        if (sig == 0) {
            *p++ = '0';
        } else {
            char digits[12];
            int i = 0;
            while (sig > 0) {
                digits[i++] = '0' + (sig % 10);
                sig /= 10;
            }
            while (i > 0) {
                *p++ = digits[--i];
            }
        }
        *p = '\0';

        return unknown_sig;
    }

    return (char*)sys_siglist[sig];
}
