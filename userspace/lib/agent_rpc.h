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

/* Per-message flags (reserved now; P6b will define real ones). */
#define TOOL_F_NONE  0x0000u

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
    /* stdout byte-channel handle. P6a: 0 (inert). P6b: a RUNNER-LOCAL token --
     * non-zero but NOT dereferenceable by the agent (no cross-process handle
     * transfer yet); the runner drained stdout itself. P6c: becomes
     * agent-readable after a one-shot read-only handle transfer. See
     * docs/AGENT_RPC_WIRE.md ("stdout_handle semantics"). */
    unsigned int   stdout_handle;
    unsigned int   reserved;         /* 0                                    */
} tool_result_t;

/* ---- validate/encode result codes ---- */
#define AR_OK         0
#define AR_E_TOOLONG (-1)   /* path/args exceeds its bound       */
#define AR_E_VERSION (-2)   /* schema version mismatch           */
#define AR_E_LEN     (-3)   /* payload length != sizeof(struct)  */
#define AR_E_FIELD   (-4)   /* a length field is out of range    */
#define AR_E_NUL     (-5)   /* path/args not NUL-terminated      */

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

/* Encode a TOOL_RESULT (P6a: stdout_handle is always 0). */
static int tool_result_encode(tool_result_t* out, int exit_code) {
    if (!out) return AR_E_FIELD;
    ar_zero_(out, sizeof(*out));
    out->version       = AGENT_RPC_VERSION;
    out->flags         = TOOL_F_NONE;
    out->exit_code     = exit_code;
    out->stdout_handle = 0;
    out->reserved      = 0;
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
