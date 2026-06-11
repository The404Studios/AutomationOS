/* agenthost -- AGENT-HOST-0: the first tiny agent that RIDES the AGENT-RPC-0 rail.
 *
 * It is not new plumbing -- it is the first real CONSUMER of the rail. One host
 * loop proves the "AI-OS" question: can an agent issue TOOL_RUN, receive
 * TOOL_RESULT, accept the stdout_token, read the EXACT stdout, and make a
 * DECISION? Plus: a malformed TOOL_RUN is rejected.
 *
 * Self-spawns (parent = AGENT HOST, child = RUNNER over a shared CH_MSG ctrl).
 * The runner handles two requests: a VALID one (spawn echoargs with an argv
 * vector, grant its stdout) and a MALFORMED one (reject -> TOOL_F_ERR). The
 * agent host renders a structured verdict to fd1 (serial).
 *
 * NO networking, NO model inference, NO async batching, NO tool registry --
 * just the decision loop on top of the finished kernel/user IPC rail.
 */
#include "../../lib/channel.h"
#include "../../lib/agent_rpc.h"

#define SYS_WRITE    3
#define SYS_YIELD   15
#define SYS_WAITPID  6
#define SYS_GETPID   8
#define WNOHANG      1
#define MSG_ACK   0x0ACu

/* echoargs prints argv one entry per line. With path "sbin/echoargs" and the
 * vector ["hello world","a;b|c"] this is the EXACT 32-byte stdout. */
