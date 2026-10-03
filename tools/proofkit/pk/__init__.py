"""proofkit -- declarative proof scenarios for AutomationOS.

A scenario describes a PROCESS (build -> boot -> observe) and the ASSERTIONS that must hold at each stage.
The runner executes it, gathers artifacts (serial log, packet capture, screenshots, host-side service logs)
and evaluates every assertion, reporting PASS / FAIL / SKIP with the evidence that decided it.

Design rules (they exist because of mistakes made while building this OS):
  * An assertion must be able to FAIL. `--expect-fail ID` proves red-before-green: it exits 0 only if the
    named assertion fails, so a fix is only trusted if the test was shown to catch the bug first.
  * Stages depend on each other (`depends_on`). When stage N fails, later stages are SKIPPED and the report
    names the FIRST failing stage -- the root cause -- instead of a wall of downstream failures.
  * "Boot finished" is not "it works": assertions look at real effects (packets on the wire, pixels on the
    screen, bytes a host service received), not just at a log line the code prints about itself.
  * Kernel health is judged the way smoke_boot.sh judges it: a ring-3 fault the kernel CONTAINED is a note,
    a panic / triple fault / uncontained exception is a failure.
"""
__version__ = "1.0"
