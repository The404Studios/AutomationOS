// pureburn -- the EXACT adversarial scenario for SCHED-FAIRNESS-0.
//
// A ring-3 task that NEVER makes a syscall: no yield, no sleep, no heartbeat, no
// exit. It just spins. Under the timer-preemptive scheduler it is therefore
// ALWAYS RESUME_IRETQ and is resumed from the IRQ path every quantum; it offers
// NO cooperative-switch boundary. The old schedule_from_irq could only iretq into
// a RESUME_IRETQ successor, so a RESUME_CRETURN task (one woken from sleep/wait/
// futex/poll) that the IRQ path picked got re-enqueued-but-never-dispatched ->
// starved forever behind even a single pureburn. (cpuburn hides the bug because
// its periodic heartbeat write() is a cooperative handoff; pureburn does not.)
//
// Used by fair_smoke.sh: alongside a sleeper (sbin/fairwake) under a minimal,
// desktop-less boot so NOTHING else provides a cooperative dispatch — if the
// sleeper still wakes, the IRQ-path fairness fix works.
void _start(void) {
    volatile unsigned long x = 0;
    for (;;) { x = x + 1; }   // pure compute, never a syscall
}
