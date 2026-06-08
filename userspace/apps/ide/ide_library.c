/*
 * ide_library.c -- the curated "complex" / snippet library (data + lookup).
 *
 * A static built-in core (C idioms, data/control structures, networking) plus
 * any *.snip files loaded from /usr/lib/snippets at startup. Bodies are real C
 * with ${N:placeholder} / $N / $0 tab-stop markers (expanded by the editor on
 * insert). Freestanding: const char* bodies in .rodata; disk entries live in a
 * bounded static pool.
 */
#include "ide_library.h"
#include "ide_sys.h"

/* ===================================================================== *
 *  Built-in core
 * ===================================================================== */
static const Snippet g_builtin[] = {
    /* ---- C idioms / boilerplate ---- */
    { "forr", "for loop", LIBCAT_IDIOM,
      "for (int ${1:i} = 0; ${1:i} < ${2:n}; ${1:i}++) {\n    $0\n}" },
    { "whilel", "while loop", LIBCAT_IDIOM,
      "while (${1:cond}) {\n    $0\n}" },
    { "ifel", "if / else", LIBCAT_IDIOM,
      "if (${1:cond}) {\n    $0\n} else {\n}" },
    { "switchc", "switch / case", LIBCAT_IDIOM,
      "switch (${1:expr}) {\n    case ${2:val}:\n        $0\n        break;\n    default:\n        break;\n}" },
    { "dowhile", "do / while", LIBCAT_IDIOM,
      "do {\n    $0\n} while (${1:cond});" },
    { "funcdef", "function definition", LIBCAT_IDIOM,
      "${1:void} ${2:name}(${3:void}) {\n    $0\n}" },
    { "structdef", "struct definition", LIBCAT_IDIOM,
      "struct ${1:Name} {\n    ${2:int field};\n};" },
    { "typedefst", "typedef struct", LIBCAT_IDIOM,
      "typedef struct {\n    ${1:int field};\n} ${2:Name};" },
    { "enumdef", "enum definition", LIBCAT_IDIOM,
      "enum ${1:Name} {\n    ${2:VALUE} = 0,\n};" },

    /* ---- data & control structures (auto-wire via g_<sys>_* globals) ---- */
    { "statemachine", "state machine (enum + step)", LIBCAT_DATA,
      "typedef enum { ${1:ST_INIT}, ST_RUN, ST_DONE } ${2:sm_state_t};\n"
      "static ${2:sm_state_t} g_${3:sm}_state = ${1:ST_INIT};\n\n"
      "void ${3:sm}_step(void) {\n"
      "    switch (g_${3:sm}_state) {\n"
      "    case ${1:ST_INIT}: g_${3:sm}_state = ST_RUN; break;\n"
      "    case ST_RUN:  $0 break;\n"
      "    case ST_DONE: break;\n"
      "    }\n"
      "}" },
    { "ringbuf", "fixed ring buffer (FIFO)", LIBCAT_DATA,
      "#define ${1:RB}_CAP 64\n"
      "static ${2:int} g_${3:rb}_buf[${1:RB}_CAP];\n"
      "static int g_${3:rb}_head, g_${3:rb}_tail;\n\n"
      "int ${3:rb}_push(${2:int} v) {\n"
      "    int n = (g_${3:rb}_head + 1) % ${1:RB}_CAP;\n"
      "    if (n == g_${3:rb}_tail) return 0;   /* full */\n"
      "    g_${3:rb}_buf[g_${3:rb}_head] = v; g_${3:rb}_head = n; return 1;\n"
      "}\n"
      "int ${3:rb}_pop(${2:int}* out) {\n"
      "    if (g_${3:rb}_tail == g_${3:rb}_head) return 0;   /* empty */\n"
      "    *out = g_${3:rb}_buf[g_${3:rb}_tail];\n"
      "    g_${3:rb}_tail = (g_${3:rb}_tail + 1) % ${1:RB}_CAP; return 1;\n"
      "}$0" },
    { "fixedpool", "fixed object pool (claim/release)", LIBCAT_DATA,
      "#define ${1:POOL}_MAX 128\n"
      "static ${2:Item} g_${3:pool}[${1:POOL}_MAX];\n"
      "static int g_${3:pool}_used[${1:POOL}_MAX];\n\n"
      "int ${3:pool}_claim_slot(void) {\n"
      "    for (int i = 0; i < ${1:POOL}_MAX; i++)\n"
      "        if (!g_${3:pool}_used[i]) { g_${3:pool}_used[i] = 1; return i; }\n"
      "    return -1;   /* pool full */\n"
      "}\n"
      "void ${3:pool}_release(int i) {\n"
      "    if (i >= 0 && i < ${1:POOL}_MAX) g_${3:pool}_used[i] = 0;\n"
      "}$0" },
    { "fixedstep", "fixed-timestep update loop", LIBCAT_DATA,
      "static long g_${1:sim}_last, g_${1:sim}_acc;\n"
      "void ${1:sim}_pump(long now_ms) {\n"
      "    const long STEP = 16;   /* ~60 Hz */\n"
      "    g_${1:sim}_acc += now_ms - g_${1:sim}_last;\n"
      "    g_${1:sim}_last = now_ms;\n"
      "    if (g_${1:sim}_acc > STEP * 5) g_${1:sim}_acc = STEP * 5;\n"
      "    while (g_${1:sim}_acc >= STEP) {\n"
      "        g_${1:sim}_acc -= STEP;\n"
      "        $0   /* fixed update: read+write g_${1:sim}_* */\n"
      "    }\n"
      "}" },

    /* ---- networking (wraps userspace/lib/net) ---- */
    { "tcpclient", "TCP connect + send + recv", LIBCAT_NET,
      "int ${1:sock} = net_socket(NET_SOCK_TCP);\n"
      "if (net_connect(${1:sock}, ip4(${2:10},0,2,2), ${3:80}) == 0) {\n"
      "    net_send(${1:sock}, ${4:req}, ${5:len});\n"
      "    char buf[1024];\n"
      "    int n = net_recv(${1:sock}, buf, sizeof buf);\n"
      "    $0\n"
      "}\n"
      "net_close(${1:sock});" },
    { "udpsock", "UDP send + receive", LIBCAT_NET,
      "int ${1:sock} = net_socket(NET_SOCK_UDP);\n"
      "net_sendto(${1:sock}, ${2:buf}, ${3:len}, ip4(${4:10},0,2,2), ${5:port});\n"
      "unsigned int src_ip; unsigned short src_port;\n"
      "char rx[512];\n"
      "int n = net_recvfrom(${1:sock}, rx, sizeof rx, &src_ip, &src_port);\n"
      "$0\n"
      "net_close(${1:sock});" },
    { "dnsresolve", "DNS resolve hostname -> IP", LIBCAT_NET,
      "unsigned int ${1:ip} = 0;\n"
      "if (dns_resolve(${2:\"example.com\"}, &${1:ip}) == 0) {\n"
      "    $0   /* ${1:ip} is host-order; use ip4()/net_fmt_ip() */\n"
      "}" },
};
#define G_BUILTIN_COUNT ((int)(sizeof(g_builtin) / sizeof(g_builtin[0])))

