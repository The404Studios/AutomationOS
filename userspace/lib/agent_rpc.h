/*
 * AGENT-RPC-0 wire schema (P6a) -- the typed-tool contract carried as the
 * PAYLOAD of CHANNEL-0 CH_MSG packets. The agent never scrapes the terminal; it
 * sends TOOL_RUN { path, args } and receives TOOL_RESULT { exit_code, ... }.
 *
 * P6a is SCHEMA ONLY: packet type IDs, versioned fixed-size packed payload
 * structs, and encode/decode/validate helpers. There is NO dispatch, NO spawn,
 * NO fd/stdout-channel passing here -- that is P6b. Lock the wire contract first.
 *
 * Wire model: a CH_MSG packet's msg_packet_t.type selects the payload struct
 * (MSG_TOOL_RUN / MSG_TOOL_RESULT); the packet payload bytes are EXACTLY one of
 * the structs below (fixed size == sizeof). The kernel ring is opaque to all of
 * this -- the schema is pure userspace policy. Full doc: docs/AGENT_RPC_WIRE.md.
 */
#ifndef USERSPACE_AGENT_RPC_H
#define USERSPACE_AGENT_RPC_H

/* Schema version. Bump on ANY incompatible struct/field change; receivers MUST
 * reject a version they don't understand (forward-safety for future updates). */
#define AGENT_RPC_VERSION 1

/* msg_packet_t.type values. The 0x01xx range is reserved for AGENT-RPC-0. */
#define MSG_TOOL_RUN     0x0101u
#define MSG_TOOL_RESULT  0x0102u

/* Bounded inline buffers so a whole packet stays well under one ring page. */
#define TOOL_PATH_MAX 120
#define TOOL_ARGS_MAX 256
#define TOOL_ARGV_MAX 16    /* P6d: max entries in argv[1..] (the extra-args vector) */

/* Per-message flags. */
#define TOOL_F_NONE  0x0000u
#define TOOL_F_ERR   0x0001u   /* TOOL_RESULT: the runner rejected/failed the call (stdout_token=0) */

/* TOOL_RUN payload: "run this tool with these args". Fixed size; path and args
 * are NUL-terminated within their buffers and bounded by *_len (the length does
 * NOT include the terminator). */
typedef struct tool_run {
    unsigned short version;          /* = AGENT_RPC_VERSION                     */
    unsigned short flags;            /* TOOL_F_*                                */
    unsigned int   path_len;         /* used bytes of path[] (1..TOOL_PATH_MAX-1)*/
    unsigned int   args_len;         /* used bytes of args[] (0..TOOL_ARGS_MAX-1)*/
    unsigned int   reserved;         /* 0; future use                           */
    char           path[TOOL_PATH_MAX];
    char           args[TOOL_ARGS_MAX];
} tool_run_t;

/* TOOL_RESULT payload: the outcome. In P6a stdout_handle is ALWAYS 0 -- channel
 * passing is P6b. */
typedef struct tool_result {
    unsigned short version;          /* = AGENT_RPC_VERSION                  */
    unsigned short flags;            /* TOOL_F_*                             */
    int            exit_code;        /* the tool's exit status               */
    /* Opaque stdout token -- its meaning is checkpoint-defined, NOT a stable
     * process-local handle. Renamed from stdout_handle so no reader assumes it
     * is directly usable:
     *   P6a: 0 (inert -- no tool ran).
     *   P6b: a RUNNER-LOCAL handle token, non-dereferenceable by the agent
     *        (the runner drained stdout itself).
     *   P6c: a one-shot GRANT ID. The agent MUST call SYS_CH_ACCEPT(stdout_token)
     *        to convert it into a read-only local handle, then read the tool's
     *        stdout. See docs/AGENT_RPC_WIRE.md ("stdout_token semantics"). */
    unsigned int   stdout_token;
    unsigned int   reserved;         /* 0                                    */
} tool_result_t;

/* ---- validate/encode result codes ---- */
#define AR_OK         0
#define AR_E_TOOLONG (-1)   /* path/args exceeds its bound       */
#define AR_E_VERSION (-2)   /* schema version mismatch           */
#define AR_E_LEN     (-3)   /* payload length != sizeof(struct)  */
#define AR_E_FIELD   (-4)   /* a length field is out of range    */
#define AR_E_NUL     (-5)   /* path/args not NUL-terminated      */
#define AR_E_EMPTYARG (-6)  /* P6d: an empty argv entry (\0\0)   */

/* ---- tiny freestanding helpers (no libc) ---- */
static unsigned ar_slen_(const char* s){ unsigned n=0; while(s&&s[n]) n++; return n; }
static void ar_zero_(void* p, unsigned n){ unsigned char* b=(unsigned char*)p; for(unsigned i=0;i<n;i++) b[i]=0; }
static void ar_copy_(char* d, const char* s, unsigned n){ for(unsigned i=0;i<n;i++) d[i]=s[i]; }

/* Encode a TOOL_RUN. `path` is required (1..TOOL_PATH_MAX-1 chars); `args` may be
 * NULL/empty. Returns AR_OK, or AR_E_TOOLONG if a field overflows its bound. */
