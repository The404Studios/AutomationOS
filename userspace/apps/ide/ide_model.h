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
#define M_MAXCALLS    40   /* real game hubs exceed 16 (deadzone _start: 37 calls) */
#define M_MAXREFS     32   /* real game hubs exceed 16 (deadzone_reset: 30 writes) */
#define M_MAXFUNCS    128   /* real game files exceed 64 (deadzone.c: 66 funcs) */
#define M_MAXGLOBALS  128   /* real game files exceed 64 (deadzone.c: ~70 globals) */
#define M_MAXCONNS    80   /* _start's inbound+outbound edges exceed 48 */
#define M_MAXRISKS    12
#define M_MAXACTIONS  10
#define M_MAXFLOW     64   /* per-function flow steps for a big game function */
#define M_MAXINCLUDES 32
#define M_MAXMACROS   128   /* real game files exceed 32 (deadzone.c: 89 macros) */
#define M_MAXRECORDS  32
#define M_MAXPROTOS   32

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
typedef struct {
    char title[64];       /* action title or library complex name */
    int  dcoherence;      /* signed, x100: coherence delta if applied */
    int  lib_id;          /* library complex ID (-1 = not a library action) */
    int  category;        /* LibraryCategory if lib_id >= 0, else unused */
    int  complexity;      /* 0..100: estimated code complexity */
} Action;            /* includes hardcoded + library actions */
typedef struct { char label[24]; int  absent; } FlowStep;

/* ---- file-level code elements (for LEGO overview) ---- */
typedef struct {
    char path[M_NAME];    /* e.g. "stdio.h" or "<stdint.h>"         */
    int  line;            /* source line (1-based)                   */
} Include;

typedef struct {
    char name[M_NAME];    /* e.g. "MAX_SIZE"                        */
    int  line;            /* source line (1-based)                   */
} Macro;

typedef struct {
    char name[M_NAME];    /* e.g. "Foo" for struct Foo               */
    char kind_tag[16];    /* "struct", "union", "enum", "typedef"    */
    int  line;            /* source line (1-based)                   */
    int  nfields;         /* number of fields/enumerators            */
} Record;

typedef struct {
    char name[M_NAME];    /* function name                           */
    char ret[M_TYPE];     /* return type                             */
    int  line;            /* source line (1-based)                   */
} Proto;

/* ---- the whole analyzed model ---- */
typedef struct {
    Func     funcs[M_MAXFUNCS];     int nfuncs;
    Global   globals[M_MAXGLOBALS]; int nglobals;
    Include  includes[M_MAXINCLUDES]; int nincludes;
    Macro    macros[M_MAXMACROS];     int nmacros;
    Record   records[M_MAXRECORDS];   int nrecords;
    Proto    protos[M_MAXPROTOS];     int nprotos;

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
/* IDE-XFILE-0: the split halves of model_parse, for MULTI-FILE models --
 * reset once, then append each file (cur_file/total_lines = last appended,
 * so drivers parse the OPEN file last). model_parse == reset + one append. */
void model_reset(Model* m);
void model_parse_append(Model* m, const char* src, int len, const char* filename);

#endif /* IDE_MODEL_H */
