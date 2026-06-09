/* toolrun -- AGENT-RPC-0 P6b proof: a path-only TOOL_RUN runner.
 *
 * Self-spawns (like msgtest):
 *   AGENT (no arg): creates a CH_MSG control channel, spawns ITSELF as the
 *     runner bound to it, sends ONE TOOL_RUN { path="/bin/free", args_len=0 },
 *     and reads back the TOOL_RESULT.
 *   RUNNER (arg "r"): recvs exactly one TOOL_RUN on its bound ctrl slave end,
 *     validates it (version/len/path_len/args_len==0/reserved==0 -- PATH ONLY,
 *     no args yet), creates a CH_BYTE channel, spawns the tool with its stdout
 *     bound to that channel, waits for exit, DRAINS the tool's stdout (the
 *     "stdout read"), and sends back TOOL_RESULT { exit_code, stdout_handle }.
 *
 * Hard limits held: no shell, no arg parser, no PATH lookup (path used verbatim),
 * no env, no stdin, no stderr capture, one run only, only the stdout byte channel
 * is inherited, no streaming protocol. args_len MUST be 0 (args are P6c).
 *
 * The AGENT prints TOOLRUN: PASS/FAIL to fd1 (serial -- unbound). The RUNNER's
 * fd0/fd1 are the ctrl channel, so it prints its own line to fd2 (unbound).
 */
#include "../../lib/channel.h"
#include "../../lib/agent_rpc.h"

#define SYS_WRITE    3
#define SYS_YIELD   15
#define SYS_WAITPID  6
#define WNOHANG      1

static unsigned slen(const char* s){ unsigned n=0; while(s&&s[n]) n++; return n; }
static void outfd(int fd, const char* s){
    long r, n=(long)slen(s);
    __asm__ volatile("syscall":"=a"(r):"a"((long)SYS_WRITE),"D"((long)fd),"S"((long)s),"d"(n):"rcx","r11","memory");
    (void)r;
}
static void decfd(int fd, long v){
    char b[20]; int i=0, neg = v<0; unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if(!u){ outfd(fd,"0"); return; }
    while(u){ b[i++]=(char)('0'+u%10); u/=10; }
    char r[22]; int j=0; if(neg) r[j++]='-'; while(i) r[j++]=b[--i]; r[j]=0; outfd(fd,r);
}
static void yield(void){ _ch_sc(SYS_YIELD,0,0,0,0,0,0); }

#define RUN_REQ_ID 0xC0FFEE

/* fd0 = ctrl slave READ (handle 1), fd1 = ctrl slave WRITE (handle 2), fd2 free */
static int runner_mode(void) {
    ch_msg_hdr hdr; tool_run_t tr;
    int got = 0;
    for (long i = 0; i < 4000000 && !got; i++) {
        int r = ch_recvmsg(1, &hdr, &tr, sizeof(tr));
        if (r >= 0) { got = 1; break; }
        if (r == CH_EAGAIN) { yield(); continue; }
        outfd(2, "RUNNER: FAIL recvmsg\n"); return 1;
    }
    if (!got) { outfd(2, "RUNNER: FAIL no-request\n"); return 1; }

    /* validate the request -- PATH ONLY in P6b */
    if (hdr.type != MSG_TOOL_RUN)               { outfd(2,"RUNNER: FAIL type\n");     return 1; }
    if (hdr.len  != sizeof(tool_run_t))         { outfd(2,"RUNNER: FAIL len\n");      return 1; }
    if (tool_run_validate(&tr, hdr.len) != AR_OK){ outfd(2,"RUNNER: FAIL validate\n"); return 1; }
    if (tr.args_len != 0)                       { outfd(2,"RUNNER: FAIL args!=0\n");  return 1; }  /* P6c */
    if (tr.reserved != 0)                       { outfd(2,"RUNNER: FAIL reserved\n"); return 1; }

    /* stdout byte channel for the tool (runner = master, full rights incl TRANSFER) */
    int out = ch_create(CH_BYTE, CH_PAGE);
    if (out <= 0) { outfd(2,"RUNNER: FAIL ch_create\n"); return 1; }

    /* spawn the tool: stdin unbound, stdout=out, stderr unbound. Path verbatim. */
    long tpid = spawn_ex(tr.path, "", 0, out, 0);
    if (tpid <= 0) { outfd(2,"RUNNER: FAIL spawn\n"); ch_close(out); return 1; }

    /* wait for exit while draining stdout (bounded, non-blocking) */
    int exit_code = -1; long stdout_bytes = 0; char db[256]; int done = 0; int st = 0, n;
    for (long i = 0; i < 8000000 && !done; i++) {
        while ((n = ch_read(out, db, sizeof(db))) > 0) stdout_bytes += n;     /* drain */
        long w = _ch_sc(SYS_WAITPID, tpid, (long)&st, WNOHANG, 0, 0, 0);
        if (w != 0) { exit_code = st;
            while ((n = ch_read(out, db, sizeof(db))) > 0) stdout_bytes += n; /* final drain */
            done = 1; break;
        }
        yield();
    }

    /* send TOOL_RESULT { exit_code, stdout_handle } back on the ctrl WRITE end */
    tool_result_t res; tool_result_encode(&res, exit_code);
    res.stdout_handle = (unsigned)out;                       /* P6b: the runner's handle */
    ch_msg_hdr rh; rh.type = MSG_TOOL_RESULT; rh.flags = 0; rh.len = sizeof(res); rh.request_id = hdr.request_id;
    int sent = 0;
    for (long i = 0; i < 2000000; i++) {
        int w = ch_sendmsg(2, &rh, &res);
        if (w >= 0) { sent = 1; break; }
        if (w == CH_EAGAIN) { yield(); continue; }
        break;
    }

    int ok = (done && stdout_bytes > 0 && sent);
    outfd(2, "RUNNER: "); outfd(2, ok ? "PASS" : "FAIL");
    outfd(2, " path="); outfd(2, tr.path);
    outfd(2, " exit="); decfd(2, exit_code);
    outfd(2, " stdout_bytes="); decfd(2, stdout_bytes);
    outfd(2, " handle="); decfd(2, out);
    outfd(2, " sent="); outfd(2, sent ? "1" : "0");
    outfd(2, "\n");
    ch_close(out);
    return ok ? 0 : 1;
}