/* ===================================================================== *
 *  Disk-loaded extension (/usr/lib/snippets/*.snip)
 * ===================================================================== */
#define LIB_DISK_MAX   64
#define LIB_TEXT_POOL  49152      /* bytes for all disk strings           */
#define LIB_FILE_CAP   8192       /* max bytes of one .snip file          */

static Snippet g_disk[LIB_DISK_MAX];
static int     g_disk_count;
static char    g_text[LIB_TEXT_POOL];
static int     g_text_used;

/* copy s (len bytes) into the text pool, NUL-terminate, return pointer or 0. */
static const char* pool_put(const char* s, int len) {
    if (len < 0) len = 0;
    if (g_text_used + len + 1 > LIB_TEXT_POOL) return 0;
    char* d = &g_text[g_text_used];
    for (int i = 0; i < len; i++) d[i] = s[i];
    d[len] = 0;
    g_text_used += len + 1;
    return d;
}

static int cat_from_tag(const char* t) {
    if (ide_strneq(t, "data", 4)) return LIBCAT_DATA;
    if (ide_strneq(t, "net", 3))  return LIBCAT_NET;
    if (ide_strneq(t, "idiom", 5) || ide_strneq(t, "c", 1)) return LIBCAT_IDIOM;
    return LIBCAT_OTHER;
}

/* Parse one .snip file buffer (NUL-terminated) into g_disk[]. Format:
 *   SNIPPET:name=...   (begins a block; value unused)
 *   TRIGGER:<trigger>
 *   CATEGORY:<tag>
 *   LABEL:<label>
 *   BODY:<first body line>
 *   <body lines...>
 *   END
 */
