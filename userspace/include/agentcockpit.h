/*
 * agentcockpit.h -- Cockpit <-> agentd status + control seam (AGENTCOCKPIT-0).
 * ===========================================================================
 *
 * A tiny shared-memory page that lets the GUI cockpit post a GOAL to the
 * OS-automation agent (agentd) and drive the human-in-the-loop safety controls
 * (Allow / Deny / STOP), while agentd writes its live step status back for the
 * cockpit to display. agentd runs as a headless loop and the cockpit is a
 * separate GUI app; there is no wl message between them, so this well-known
 * SysV SHM page is the seam (same pattern as the DOCK-DND drag handoff and the
 * SYNTHINPUT injection ring).
 *
 * Ownership: the COCKPIT creates + zeroes the page at mode 0600 (it owns the
 * page; it is the GUI the human sits in front of) and publishes the magic LAST.
 * agentd attaches LOOKUP-ONLY (shmget(KEY, SIZE, 0) -- NO IPC_CREAT) and never
 * creates, resizes, or owns the page. If the page is ABSENT (no cockpit
 * running, or magic mismatch) agentd behaves EXACTLY as it does today: it
 * writes no status, ignores all control fields, and runs the goal it was given
 * on its own argv. The seam is therefore purely additive -- attaching the
 * cockpit is the only thing that turns the status/control rail on.
 *
 * Single-writer-per-field discipline (so no locking is needed): every field
 * has exactly one writer. The CONTROL fields (goal_seq, stop, grant_full,
 * confirm, goal) are written ONLY by the cockpit and read-only to agentd; the
 * STATUS fields (state, step, run_seq, tool, args, last) are written ONLY by
 * agentd and read-only to the cockpit. Because no field has two writers there
 * is no torn-write race to lock against -- each side simply reads the other
 * side's fields and writes its own. The reader must not cache a field across
 * iterations (read it fresh each loop) so it observes the other side's edits.
 *
 * The `magic` field is the publish guard: the cockpit zeroes the whole page
 * FIRST and writes AGENTCOCKPIT_MAGIC LAST, so an agentd that races the
 * cockpit's init sees either an all-zero page (magic mismatch -> seam not
 * ready, run on argv as today) or a fully initialised one, never a half-built
 * contract.
 *
 * The goal handshake is sequence-numbered: the cockpit fills `goal[]` and then
 * bumps `goal_seq`; agentd notices goal_seq != run_seq, copies the goal, sets
 * run_seq = goal_seq, and starts executing. A bump while a run is in flight is
 * how the cockpit re-targets the agent.
 *
 * The `stop`, `grant_full`, and `confirm` fields are the human-in-the-loop
 * SAFETY controls (the model is hostile text; nothing it asks for runs without
 * a human gate):
 *   - `stop` is the kill switch: the cockpit sets it to 1 to ask the running
 *     agent to halt at the next safe point; agentd stops and reports
 *     AC_STATE_STOPPED.
 *   - When agentd reaches a CONFIRM-class tool it parks in AC_STATE_CONFIRM and
 *     publishes the pending tool/args; it then waits for the cockpit to write
 *     `confirm` = AC_CONFIRM_ALLOW (run it) or AC_CONFIRM_DENY (skip it). The
 *     cockpit clears `confirm` back to AC_CONFIRM_NONE once consumed.
 *   - `grant_full` = 1 is the session-wide auto-Allow: the cockpit sets it to
 *     let every CONFIRM-class tool through for this session without prompting
 *     (an explicit, revocable "I trust this run" -- still operator-only).
 */
#ifndef AGENTCOCKPIT_H
#define AGENTCOCKPIT_H

#define AGENTCOCKPIT_SHM_KEY  0x41434B50u   /* 'ACKP'                                */
#define AGENTCOCKPIT_SHM_SIZE 4096u         /* one page                              */
#define AGENTCOCKPIT_MAGIC    0x41434B01u   /* 'ACK\x01' -- cockpit (owner) publishes LAST */

/* agent run state (agentd writes) */
#define AC_STATE_IDLE     0
#define AC_STATE_RUNNING  1
#define AC_STATE_CONFIRM  2   /* awaiting the cockpit's Allow/Deny on the pending tool */
#define AC_STATE_DONE     3
#define AC_STATE_STOPPED  4

/* confirm decision (cockpit writes) */
#define AC_CONFIRM_NONE   0
#define AC_CONFIRM_ALLOW  1
#define AC_CONFIRM_DENY   2

typedef struct {
    unsigned int magic;        /* AGENTCOCKPIT_MAGIC once the cockpit has initialised it */
    /* --- control: cockpit WRITES, agentd READS --- */
    unsigned int goal_seq;     /* bumped by the cockpit each time a new goal is posted */
    unsigned int stop;         /* 1 = cockpit asked the running agent to STOP */
    unsigned int grant_full;   /* 1 = auto-ALLOW every CONFIRM-class tool this session */
    unsigned int confirm;      /* AC_CONFIRM_* : the cockpit's decision on the pending tool */
    char goal[256];            /* the goal text the cockpit posted */
    /* --- status: agentd WRITES, cockpit READS --- */
    unsigned int state;        /* AC_STATE_* */
    unsigned int step;         /* current step number (1-based) */
    unsigned int run_seq;      /* the goal_seq agentd is currently executing */
    char tool[32];             /* current or pending tool name */
    char args[256];            /* current or pending tool args (first 255 bytes) */
    char last[256];            /* last RESULT text, truncated */
} agentcockpit_shm_t;

#endif /* AGENTCOCKPIT_H */