static int agent_mode(void) {
    int ctrl = ch_create(CH_MSG, CH_PAGE);
    if (ctrl <= 0) { outfd(1, "TOOLRUN: FAIL ctrl\n"); return 1; }

    long pid = spawn_ex("sbin/toolrun", "r", ctrl, ctrl, 0);   /* runner fd0=ctrl(R), fd1=ctrl(W) */
    if (pid <= 0) { outfd(1, "TOOLRUN: FAIL spawn-runner\n"); ch_close(ctrl); return 1; }

    /* one path-only TOOL_RUN */
    tool_run_t tr; tool_run_encode(&tr, "/bin/free", "");      /* args="" => args_len=0 */
    ch_msg_hdr hdr; hdr.type = MSG_TOOL_RUN; hdr.flags = 0; hdr.len = sizeof(tr); hdr.request_id = RUN_REQ_ID;
    int sent = (ch_sendmsg(ctrl, &hdr, &tr) == (int)(sizeof(ch_msg_hdr) + sizeof(tr)));

    /* await the TOOL_RESULT */
    ch_msg_hdr rh; tool_result_t res; int got = 0;
    for (long i = 0; i < 12000000 && !got; i++) {
        int r = ch_recvmsg(ctrl, &rh, &res, sizeof(res));
        if (r >= 0) { got = 1; break; }
        if (r == CH_EAGAIN) { yield(); continue; }
        break;
    }
    int type_ok = got && (rh.type == MSG_TOOL_RESULT);
    int rid_ok  = got && (rh.request_id == RUN_REQ_ID);
    int len_ok  = got && (rh.len == sizeof(tool_result_t));
    int valid   = len_ok && (tool_result_validate(&res, rh.len) == AR_OK);
    int hnz     = valid && (res.stdout_handle != 0);

    int ok = (sent && got && type_ok && rid_ok && valid && hnz);
    outfd(1, "TOOLRUN: "); outfd(1, ok ? "PASS" : "FAIL");
    outfd(1, " sent=");      outfd(1, sent ? "1" : "0");
    outfd(1, " result=");    outfd(1, got ? "1" : "0");
    outfd(1, " type=");      outfd(1, type_ok ? "1" : "0");
    outfd(1, " rid=");       outfd(1, rid_ok ? "1" : "0");
    outfd(1, " valid=");     outfd(1, valid ? "1" : "0");
    outfd(1, " handle_nz="); outfd(1, hnz ? "1" : "0");
    outfd(1, " exit=");      decfd(1, valid ? res.exit_code : -999);
    outfd(1, "\n");
    ch_close(ctrl);
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && argv[1][0] == 'r') return runner_mode();
    return agent_mode();
}
