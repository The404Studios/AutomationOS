/* toolrun -- AGENT-RPC-0 P6c proof: make stdout_token a REAL capability.
 *
 * The runner no longer drains the tool's stdout. Instead it GRANTs a one-shot,
 * read-only, MASTER-end capability to the CH_BYTE stdout channel to the agent's
 * pid, and returns the grant_id in TOOL_RESULT.stdout_token. The agent ACCEPTs
 * it -> a read-only local handle -> and reads the EXACT stdout bytes itself.
 *
 * Self-spawns (parent=AGENT, child=RUNNER over a shared CH_MSG ctrl). The agent
 * passes its pid as runner setup metadata: spawn_ex("toolrun","r:<pid>",...) --
 * NOT part of the TOOL_RUN payload, which stays path-only (args_len==0).
 *
 * Negative tests (the capability boundary):
 *   runner: grant CH_MSG ctrl -> EINVAL; grant invalid handle -> EBADF; grant a
 *           no-READ handle -> EPERM; accept own grant (wrong pid) -> EPERM;
 *           fill the grant table -> ENOSPC (self-grants, drained after).
 *   agent:  double-accept -> EBADF; bogus grant id -> EBADF; write the accepted
 *           read-only handle -> denied (ro).
 *
 * An ACK handshake makes the runner outlive the agent's accept (grants are swept
 * on the granter's death, so the runner must not exit before the agent claims).
 */
#include "../../lib/channel.h"
#include "../../lib/agent_rpc.h"

#define SYS_WRITE    3
#define SYS_YIELD   15
#define SYS_WAITPID  6
#define SYS_GETPID   8
#define WNOHANG      1
#define MSG_ACK   0x0ACu
#define RUN_REQ_ID 0xC0FFEE

static const char MARKER[] = "AGENT-RPC-0-PROOF";   /* 17 bytes, must match echoproof */
#define MARKER_LEN 17

