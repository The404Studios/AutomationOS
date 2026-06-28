/*
 * ide_semantic.c -- Semantic analyzer for the "Semantic LEGO Map" IDE.
 *
 * The parser (ide_parse.c) fills m->funcs[] / m->globals[] with names, params,
 * calls[], reads[], writes[] and per-global nreaders/nwriters. This stage turns
 * that raw shape into the LEGO model the views render: per-function PORTS,
 * plus -- for the focused function only -- connections, risks, recommended
 * actions, a runtime flow, and a single coherence score.
 *
 * Freestanding: no libc, no malloc, no stdio. All buffers are caller-owned
 * (inside Model). Helpers are static and prefixed sem_. Deterministic and
 * crash-proof: every loop is bounded by the M_MAX* caps in ide_model.h.
 */

#include "ide_model.h"
#include "ide_sys.h"
#include "ide_library.h"

/* ===========================================================================
 * Tiny string helpers (freestanding -- no <string.h>).
 * ===========================================================================*/

static int sem_strlen(const char* s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n] && n < 4096) n++;
    return n;
}

/* Bounded copy into a fixed buffer; always NUL-terminates. */
static void sem_cpy(char* d, const char* s, int cap)
{
    int i = 0;
    if (!d || cap <= 0) return;
    if (s) {
        for (; i < cap - 1 && s[i]; i++) d[i] = s[i];
    }
    d[i] = '\0';
}

/* Append src onto d (which is cap bytes) without overflowing. */
static void sem_cat(char* d, const char* s, int cap)
{
    int i, j = 0;
    if (!d || cap <= 0) return;
    i = sem_strlen(d);
    if (i >= cap - 1) { d[cap - 1] = '\0'; return; }
    if (s) {
        for (; i < cap - 1 && s[j]; i++, j++) d[i] = s[j];
    }
    d[i] = '\0';
}

