/*
 * ide_model.h -- Semantic model shared by the whole IDE.
 *
 * The engine (ide_lex.c -> ide_parse.c -> ide_semantic.c) fills this in;
 * every panel (map, inspector, runtime, code, funcs) renders FROM it.
 *
 * This is the single contract between the analysis engine and the views.
 * Pure C, freestanding-safe. Fixed caps, no malloc.
 */
#ifndef IDE_MODEL_H
#define IDE_MODEL_H

#include <stdint.h>

/* ---- capacities (kept modest to bound static memory) ---- */
#define M_NAME        48
#define M_TYPE        48
#define M_MAXPARAMS   12
#define M_MAXPORTS    16
#define M_MAXCALLS    16
#define M_MAXREFS     16
#define M_MAXFUNCS    64
#define M_MAXGLOBALS  64
#define M_MAXCONNS    48
#define M_MAXRISKS    12
#define M_MAXACTIONS  10
#define M_MAXFLOW     16

/* ---- port semantics (the heart of the "LEGO" model) ---- */
typedef enum {
    PORT_INPUT = 0,      /* function parameter                 */
    PORT_STATE_READ,     /* reads a global/state entity        */
    PORT_STATE_WRITE,    /* writes a global/state entity       */
    PORT_CONTROL,        /* invokes/controls another function  */
    PORT_CONTROL_GATE,   /* a guard/gate that *should* exist    */
    PORT_LIFECYCLE       /* claim/release lifecycle hook        */
} PortType;

typedef enum { DIR_IN = 0, DIR_OUT } PortDir;
typedef enum { PS_CONNECTED = 0, PS_ABSENT, PS_WEAK } PortStatus;

typedef struct {
    char       name[M_NAME];
    PortType   type;
    PortDir    dir;
    int        fit;       /* 0..100 = fit/req score x100   */
    PortStatus status;    /* connected / absent / weak     */
} Port;

typedef struct { char type[M_TYPE]; char name[M_NAME]; } Param;

/* ---- a parsed function ---- */
typedef struct {
    char  name[M_NAME];
    char  ret[M_TYPE];
    char  file[M_NAME];
    int   line_start, line_end;

    Param params[M_MAXPARAMS]; int nparams;
    char  calls [M_MAXCALLS][M_NAME]; int ncalls;   /* functions it calls   */
    char  reads [M_MAXREFS ][M_NAME]; int nreads;   /* globals it reads     */
    char  writes[M_MAXREFS ][M_NAME]; int nwrites;  /* globals it writes    */

    Port  ports[M_MAXPORTS]; int nports;            /* filled by semantic   */
    int   closed;                                   /* IDA-style UI collapse */
} Func;

/* ---- a global/state entity ---- */
typedef struct {
    char name[M_NAME];
    char type[M_TYPE];
    char file[M_NAME];
    int  nreaders, nwriters;   /* fan-in across all functions (risk input) */
} Global;

/* ---- derived analysis for the focused function ---- */
typedef enum { CS_SAFE = 0, CS_WEAK, CS_ABSENT } ConnStatus;
typedef struct {
    char       from[M_NAME];
    char       to[M_NAME];
    PortType   type;
    int        compat;    /* 0..100, or -1 = n/a   */
    ConnStatus status;
} Conn;

typedef struct { char title[64]; char risk[32]; int score; } Risk;     /* score 0..100 */
typedef struct { char title[64]; int  dcoherence; } Action;            /* signed, x100 */
typedef struct { char label[24]; int  absent; } FlowStep;

/* ---- the whole analyzed model ---- */
typedef struct {
    Func     funcs[M_MAXFUNCS];     int nfuncs;
    Global   globals[M_MAXGLOBALS]; int nglobals;

    int      focus;                 /* index into funcs[], or -1 */

    /* derived for funcs[focus]: */
    Conn     conns[M_MAXCONNS];     int nconns;
    Risk     risks[M_MAXRISKS];     int nrisks;
    Action   actions[M_MAXACTIONS]; int nactions;
    FlowStep flow[M_MAXFLOW];       int nflow;
    int      coherence;             /* 0..100 */

    /* pipeline status banner */
    int      lexed, parsed, analyzed;
    int      total_lines;
    char     cur_file[M_NAME];
} Model;

/* ===========================================================================
 * Engine entry points
 *
 * model_parse()   -- ide_parse.c (uses ide_lex.c): clear m, then fill
 *                    m->funcs / m->globals / m->total_lines from one C source
 *                    buffer. Sets m->lexed = m->parsed = 1.
 * model_analyze() -- ide_semantic.c: using m->focus, derive every function's
 *                    ports plus m->conns / m->risks / m->actions / m->flow /
 *                    m->coherence. Sets m->analyzed = 1. Safe if focus < 0.
 * ===========================================================================*/
void model_parse(Model* m, const char* src, int len, const char* filename);
void model_analyze(Model* m);

#endif /* IDE_MODEL_H */