static unsigned slen(const char* s){ unsigned n=0; while(s&&s[n]) n++; return n; }
static void outfd(int fd, const char* s){
    long r, n=(long)slen(s);
    __asm__ volatile("syscall":"=a"(r):"a"((long)SYS_WRITE),"D"((long)fd),"S"((long)s),"d"(n):"rcx","r11","memory");
    (void)r;
}
static void decfd(int fd, long v){
    char b[20]; int i=0, neg=v<0; unsigned long u=neg?(unsigned long)(-v):(unsigned long)v;
    if(!u){ outfd(fd,"0"); return; }
    while(u){ b[i++]=(char)('0'+u%10); u/=10; }
    char r[22]; int j=0; if(neg) r[j++]='-'; while(i) r[j++]=b[--i]; r[j]=0; outfd(fd,r);
}
static void yield(void){ _ch_sc(SYS_YIELD,0,0,0,0,0,0); }
static long getpid(void){ return _ch_sc(SYS_GETPID,0,0,0,0,0,0); }
static int eqn(const char* a, const char* b, unsigned n){ for(unsigned i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
static unsigned parse_pid(const char* s){ /* "r:<pid>" -> pid */
    unsigned v=0; while(*s && *s!=':') s++; if(*s==':') s++;
    while(*s>='0'&&*s<='9'){ v=v*10+(unsigned)(*s-'0'); s++; } return v;
}

/* fd0 = ctrl slave READ (h1), fd1 = ctrl slave WRITE (h2); argv = "r:<agentpid>" */
static int runner_mode(unsigned agent_pid) {
    ch_msg_hdr hdr; tool_run_t tr; int got=0;
    for (long i=0;i<4000000 && !got;i++){
        int r = ch_recvmsg(1, &hdr, &tr, sizeof(tr));
        if (r>=0){ got=1; break; }
        if (r==CH_EAGAIN){ yield(); continue; }
        outfd(2,"RUNNER: FAIL recvmsg\n"); return 1;
    }
    if (!got){ outfd(2,"RUNNER: FAIL no-request\n"); return 1; }
    if (hdr.type!=MSG_TOOL_RUN || hdr.len!=sizeof(tool_run_t) ||
        tool_run_validate(&tr,hdr.len)!=AR_OK || tr.args_len!=0 || tr.reserved!=0){
        outfd(2,"RUNNER: FAIL validate\n"); return 1;
    }

    int out = ch_create(CH_BYTE, CH_PAGE);
    if (out<=0){ outfd(2,"RUNNER: FAIL ch_create\n"); return 1; }
    long tpid = spawn_ex(tr.path, "", 0, out, 0);          /* tool stdout -> out (slave) */
    if (tpid<=0){ outfd(2,"RUNNER: FAIL spawn\n"); ch_close(out); return 1; }

    /* wait for the tool to finish writing -- do NOT drain (the agent reads it) */
    int exit_code=-1, st=0, done=0;
    for (long i=0;i<8000000 && !done;i++){
        long w = _ch_sc(SYS_WAITPID, tpid, (long)&st, WNOHANG, 0,0,0);
        if (w!=0){ exit_code=st; done=1; break; }
        yield();
    }

    /* the real grant: read-only stdout capability to the agent */
    int gid = ch_grant(out, agent_pid);
    int grant_ok = (gid > 0);

    /* negative tests (boundary) */
    int ctrl_deny    = (ch_grant(1, agent_pid) == CH_EINVAL);   /* h1 = ctrl (CH_MSG)        */
    int inv_deny     = (ch_grant(77, agent_pid) == CH_EBADF);   /* no such handle            */
    int norights_deny= (ch_grant(2, agent_pid) == CH_EPERM);    /* h2 = ctrl WRITE-only, no R */
    int wrongpid_deny= (ch_accept(gid) == CH_EPERM);            /* runner != to_pid (agent)  */

    /* grant-table-full: self-grants fill the rest, observe ENOSPC, then drain */
    int enospc=0; int dummies[16]; int nd=0;
    for (int i=0;i<16;i++){
        int g = ch_grant(out, (unsigned)getpid());             /* to self, so we can drain  */
        if (g==CH_ENOSPC){ enospc=1; break; }
        if (g>0 && nd<16) dummies[nd++]=g; else break;
    }
    for (int i=0;i<nd;i++){ int h=ch_accept(dummies[i]); if (h>0) ch_close(h); }  /* drain, no leak */

    /* send TOOL_RESULT { exit_code, stdout_token = grant_id } */
    tool_result_t res; tool_result_encode(&res, exit_code); res.stdout_token=(unsigned)gid;
    ch_msg_hdr rh; rh.type=MSG_TOOL_RESULT; rh.flags=0; rh.len=sizeof(res); rh.request_id=hdr.request_id;
    int sent=0;
    for (long i=0;i<2000000;i++){ int w=ch_sendmsg(2,&rh,&res); if(w>=0){sent=1;break;} if(w==CH_EAGAIN){yield();continue;} break; }

    /* wait for the agent's ACK so we outlive its accept (grants swept on our death) */
    int acked=0; ch_msg_hdr ah; char d8[8];
    for (long i=0;i<8000000 && !acked;i++){ int r=ch_recvmsg(1,&ah,d8,sizeof(d8)); if(r>=0){acked=1;break;} if(r==CH_EAGAIN){yield();continue;} break; }

    int ok = (grant_ok && ctrl_deny && inv_deny && norights_deny && wrongpid_deny && enospc && sent && acked);
    outfd(2,"RUNNER: "); outfd(2, ok?"PASS":"FAIL");
    outfd(2," grant=");        outfd(2, grant_ok?"1":"0");
    outfd(2," ctrl_deny=");    outfd(2, ctrl_deny?"1":"0");
    outfd(2," inv_deny=");     outfd(2, inv_deny?"1":"0");
    outfd(2," norights_deny=");outfd(2, norights_deny?"1":"0");
    outfd(2," wrongpid_deny=");outfd(2, wrongpid_deny?"1":"0");
    outfd(2," enospc=");       outfd(2, enospc?"1":"0");
    outfd(2," acked=");        outfd(2, acked?"1":"0");
    outfd(2,"\n");
    ch_close(out);
    return ok?0:1;
}

static int agent_mode(void) {
    unsigned my_pid = (unsigned)getpid();
    int ctrl = ch_create(CH_MSG, CH_PAGE);
    if (ctrl<=0){ outfd(1,"TOOLRUN: FAIL ctrl\n"); return 1; }

    char arg[24]; { char* p=arg; *p++='r'; *p++=':'; char t[12]; int i=0; unsigned v=my_pid;
        if(!v) t[i++]='0'; while(v){ t[i++]=(char)('0'+v%10); v/=10; } while(i) *p++=t[--i]; *p=0; }
    long pid = spawn_ex("sbin/toolrun", arg, ctrl, ctrl, 0);
    if (pid<=0){ outfd(1,"TOOLRUN: FAIL spawn-runner\n"); ch_close(ctrl); return 1; }

    tool_run_t tr; tool_run_encode(&tr, "sbin/echoproof", "");   /* PATH ONLY (args_len=0) */
    ch_msg_hdr hdr; hdr.type=MSG_TOOL_RUN; hdr.flags=0; hdr.len=sizeof(tr); hdr.request_id=RUN_REQ_ID;
    int sent = (ch_sendmsg(ctrl,&hdr,&tr) == (int)(sizeof(ch_msg_hdr)+sizeof(tr)));

    ch_msg_hdr rh; tool_result_t res; int got=0;
    for (long i=0;i<12000000 && !got;i++){ int r=ch_recvmsg(ctrl,&rh,&res,sizeof(res)); if(r>=0){got=1;break;} if(r==CH_EAGAIN){yield();continue;} break; }
    int type_ok = got && rh.type==MSG_TOOL_RESULT;
    int rid_ok  = got && rh.request_id==RUN_REQ_ID;
    int valid   = got && rh.len==sizeof(tool_result_t) && tool_result_validate(&res,rh.len)==AR_OK;

    int accept_ok=0, agent_read=0, exact=0, ro=0, dbl_deny=0, bogus_deny=0;
    if (valid){
        int rh_handle = ch_accept((int)res.stdout_token);     /* token -> read-only handle */
        accept_ok = (rh_handle > 0);
        if (accept_ok){
            char buf[64]; int total=0, n;
            for (long i=0;i<2000000;i++){
                n = ch_read(rh_handle, buf+total, (unsigned long)(sizeof(buf)-total));
                if (n>0){ total+=n; if(total>=MARKER_LEN) break; continue; }
                yield();
                if (i>4) break;                                /* bytes are pre-buffered */
            }
            agent_read = total;
            exact = (total==MARKER_LEN && eqn(buf, MARKER, MARKER_LEN));
            ro  = (ch_write(rh_handle, "x", 1) < 0);            /* read-only: write denied  */
            dbl_deny   = (ch_accept((int)res.stdout_token) == CH_EBADF);   /* one-shot consumed */
            bogus_deny = (ch_accept(200) == CH_EBADF);          /* slot out of range        */
            ch_close(rh_handle);
        }
    }

    /* ACK so the runner can finish + exit after we've claimed the grant */
    ch_msg_hdr ack; ack.type=MSG_ACK; ack.flags=0; ack.len=0; ack.request_id=RUN_REQ_ID;
    ch_sendmsg(ctrl, &ack, (const void*)0);

    int ok = (sent && got && type_ok && rid_ok && valid && accept_ok && exact && ro && dbl_deny && bogus_deny);
    outfd(1,"TOOLRUN: "); outfd(1, ok?"PASS":"FAIL");
    outfd(1," sent=");       outfd(1, sent?"1":"0");
    outfd(1," result=");     outfd(1, got?"1":"0");
    outfd(1," type=");       outfd(1, type_ok?"1":"0");
    outfd(1," rid=");        outfd(1, rid_ok?"1":"0");
    outfd(1," exit=");       decfd(1, valid?res.exit_code:-999);
    outfd(1," accept=");     outfd(1, accept_ok?"1":"0");
    outfd(1," agent_read="); decfd(1, agent_read);
    outfd(1," exact=");      outfd(1, exact?"1":"0");
    outfd(1," ro=");         outfd(1, ro?"1":"0");
    outfd(1," dblaccept_deny="); outfd(1, dbl_deny?"1":"0");
    outfd(1," bogus_deny=");     outfd(1, bogus_deny?"1":"0");
    outfd(1,"\n");
    ch_close(ctrl);
    return ok?0:1;
}

int main(int argc, char** argv) {
    if (argc>=2 && argv[1] && argv[1][0]=='r') return runner_mode(parse_pid(argv[1]));
    return agent_mode();
}