static void parse_snip_file(const char* buf, int len) {
    int i = 0;
    while (i < len && g_disk_count < LIB_DISK_MAX) {
        /* read a line [i, eol) */
        int ls = i;
        while (i < len && buf[i] != '\n') i++;
        int le = i;
        if (i < len) i++;             /* consume newline */
        if (le > ls && buf[le - 1] == '\r') le--;
        int llen = le - ls;
        const char* line = buf + ls;

        if (!(llen >= 8 && ide_strneq(line, "SNIPPET:", 8))) continue;

        const char* trig = 0; const char* label = 0; int cat = LIBCAT_OTHER;
        const char* body = 0;
        while (i < len && g_disk_count < LIB_DISK_MAX) {
            int fs = i;
            while (i < len && buf[i] != '\n') i++;
            int fe = i;
            if (i < len) i++;
            if (fe > fs && buf[fe - 1] == '\r') fe--;
            int flen = fe - fs;
            const char* f = buf + fs;

            if (flen == 3 && ide_strneq(f, "END", 3)) break;
            if (flen >= 8 && ide_strneq(f, "TRIGGER:", 8))
                trig = pool_put(f + 8, flen - 8);
            else if (flen >= 6 && ide_strneq(f, "LABEL:", 6))
                label = pool_put(f + 6, flen - 6);
            else if (flen >= 9 && ide_strneq(f, "CATEGORY:", 9)) {
                static char tag[16];
                int n = flen - 9; if (n > 15) n = 15;
                for (int k = 0; k < n; k++) tag[k] = f[9 + k];
                tag[n] = 0; cat = cat_from_tag(tag);
            } else if (flen >= 5 && ide_strneq(f, "BODY:", 5)) {
                int bstart = fs + 5;
                int j = i;            /* start of the line after BODY: */
                int bend = fe;
                for (;;) {
                    if (j >= len) { bend = len; break; }
                    int ks = j;
                    while (j < len && buf[j] != '\n') j++;
                    int ke = j;
                    if (j < len) j++;
                    int kl = ke - ks - ((ke > ks && buf[ke-1]=='\r') ? 1 : 0);
                    if (kl == 3 && ide_strneq(buf + ks, "END", 3)) {
                        bend = ks - 1;   /* drop the '\n' before END */
                        if (bend < bstart) bend = bstart;
                        break;
                    }
                    bend = ke;
                }
                body = pool_put(buf + bstart, bend - bstart);
                i = j;                /* resume after END */
                break;
            }
        }
        if (trig && body) {
            Snippet* s = &g_disk[g_disk_count++];
            s->trigger = trig;
            s->label = label ? label : trig;
            s->body = body;
            s->category = cat;
        }
    }
}

void lib_load_disk(struct Ide* a) {
    (void)a;
    IdeDirent ents[LIB_DISK_MAX];
    int n = ide_list_dir("/usr/lib/snippets", ents, LIB_DISK_MAX);
    if (n <= 0) return;                       /* no dir -> built-in only */
    static char filebuf[LIB_FILE_CAP];
    for (int e = 0; e < n; e++) {
        if (ents[e].type != IDE_DT_REG) continue;
        const char* nm = ents[e].name;
        int l = ide_strlen(nm);
        if (l < 5 || !ide_streq(nm + l - 5, ".snip")) continue;
        char path[320];
        ide_strlcpy(path, "/usr/lib/snippets/", sizeof path);
        int p = ide_strlen(path);
        ide_strlcpy(path + p, nm, (int)sizeof(path) - p);
        int got = ide_read_file(path, filebuf, LIB_FILE_CAP - 1);
        if (got <= 0) continue;
        filebuf[got] = 0;
        parse_snip_file(filebuf, got);
    }
}

/* ===================================================================== *
 *  Public lookup
 * ===================================================================== */
int lib_count(void) { return G_BUILTIN_COUNT + g_disk_count; }

const Snippet* lib_get(int idx) {
    if (idx < 0) return 0;
    if (idx < G_BUILTIN_COUNT) return &g_builtin[idx];
    idx -= G_BUILTIN_COUNT;
    if (idx < g_disk_count) return &g_disk[idx];
    return 0;
}

const char* lib_cat_tag(int category) {
    switch (category) {
    case LIBCAT_IDIOM: return "c";
    case LIBCAT_DATA:  return "data";
    case LIBCAT_NET:   return "net";
    default:           return "lib";
    }
}