static int tool_run_encode(tool_run_t* out, const char* path, const char* args) {
    if (!out || !path) return AR_E_FIELD;
    unsigned pl = ar_slen_(path), al = args ? ar_slen_(args) : 0;
    if (pl == 0 || pl >= TOOL_PATH_MAX) return AR_E_TOOLONG;
    if (al >= TOOL_ARGS_MAX)            return AR_E_TOOLONG;
    ar_zero_(out, sizeof(*out));
    out->version  = AGENT_RPC_VERSION;
    out->flags    = TOOL_F_NONE;
    out->path_len = pl;
    out->args_len = al;
    out->reserved = 0;
    ar_copy_(out->path, path, pl); out->path[pl] = 0;
    if (al) ar_copy_(out->args, args, al);
    out->args[al] = 0;
    return AR_OK;
}

/* Validate a received TOOL_RUN of `payload_len` bytes (the CH_MSG header's len). */
static int tool_run_validate(const tool_run_t* t, unsigned payload_len) {
    if (!t) return AR_E_FIELD;
    if (payload_len != sizeof(tool_run_t))        return AR_E_LEN;
    if (t->version != AGENT_RPC_VERSION)          return AR_E_VERSION;
    if (t->path_len == 0 || t->path_len >= TOOL_PATH_MAX) return AR_E_FIELD;
    if (t->args_len >= TOOL_ARGS_MAX)             return AR_E_FIELD;
    if (t->path[t->path_len] != 0)                return AR_E_NUL;
    if (t->args[t->args_len] != 0)                return AR_E_NUL;
    return AR_OK;
}

/* ---- P6d: argv as a VECTOR (the args field = NUL-separated argv[1..]) ----
 * argv[0] is always the explicit `path`; the args buffer holds the EXTRA entries
 * only -- never arg0 (no duplicated path state). Build N entries into out->args
 * as "e0\0e1\0...\0" and set args_len (incl. all NULs). Each entry must be
 * non-empty; the whole thing must fit TOOL_ARGS_MAX. Returns AR_OK / AR_E_*. */
static int tool_run_set_argv(tool_run_t* out, const char* const* entries, int n) {
    if (!out) return AR_E_FIELD;
    if (n > TOOL_ARGV_MAX) return AR_E_FIELD;
    unsigned pos = 0;
    for (int i = 0; i < n; i++) {
        unsigned l = ar_slen_(entries[i]);
        if (l == 0) return AR_E_EMPTYARG;                 /* no empty argv entries */
        if (pos + l + 1 > TOOL_ARGS_MAX) return AR_E_TOOLONG;
        ar_copy_(out->args + pos, entries[i], l); pos += l;
        out->args[pos++] = 0;                              /* NUL separator (incl. the last) */
    }
    out->args_len = pos;                                   /* total bytes, includes all NULs */
    return AR_OK;
}

/* Validate the args field as argv[1..] (P6d). args_len==0 is valid (no extra
 * args / path-only). Otherwise: within cap, NUL-terminated, no empty entry, and
 * at most TOOL_ARGV_MAX entries. Shell metacharacters are NOT inspected -- they
 * ride through as literal bytes (argv is a vector, not a command line). */
static int argv_validate(const tool_run_t* t) {
    if (!t) return AR_E_FIELD;
    if (t->args_len == 0) return AR_OK;                    /* no extra args */
    if (t->args_len >= TOOL_ARGS_MAX)         return AR_E_FIELD;  /* over cap        */
    if (t->args[t->args_len - 1] != 0)        return AR_E_NUL;    /* missing final NUL */
    unsigned i = 0, count = 0;
    while (i < t->args_len) {
        unsigned s = i;
        while (i < t->args_len && t->args[i] != 0) i++;    /* one entry [s, i) */
        if (i == s) return AR_E_EMPTYARG;                  /* empty entry (\0\0 / leading \0) */
        i++;                                               /* skip the NUL separator */
        if (++count > TOOL_ARGV_MAX) return AR_E_FIELD;    /* too many args */
    }
    return AR_OK;
}

/* Encode a TOOL_RESULT (P6a: stdout_handle is always 0). */
static int tool_result_encode(tool_result_t* out, int exit_code) {
    if (!out) return AR_E_FIELD;
    ar_zero_(out, sizeof(*out));
    out->version      = AGENT_RPC_VERSION;
    out->flags        = TOOL_F_NONE;
    out->exit_code    = exit_code;
    out->stdout_token = 0;
    out->reserved     = 0;
    return AR_OK;
}

/* Validate a received TOOL_RESULT of `payload_len` bytes. */
static int tool_result_validate(const tool_result_t* r, unsigned payload_len) {
    if (!r) return AR_E_FIELD;
    if (payload_len != sizeof(tool_result_t)) return AR_E_LEN;
    if (r->version != AGENT_RPC_VERSION)      return AR_E_VERSION;
    return AR_OK;
}

#endif /* USERSPACE_AGENT_RPC_H */