static int sem_eq(const char* a, const char* b)
{
    int i = 0;
    if (a == b) return 1;
    if (!a || !b) return 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

/* Case-insensitive "does haystack contain needle?" (needle assumed lowercase). */
static int sem_icontains(const char* hay, const char* needle)
{
    int hl, nl, i, j;
    if (!hay || !needle) return 0;
    hl = sem_strlen(hay);
    nl = sem_strlen(needle);
    if (nl == 0) return 1;
    if (nl > hl) return 0;
    for (i = 0; i + nl <= hl; i++) {
        for (j = 0; j < nl; j++) {
            char c = hay[i + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != needle[j]) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

/* True if any of f->calls contains one of a NULL-terminated list of substrings. */
static int sem_calls_any(const Func* f, const char* const* keys)
{
    int i, k;
    if (!f) return 0;
    for (i = 0; i < f->ncalls && i < M_MAXCALLS; i++) {
        for (k = 0; keys[k]; k++) {
            if (sem_icontains(f->calls[i], keys[k])) return 1;
        }
    }
    return 0;
}

/* ===========================================================================
 * PORT construction.
 * ===========================================================================*/

static void sem_add_port(Func* f, const char* name, PortType type,
                         PortDir dir, int fit, PortStatus status)
{
    Port* p;
    if (!f) return;
    f->nports_true++;                       /* count EVERY intended port (incl. those */
    if (f->nports >= M_MAXPORTS) return;    /* dropped past the 16-slot store cap)    */
    p = &f->ports[f->nports++];
    sem_cpy(p->name, name, M_NAME);
    p->type   = type;
    p->dir    = dir;
    p->fit    = fit;
    p->status = status;
}

/* Does f have a port of the given type and status already? */
static int sem_has_port(const Func* f, PortType type, PortStatus status)
{
    int i;
    if (!f) return 0;
    for (i = 0; i < f->nports && i < M_MAXPORTS; i++) {
        if (f->ports[i].type == type && f->ports[i].status == status) return 1;
    }
    return 0;
}

/* Find a global by name; returns index or -1. */
static int sem_find_global(const Model* m, const char* name)
{
    int i;
    if (!m || !name) return -1;
    for (i = 0; i < m->nglobals && i < M_MAXGLOBALS; i++) {
        if (sem_eq(m->globals[i].name, name)) return i;
    }
    return -1;
}

/* True if a written global is shared mutable state (>1 writer across program). */
static int sem_is_shared_write(const Model* m, const char* gname)
{
    int gi = sem_find_global(m, gname);
    if (gi < 0) return 0;
    return m->globals[gi].nwriters > 1;
}

/*
 * Build ports for one function. Order is stable so port counts/fit are
 * deterministic across runs:
 *   inputs -> state reads -> state writes -> control(calls) -> absent gates.
 */
static void sem_build_ports(Model* m, Func* f)
{
    int i;
    int writes_shared = 0;

    f->nports = 0;
    f->nports_true = 0;                 /* reset the cap-independent true count   */
    f->wants_lifecycle = 0;            /* and the absent-gate intent (set below)  */
    f->wants_gate = 0;

    /* PORT_INPUT per parameter. fit walks 90..96 then holds at 96. */
    for (i = 0; i < f->nparams && i < M_MAXPARAMS; i++) {
        const char* nm = f->params[i].name[0] ? f->params[i].name
                                               : f->params[i].type;
        int fit = 90 + i; if (fit > 96) fit = 96;
        sem_add_port(f, nm, PORT_INPUT, DIR_IN, fit, PS_CONNECTED);
    }

    /* PORT_STATE_READ per read. fit 85..92. */
    for (i = 0; i < f->nreads && i < M_MAXREFS; i++) {
        int fit = 85 + (i % 8);            /* 85..92 */
        sem_add_port(f, f->reads[i], PORT_STATE_READ, DIR_IN, fit, PS_CONNECTED);
    }

    /* PORT_STATE_WRITE per write. fit 88..92. Track shared-write presence. */
    for (i = 0; i < f->nwrites && i < M_MAXREFS; i++) {
        int fit = 88 + (i % 5);            /* 88..92 */
        sem_add_port(f, f->writes[i], PORT_STATE_WRITE, DIR_OUT, fit, PS_CONNECTED);
        if (sem_is_shared_write(m, f->writes[i])) writes_shared = 1;
    }

    /* PORT_CONTROL per call (cap to remaining room implicitly via sem_add_port). */
    for (i = 0; i < f->ncalls && i < M_MAXCALLS; i++) {
        int fit = 80 + (i % 6);            /* 80..85 */
        sem_add_port(f, f->calls[i], PORT_CONTROL, DIR_OUT, fit, PS_CONNECTED);
    }

    /* ---- ABSENT ports via heuristics (only if shared mutable write) ---- */
    if (writes_shared) {
        static const char* const claim_keys[] =
            { "claim", "lock", "acquire", "gate", "reserve", 0 };
        static const char* const gate_keys[] =
            { "cooldown", "gate", "ready", "can_", 0 };

        /* Missing claim/lifecycle hook. Record the INTENT independently of
         * whether the port fits in the 16-slot array, so coherence + the
         * absent-gate risks/actions stay honest even when ports[] truncated. */
        if (!sem_calls_any(f, claim_keys)) {
            f->wants_lifecycle = 1;
            sem_add_port(f, "claim_slot", PORT_LIFECYCLE, DIR_OUT, 94, PS_ABSENT);
        }
        /* Missing cooldown/control gate. */
        if (!sem_calls_any(f, gate_keys)) {
            f->wants_gate = 1;
            sem_add_port(f, "cooldown_gate", PORT_CONTROL_GATE, DIR_OUT, 91, PS_ABSENT);
        }
    }
}

/* ===========================================================================
 * Connection / risk / action / flow builders for the FOCUS function.
 * ===========================================================================*/

static void sem_add_conn(Model* m, const char* from, const char* to,
                         PortType type, int compat, ConnStatus status)
{
    Conn* c;
    if (m->nconns >= M_MAXCONNS) return;
    c = &m->conns[m->nconns++];
    sem_cpy(c->from, from, M_NAME);
    sem_cpy(c->to,   to,   M_NAME);
    c->type   = type;
    c->compat = compat;
    c->status = status;
}

static void sem_add_risk(Model* m, const char* title, const char* risk, int score)
{
    Risk* r;
    if (m->nrisks >= M_MAXRISKS) return;
    r = &m->risks[m->nrisks++];
    sem_cpy(r->title, title, 64);
    sem_cpy(r->risk,  risk,  32);
    r->score = score;
}

static void sem_add_action(Model* m, const char* title, int dcoherence)
{
    Action* a;
    if (m->nactions >= M_MAXACTIONS) return;
    a = &m->actions[m->nactions++];
    sem_cpy(a->title, title, 64);
    a->dcoherence = dcoherence;
    a->lib_id = -1;      /* not a library action by default */
    a->category = 0;
    a->complexity = 0;
}

static void sem_add_flow(Model* m, const char* label, int absent)
{
    FlowStep* s;
    if (m->nflow >= M_MAXFLOW) return;
    s = &m->flow[m->nflow++];
    sem_cpy(s->label, label, 24);   /* label cap is 24 per contract */
    s->absent = absent;
}

/* Capitalized short root of a function name (e.g. "tower_tick" -> "Tower"). */
static void sem_root_cap(const char* name, char* out, int cap)
{
    int i = 0;
    if (cap <= 0) { return; }
    out[0] = '\0';
    if (!name) return;
    for (i = 0; i < cap - 1 && name[i] && name[i] != '_'; i++) {
        char c = name[i];
        if (i == 0 && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[i] = c;
    }
    out[i] = '\0';
    if (out[0] == '\0') sem_cpy(out, "Fn", cap);
}

/* Does an absent port of the given type exist on f? */
static int sem_has_absent(const Func* f, PortType type)
{
    int i;
    for (i = 0; i < f->nports && i < M_MAXPORTS; i++) {
        if (f->ports[i].status == PS_ABSENT && f->ports[i].type == type) return 1;
    }
    return 0;
}

/* Name of the first absent port of a given type (for naming flow/conns). */
static const char* sem_absent_name(const Func* f, PortType type)
{
    int i;
    for (i = 0; i < f->nports && i < M_MAXPORTS; i++) {
        if (f->ports[i].status == PS_ABSENT && f->ports[i].type == type)
            return f->ports[i].name;
    }
    return "gate";
}

/*
 * For a global the focus writes, find ANOTHER function (not the focus) that
 * reads it. Returns its name (preferring file label if set) or 0 if none.
 */
static const char* sem_other_reader(const Model* m, int focus, const char* gname)
{
    int i, j;
    for (i = 0; i < m->nfuncs && i < M_MAXFUNCS; i++) {
        if (i == focus) continue;
        for (j = 0; j < m->funcs[i].nreads && j < M_MAXREFS; j++) {
            if (sem_eq(m->funcs[i].reads[j], gname)) {
                const Func* o = &m->funcs[i];
                return o->file[0] ? o->file : o->name;
            }
        }
    }
    return 0;
}

/* "pool-like" write target heuristic: bullet / pool / _list / trailing 's'. */
static int sem_is_pool_like(const char* g)
{
    int n;
    if (!g) return 0;
    if (sem_icontains(g, "bullet")) return 1;
    if (sem_icontains(g, "pool"))   return 1;
    if (sem_icontains(g, "_list"))  return 1;
    n = sem_strlen(g);
    if (n >= 2 && g[n - 1] == 's') return 1;   /* plural collection */
    return 0;
}

/* ===========================================================================
 * Derived analysis for the focus function.
 * ===========================================================================*/

static void sem_analyze_focus(Model* m, Func* f, int focus)
{
    int i;
    /* AUDIT honest-map (#3): read the absent-gate INTENT (set in sem_build_ports)
     * rather than scanning the 16-slot ports[] -- otherwise a function whose
     * absent gates were pushed past M_MAXPORTS loses its risks/actions AND keeps
     * coherence=100, a pure cap artifact. Equivalent to the old sem_has_absent
     * when not truncated (those gates are the only PS_ABSENT ports ever made). */
    int has_lifecycle = f->wants_lifecycle;
    int has_gate      = f->wants_gate;
    int coherence     = 100;
    char buf[64];
    char root[M_NAME];

    /* ---------- CONNECTIONS ---------- */

    /* reads: safe, compat 80..90 */
    for (i = 0; i < f->nreads && i < M_MAXREFS; i++) {
        int compat = 80 + (i % 11);                 /* 80..90 */
        sem_add_conn(m, f->name, f->reads[i], PORT_STATE_READ, compat, CS_SAFE);
    }
    /* writes: compat 60..90, WEAK if <70 */
    for (i = 0; i < f->nwrites && i < M_MAXREFS; i++) {
        int compat = 60 + ((i * 13) % 31);          /* 60..90, deterministic spread */
        ConnStatus st = (compat < 70) ? CS_WEAK : CS_SAFE;
        sem_add_conn(m, f->name, f->writes[i], PORT_STATE_WRITE, compat, st);
    }
    /* calls: control, compat 80..85, safe */
    for (i = 0; i < f->ncalls && i < M_MAXCALLS; i++) {
        int compat = 80 + (i % 6);                  /* 80..85 */
        sem_add_conn(m, f->name, f->calls[i], PORT_CONTROL, compat, CS_SAFE);
    }
    /* REVERSE conns: another function reading a global the focus writes. */
    for (i = 0; i < f->nwrites && i < M_MAXREFS; i++) {
        const char* rdr = sem_other_reader(m, focus, f->writes[i]);
        if (rdr) sem_add_conn(m, rdr, f->writes[i], PORT_STATE_READ, 89, CS_SAFE);
    }
    /* ABSENT conns: each absent port -> its relevant global (first write). */
    {
        const char* gtarget = (f->nwrites > 0) ? f->writes[0] : f->name;
        if (has_lifecycle)
            sem_add_conn(m, sem_absent_name(f, PORT_LIFECYCLE), gtarget,
                         PORT_LIFECYCLE, -1, CS_ABSENT);
        if (has_gate)
            sem_add_conn(m, sem_absent_name(f, PORT_CONTROL_GATE), gtarget,
                         PORT_CONTROL_GATE, -1, CS_ABSENT);
    }

    /* ---------- RISKS ---------- */

    /* Multiple writers per shared written global. */
    for (i = 0; i < f->nwrites && i < M_MAXREFS; i++) {
        if (sem_is_shared_write(m, f->writes[i])) {
            buf[0] = '\0';
            sem_cat(buf, "Multiple Writers: ", 64);
            sem_cat(buf, f->writes[i], 64);
            sem_add_risk(m, buf, "Collision", 78);
        }
    }
    if (has_lifecycle) sem_add_risk(m, "Missing Claim Gate",    "Lifecycle Leak", 84);
    if (has_gate)      sem_add_risk(m, "Missing Cooldown Gate", "Overfire",       69);
    /* Performance: first read scanned repeatedly. */
    if (f->nreads > 0) {
        buf[0] = '\0';
        if (sem_icontains(f->reads[0], "enem")) {
            sem_cat(buf, "Nearest Enemy Scan Repeated", 64);
        } else {
            sem_cat(buf, f->reads[0], 64);
            sem_cat(buf, " Scan Repeated", 64);
        }
        sem_add_risk(m, buf, "Performance", 61);
    }

    /* ---------- ACTIONS ---------- */

    if (has_lifecycle) sem_add_action(m, "Add claim_slot() wrapper", 38);
    /* Pool release if any written global looks like a pool/collection. */
    for (i = 0; i < f->nwrites && i < M_MAXREFS; i++) {
        if (sem_is_pool_like(f->writes[i])) {
            sem_add_action(m, "Add projectile_pool_release_dead()", 29);
            break;
        }
    }
    if (has_gate) sem_add_action(m, "Add cooldown_gate", 17);
    if (f->nreads > 0) {
        if (sem_icontains(f->reads[0], "enem")) {
            sem_add_action(m, "Cache nearest enemy query", 18);
        } else {
            buf[0] = '\0';
            sem_cat(buf, "Cache ", 64);
            sem_cat(buf, f->reads[0], 64);
            sem_cat(buf, " query", 64);
            sem_add_action(m, buf, 18);
        }
    }

    /* ---------- FLOW (reads -> gates -> writes -> calls) ---------- */

    sem_root_cap(f->name, root, sizeof(root));
    buf[0] = '\0';
    sem_cat(buf, root, 24);
    sem_cat(buf, " Event", 24);
    sem_add_flow(m, buf, 0);

    for (i = 0; i < f->nreads && i < M_MAXREFS; i++) {
        buf[0] = '\0';
        sem_cat(buf, "Read ", 24);
        sem_cat(buf, f->reads[i], 24);
        sem_add_flow(m, buf, 0);
    }
    if (has_gate) {
        buf[0] = '\0';
        sem_cat(buf, "Check ", 24);
        sem_cat(buf, sem_absent_name(f, PORT_CONTROL_GATE), 24);
        sem_add_flow(m, buf, 1);
    }
    if (has_lifecycle) {
        sem_add_flow(m, "Claim Slot", 1);
    }
    for (i = 0; i < f->nwrites && i < M_MAXREFS; i++) {
        buf[0] = '\0';
        sem_cat(buf, "Write ", 24);
        sem_cat(buf, f->writes[i], 24);
        sem_add_flow(m, buf, 0);
    }
    for (i = 0; i < f->ncalls && i < M_MAXCALLS; i++) {
        sem_add_flow(m, f->calls[i], 0);
    }

    /* ---------- COHERENCE ---------- */

    /* -10 per absent gate, read from INTENT not the truncated ports[] array
     * (#3: makes the score cap-independent; same result when not truncated). */
    if (f->wants_lifecycle) coherence -= 10;
    if (f->wants_gate)      coherence -= 10;
    /* -6 per high-severity risk (>75), -3 otherwise. */
    for (i = 0; i < m->nrisks && i < M_MAXRISKS; i++) {
        coherence -= (m->risks[i].score > 75) ? 6 : 3;
    }
    if (coherence < 0)   coherence = 0;
    if (coherence > 100) coherence = 100;
    m->coherence = coherence;
}

/* ===========================================================================
 * Public entry point.
 * ===========================================================================*/

void model_analyze(Model* m)
{
    int i;
    Func* f;

    if (!m) return;

    /* clamp counts defensively (parser is trusted, but stay crash-proof). */
    if (m->nfuncs < 0)   m->nfuncs = 0;
    if (m->nfuncs > M_MAXFUNCS)   m->nfuncs = M_MAXFUNCS;
    if (m->nglobals < 0) m->nglobals = 0;
    if (m->nglobals > M_MAXGLOBALS) m->nglobals = M_MAXGLOBALS;

    /* clear derived state up front. */
    m->nconns = m->nrisks = m->nactions = m->nflow = 0;
    m->coherence = 0;

    /* (1) PORTS for every function. */
    for (i = 0; i < m->nfuncs; i++)
        sem_build_ports(m, &m->funcs[i]);

    /* (2) Derived analysis for the focus function only. */
    if (m->focus < 0 || m->focus >= m->nfuncs) {
        m->analyzed = 1;
        return;
    }
    f = &m->funcs[m->focus];
    sem_analyze_focus(m, f, m->focus);

    /* (3) done. */
    (void)sem_has_port;   /* retained helpers; silence unused warning */
    (void)sem_has_absent; /* superseded by wants_* intent (#3) but kept */
    m->analyzed = 1;
}