static const char EXPECT[] = "sbin/echoargs\nhello world\na;b|c\n";
#define EXPECT_LEN 32

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
static int eqn(const char* a,const char* b,unsigned n){ for(unsigned i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
static int contains(const char* h,unsigned hn,const char* nd){
    unsigned nn=slen(nd); if(!nn||nn>hn) return 0;
    for(unsigned i=0;i+nn<=hn;i++) if(eqn(h+i,nd,nn)) return 1; return 0;
}
static unsigned parse_pid(const char* s){ unsigned v=0; while(*s&&*s!=':') s++; if(*s==':') s++;
    while(*s>='0'&&*s<='9'){ v=v*10+(unsigned)(*s-'0'); s++; } return v; }

/* recv one CH_MSG packet (bounded poll). returns 1 on got, 0 on timeout/error. */
static int recv_msg(int h, ch_msg_hdr* hdr, void* buf, unsigned cap){
    for(long i=0;i<10000000;i++){ int r=ch_recvmsg(h,hdr,buf,cap); if(r>=0) return 1; if(r==CH_EAGAIN){yield();continue;} return 0; }
    return 0;
}
static void send_ack(int ctrl, unsigned long rid){
    ch_msg_hdr a; a.type=MSG_ACK; a.flags=0; a.len=0; a.request_id=rid;
    ch_sendmsg(ctrl, &a, (const void*)0);
}

/* ===== RUNNER (child): handle exactly two requests, valid then malformed ===== */
static int runner_mode(unsigned agent_pid){
    for(int req=0; req<2; req++){
        ch_msg_hdr hdr; tool_run_t tr;
        if(!recv_msg(1, &hdr, &tr, sizeof(tr))){ outfd(2,"RUNNER: FAIL recv\n"); return 1; }

        int valid = (hdr.type==MSG_TOOL_RUN && hdr.len==sizeof(tool_run_t) &&
                     tool_run_validate(&tr,hdr.len)==AR_OK && argv_validate(&tr)==AR_OK && tr.reserved==0);

        tool_result_t res; tool_result_encode(&res, -1);
        ch_msg_hdr rh; rh.type=MSG_TOOL_RESULT; rh.flags=0; rh.len=sizeof(res); rh.request_id=hdr.request_id;
        int out = -1;

        if(valid){
            out = ch_create(CH_BYTE, CH_PAGE);
            long tpid = (out>0) ? spawn_ex_argv(tr.path, tr.args, tr.args_len, 0, out, 0) : -1;
            int exit_code=-1, st=0;
            if(tpid>0){ for(long i=0;i<8000000;i++){ long w=_ch_sc(SYS_WAITPID,tpid,(long)&st,WNOHANG,0,0,0); if(w!=0){exit_code=st;break;} yield(); } }
            int gid = (out>0 && tpid>0) ? ch_grant(out, agent_pid) : -1;   /* read-only stdout capability */
            tool_result_encode(&res, exit_code);
            res.flags        = (gid>0) ? TOOL_F_NONE : TOOL_F_ERR;
            res.stdout_token = (gid>0) ? (unsigned)gid : 0;
        } else {
            res.flags = TOOL_F_ERR; res.stdout_token = 0;                  /* rejected: no spawn, no grant */
        }

        for(long i=0;i<2000000;i++){ int w=ch_sendmsg(2,&rh,&res); if(w>=0)break; if(w==CH_EAGAIN){yield();continue;} break; }
        { ch_msg_hdr ah; char d8[8]; recv_msg(1, &ah, d8, sizeof(d8)); }    /* wait the agent's ACK */
        if(out>0) ch_close(out);
    }
    outfd(2,"RUNNER: done (2 requests)\n");
    return 0;
}

/* ===== AGENT HOST (parent): issue tool calls, read results, DECIDE ===== */
static int agent_mode(void){
    unsigned my_pid = (unsigned)getpid();
    int ctrl = ch_create(CH_MSG, CH_PAGE);
    if(ctrl<=0){ outfd(1,"AGENTHOST: FAIL ctrl\n"); return 1; }
    char arg[24]; { char* p=arg; *p++='r'; *p++=':'; char t[12]; int i=0; unsigned v=my_pid;
        if(!v) t[i++]='0'; while(v){ t[i++]=(char)('0'+v%10); v/=10; } while(i) *p++=t[--i]; *p=0; }
    long pid = spawn_ex("sbin/agenthost", arg, ctrl, ctrl, 0);
    if(pid<=0){ outfd(1,"AGENTHOST: FAIL spawn-runner\n"); ch_close(ctrl); return 1; }

    /* ---- request 1: a VALID tool call (echoargs + an argv vector) ---- */
    tool_run_t tr; tool_run_encode(&tr, "sbin/echoargs", "");
    const char* av[2] = { "hello world", "a;b|c" };
    tool_run_set_argv(&tr, av, 2);
    ch_msg_hdr h1; h1.type=MSG_TOOL_RUN; h1.flags=0; h1.len=sizeof(tr); h1.request_id=1;
    int sent1 = (ch_sendmsg(ctrl,&h1,&tr) == (int)(sizeof(ch_msg_hdr)+sizeof(tr)));

    ch_msg_hdr rh; tool_result_t res; int got1 = recv_msg(ctrl, &rh, &res, sizeof(res));
    int path_ok=0, argv_ok=0, stdout_exact=0; long exitc=-999;
    if(got1 && rh.type==MSG_TOOL_RESULT && rh.len==sizeof(res) &&
       tool_result_validate(&res,rh.len)==AR_OK && !(res.flags & TOOL_F_ERR) && res.stdout_token!=0){
        path_ok = 1; exitc = res.exit_code;                  /* the path was accepted + the tool ran */
        int rh_handle = ch_accept((int)res.stdout_token);    /* the agent's DECISION needs the output */
        if(rh_handle>0){
            char buf[80]; int total=0,n;
            for(long i=0;i<2000000;i++){ n=ch_read(rh_handle,buf+total,(unsigned long)(sizeof(buf)-total));
                if(n>0){ total+=n; if(total>=EXPECT_LEN) break; continue; } yield(); if(i>4) break; }
            stdout_exact = (total==EXPECT_LEN && eqn(buf, EXPECT, EXPECT_LEN));
            argv_ok = (contains(buf,(unsigned)total,"hello world\n") && contains(buf,(unsigned)total,"a;b|c\n"));
            ch_close(rh_handle);
        }
    }
    send_ack(ctrl, 1);

    /* ---- request 2: a MALFORMED tool call (argv missing its final NUL) ---- */
    tool_run_t bad; tool_run_encode(&bad, "sbin/echoargs", ""); tool_run_set_argv(&bad, av, 2);
    bad.args[bad.args_len - 1] = 'X';                        /* clobber the final NUL -> argv_validate fails */
    ch_msg_hdr h2; h2.type=MSG_TOOL_RUN; h2.flags=0; h2.len=sizeof(bad); h2.request_id=2;
    int sent2 = (ch_sendmsg(ctrl,&h2,&bad) == (int)(sizeof(ch_msg_hdr)+sizeof(bad)));
    ch_msg_hdr rh2; tool_result_t res2; int got2 = recv_msg(ctrl, &rh2, &res2, sizeof(res2));
    int malformed_rejected = (got2 && rh2.type==MSG_TOOL_RESULT && (res2.flags & TOOL_F_ERR) && res2.stdout_token==0);
    send_ack(ctrl, 2);

    /* the agent's DECISION: the tool ran, its output verified, and a bad call was refused */
    int ok = (sent1 && sent2 && path_ok && argv_ok && stdout_exact && exitc==0 && malformed_rejected);
    outfd(1,"AGENTHOST: "); outfd(1, ok?"PASS":"FAIL");
    outfd(1," path_ok=");           outfd(1, path_ok?"1":"0");
    outfd(1," argv_ok=");           outfd(1, argv_ok?"1":"0");
    outfd(1," stdout_exact=");      outfd(1, stdout_exact?"1":"0");
    outfd(1," exit=");              decfd(1, exitc);
    outfd(1," malformed_rejected=");outfd(1, malformed_rejected?"1":"0");
    outfd(1,"\n");
    ch_close(ctrl);
    return ok?0:1;
}

int main(int argc, char** argv){
    if(argc>=2 && argv[1] && argv[1][0]=='r') return runner_mode(parse_pid(argv[1]));
    return agent_mode();
}
