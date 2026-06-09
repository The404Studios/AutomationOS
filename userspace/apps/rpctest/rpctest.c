/* rpctest -- AGENT-RPC-0 P6a proof: encode/decode/validate the TOOL_RUN /
 * TOOL_RESULT wire schema. SCHEMA ONLY -- no channels, no spawn, no dispatch.
 * It round-trips each struct in memory and exercises the rejection paths
 * (wrong length, wrong version, out-of-range field, over-long encode). Prints
 * RPCTEST: PASS/FAIL to fd1 (serial). init spawns it at boot. */
#include "../../lib/agent_rpc.h"

#define SYS_WRITE 3

typedef unsigned long size_t;
static size_t slen(const char* s){ size_t n=0; while(s&&s[n]) n++; return n; }
/* NOTE: capture the "=a" output. syscall clobbers rax with the return value; an
 * input-only "a"(N) lets gcc assume rax is unchanged and skip reloading it on
 * the next call -> stray wrong-numbered syscalls. */
static void out(const char* s){
    long r;
    __asm__ volatile("syscall":"=a"(r):"a"((long)SYS_WRITE),"D"(1L),"S"((long)s),"d"((long)slen(s)):"rcx","r11","memory");
    (void)r;
}
static int eq(const char* a, const char* b, unsigned n){ for(unsigned i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
static void putdec(unsigned v){ char b[12]; int i=0; if(!v){ out("0"); return; }
    while(v){ b[i++]=(char)('0'+v%10); v/=10; } char r[12]; int j=0; while(i) r[j++]=b[--i]; r[j]=0; out(r); }

int main(void) {
    int ok = 1;

    /* TOOL_RUN encode + in-memory round-trip (no args) */
    tool_run_t tr;
    if (tool_run_encode(&tr, "/bin/free", "") != AR_OK)      ok = 0;
    if (tr.version != AGENT_RPC_VERSION)                     ok = 0;
    if (tr.path_len != 9 || !eq(tr.path, "/bin/free", 9))    ok = 0;   /* "/bin/free" = 9 */
    if (tr.args_len != 0)                                    ok = 0;
    if (tool_run_validate(&tr, sizeof(tr)) != AR_OK)         ok = 0;

    /* TOOL_RUN with args */
    tool_run_t tr2;
    if (tool_run_encode(&tr2, "/bin/cc", "main.c -o a") != AR_OK)      ok = 0;
    if (!eq(tr2.args, "main.c -o a", tr2.args_len))                    ok = 0;
    if (tool_run_validate(&tr2, sizeof(tr2)) != AR_OK)                 ok = 0;

    /* rejection paths */
    int rej_len = (tool_run_validate(&tr, sizeof(tr) - 1) == AR_E_LEN);        /* truncated payload */
    tool_run_t bad_v = tr; bad_v.version = 99;
    int rej_ver = (tool_run_validate(&bad_v, sizeof(bad_v)) == AR_E_VERSION);  /* wrong version     */
    tool_run_t bad_f = tr; bad_f.path_len = TOOL_PATH_MAX + 5;
    int rej_fld = (tool_run_validate(&bad_f, sizeof(bad_f)) == AR_E_FIELD);    /* field out of range*/
    char longp[TOOL_PATH_MAX + 8];
    for (int i = 0; i < TOOL_PATH_MAX + 7; i++) longp[i] = 'a';
    longp[TOOL_PATH_MAX + 7] = 0;
    int rej_enc = (tool_run_encode(&tr2, longp, "") == AR_E_TOOLONG);          /* over-long encode  */

    /* TOOL_RESULT encode + round-trip + version rejection */
    tool_result_t res;
    if (tool_result_encode(&res, 0) != AR_OK)               ok = 0;
    if (res.exit_code != 0 || res.stdout_handle != 0)       ok = 0;   /* P6a: no channel passing */
    if (tool_result_validate(&res, sizeof(res)) != AR_OK)   ok = 0;
    tool_result_t bad_r = res; bad_r.version = 99;
    int rej_res = (tool_result_validate(&bad_r, sizeof(bad_r)) == AR_E_VERSION);

    ok = ok && rej_len && rej_ver && rej_fld && rej_enc && rej_res;

    out("RPCTEST: "); out(ok ? "PASS" : "FAIL");
    out(" v=");       putdec(AGENT_RPC_VERSION);
    out(" run_sz=");  putdec((unsigned)sizeof(tool_run_t));
    out(" res_sz=");  putdec((unsigned)sizeof(tool_result_t));
    out(" rej(len="); out(rej_len?"1":"0");
    out(",ver=");     out(rej_ver?"1":"0");
    out(",fld=");     out(rej_fld?"1":"0");
    out(",enc=");     out(rej_enc?"1":"0");
    out(",resver=");  out(rej_res?"1":"0");
    out(")\n");
    return ok ? 0 : 1;
}
