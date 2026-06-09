/* toolset_host -- TOOLSET-0: a small, SAFE, structured tool surface for the
 * agent, on top of the AGENT-RPC-0 rail. The host is the TRUST SURFACE:
 *
 *   tool request -> host validates tool NAME (whitelist) + path policy
 *     -> host dispatches the known program path -> the tool does ONE bounded op
 *     -> stdout_token returns the result -> host reads the exact stdout.
 *
 * Tools are small sandboxed programs (sbin/tool_read / tool_ls / tool_stat /
 * echoargs), NOT in-kernel handlers and NOT a giant registry. The model (later)
 * drives this surface, never arbitrary process execution.
 *
 * TOOLSET-0 path policy = CONSERVATIVE traversal denial (reject empty / "..").
 * It is NOT a full jail -- root allowlists / per-tool authority scopes are a
 * later brick (TOOL-AUTH-0). Result format = plain text stdout (typed result
 * envelopes are a later brick, TOOL-RESULT-0). NO shell, NO networking, NO model,
 * NO recursive fs walk, NO write/delete tools, NO kernel change.
 */
#include "../../lib/channel.h"
#include "../../lib/agent_rpc.h"

#define SYS_WRITE    3
#define SYS_YIELD   15
#define SYS_WAITPID  6
#define SYS_GETPID   8
#define WNOHANG      1
#define MSG_ACK      0x0ACu
#define MSG_SHUTDOWN 0x0FEu

static unsigned slen(const char* s){ unsigned n=0; while(s&&s[n]) n++; return n; }
static void outfd(int fd,const char* s){ long r,n=(long)slen(s);
    __asm__ volatile("syscall":"=a"(r):"a"((long)SYS_WRITE),"D"((long)fd),"S"((long)s),"d"(n):"rcx","r11","memory"); (void)r; }
static void yield(void){ _ch_sc(SYS_YIELD,0,0,0,0,0,0); }
static long getpid(void){ return _ch_sc(SYS_GETPID,0,0,0,0,0,0); }
static int eqn(const char* a,const char* b,unsigned n){ for(unsigned i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
static int streq(const char* a,const char* b){ while(*a&&*b){ if(*a!=*b) return 0; a++; b++; } return *a==*b; }
static int contains(const char* h,unsigned hn,const char* nd){
    unsigned nn=slen(nd); if(!nn||nn>hn) return 0;
    for(unsigned i=0;i+nn<=hn;i++) if(eqn(h+i,nd,nn)) return 1; return 0; }
static unsigned parse_pid(const char* s){ unsigned v=0; while(*s&&*s!=':') s++; if(*s==':') s++;
    while(*s>='0'&&*s<='9'){ v=v*10+(unsigned)(*s-'0'); s++; } return v; }
static int recv_msg(int h, ch_msg_hdr* hdr, void* buf, unsigned cap){
    for(long i=0;i<10000000;i++){ int r=ch_recvmsg(h,hdr,buf,cap); if(r>=0) return 1; if(r==CH_EAGAIN){yield();continue;} return 0; } return 0; }
static void send_ack(int ctrl, unsigned long rid){ ch_msg_hdr a; a.type=MSG_ACK; a.flags=0; a.len=0; a.request_id=rid; ch_sendmsg(ctrl,&a,(const void*)0); }

/* ---- the host trust surface: whitelist + path policy ---- */
static const char* resolve_tool(const char* name){
    if(streq(name,"read_file")) return "sbin/tool_read";
    if(streq(name,"list_dir"))  return "sbin/tool_ls";
    if(streq(name,"stat"))      return "sbin/tool_stat";
    if(streq(name,"echoargs"))  return "sbin/echoargs";
    return 0;                                              /* unknown tool -> rejected */
}
static int bad_path(const char* p){ if(!p||!p[0]) return 1;       /* empty rejected */
    for(unsigned i=0;p[i];i++) if(p[i]=='.'&&p[i+1]=='.') return 1; return 0; }  /* ".." rejected */

/* ===== RUNNER (child): dispatch whitelisted tools until SHUTDOWN ===== */
static int runner_mode(unsigned agent_pid){
    for(;;){
        ch_msg_hdr hdr; tool_run_t tr;
        if(!recv_msg(1,&hdr,&tr,sizeof(tr))){ outfd(2,"RUNNER: FAIL recv\n"); return 1; }
        if(hdr.type==MSG_SHUTDOWN) break;
        int valid = (hdr.type==MSG_TOOL_RUN && hdr.len==sizeof(tool_run_t) &&
                     tool_run_validate(&tr,hdr.len)==AR_OK && argv_validate(&tr)==AR_OK && tr.reserved==0);
        tool_result_t res; tool_result_encode(&res,-1);
        ch_msg_hdr rh; rh.type=MSG_TOOL_RESULT; rh.flags=0; rh.len=sizeof(res); rh.request_id=hdr.request_id;
        int out=-1;
        if(valid){
            out=ch_create(CH_BYTE,CH_PAGE);
            long tpid=(out>0)?spawn_ex_argv(tr.path,tr.args,tr.args_len,0,out,0):-1;
            int exit_code=-1, st=0;
            if(tpid>0){ for(long i=0;i<8000000;i++){ long w=_ch_sc(SYS_WAITPID,tpid,(long)&st,WNOHANG,0,0,0); if(w!=0){exit_code=st;break;} yield(); } }
            int gid=(out>0&&tpid>0)?ch_grant(out,agent_pid):-1;
            tool_result_encode(&res,exit_code);
            res.flags=(gid>0)?TOOL_F_NONE:TOOL_F_ERR;
            res.stdout_token=(gid>0)?(unsigned)gid:0;
        } else { res.flags=TOOL_F_ERR; res.stdout_token=0; }    /* malformed -> rejected */
        for(long i=0;i<2000000;i++){ int w=ch_sendmsg(2,&rh,&res); if(w>=0)break; if(w==CH_EAGAIN){yield();continue;} break; }
        { ch_msg_hdr ah; char d8[8]; recv_msg(1,&ah,d8,sizeof(d8)); }   /* wait the host's ACK */
        if(out>0) ch_close(out);
    }
    outfd(2,"RUNNER: shutdown\n");
    return 0;
}

/* dispatch one whitelisted tool over the rail; read its stdout into buf.
 * returns 1 if a result came back (sets *ec exit code, *rr runner-rejected,
 * *bl stdout length), 0 on a transport failure. */
static int dispatch(int ctrl, unsigned long rid, const char* prog, const char* arg, int malformed,
                    char* buf, int cap, int* bl, int* ec, int* rr){
    *bl=0; *ec=-999; *rr=0;
    tool_run_t tr; tool_run_encode(&tr, prog, "");
    const char* av[1]={arg}; tool_run_set_argv(&tr, av, 1);
    if(malformed) tr.args[tr.args_len-1]='X';                /* break the argv -> runner rejects */
    ch_msg_hdr h; h.type=MSG_TOOL_RUN; h.flags=0; h.len=sizeof(tr); h.request_id=rid;
    if(ch_sendmsg(ctrl,&h,&tr)!=(int)(sizeof(ch_msg_hdr)+sizeof(tr))) return 0;
    ch_msg_hdr rh; tool_result_t res;
    if(!recv_msg(ctrl,&rh,&res,sizeof(res))) return 0;
    if(!(rh.type==MSG_TOOL_RESULT && rh.len==sizeof(res) && tool_result_validate(&res,rh.len)==AR_OK)){ send_ack(ctrl,rid); return 0; }
    if(res.flags & TOOL_F_ERR){ *rr=1; send_ack(ctrl,rid); return 1; }
    *ec = res.exit_code;
    if(res.stdout_token){
        int rhh = ch_accept((int)res.stdout_token);
        if(rhh>0){ int total=0; long n;
            for(long i=0;i<2000000;i++){ n=ch_read(rhh, buf+total, (unsigned long)(cap-total));
                if(n>0){ total+=(int)n; if(total>=cap) break; continue; } yield(); if(i>4) break; }
            *bl=total; ch_close(rhh); }
    }
    send_ack(ctrl,rid);
    return 1;
}

/* ===== AGENT HOST (parent): exercise the tool surface, render the verdict ===== */
static int agent_mode(void){
    unsigned my_pid=(unsigned)getpid();
    int ctrl=ch_create(CH_MSG,CH_PAGE);
    if(ctrl<=0){ outfd(1,"TOOLSET: FAIL ctrl\n"); return 1; }
    char arg[24]; { char* p=arg; *p++='r'; *p++=':'; char t[12]; int i=0; unsigned v=my_pid;
        if(!v) t[i++]='0'; while(v){ t[i++]=(char)('0'+v%10); v/=10; } while(i) *p++=t[--i]; *p=0; }
    long pid=spawn_ex("sbin/toolset_host", arg, ctrl, ctrl, 0);
    if(pid<=0){ outfd(1,"TOOLSET: FAIL spawn-runner\n"); ch_close(ctrl); return 1; }

    char buf[256]; int bl, ec, rr; unsigned long rid=1;
    const char* prog;

    /* 1. list_dir /etc -> must contain the known file */
    int ls=0; prog=resolve_tool("list_dir");
    if(prog && !bad_path("/etc") && dispatch(ctrl,rid++,prog,"/etc",0,buf,sizeof(buf),&bl,&ec,&rr))
        ls = (!rr && ec==0 && contains(buf,(unsigned)bl,"toolset0.txt"));

    /* 2. stat /etc/toolset0.txt -> size=15 type=f */
    int stt=0; prog=resolve_tool("stat");
    if(prog && !bad_path("/etc/toolset0.txt") && dispatch(ctrl,rid++,prog,"/etc/toolset0.txt",0,buf,sizeof(buf),&bl,&ec,&rr))
        stt = (!rr && ec==0 && contains(buf,(unsigned)bl,"size=15") && contains(buf,(unsigned)bl,"type=f"));

    /* 3. read_file /etc/toolset0.txt -> EXACT 15 bytes */
    int rdx=0; prog=resolve_tool("read_file");
    if(prog && !bad_path("/etc/toolset0.txt") && dispatch(ctrl,rid++,prog,"/etc/toolset0.txt",0,buf,sizeof(buf),&bl,&ec,&rr))
        rdx = (!rr && ec==0 && bl==15 && eqn(buf,"TOOLSET-0-FILE\n",15));

    /* 4. run echoargs */
    int run=0; prog=resolve_tool("echoargs");
    if(prog && dispatch(ctrl,rid++,prog,"hi",0,buf,sizeof(buf),&bl,&ec,&rr))
        run = (!rr && ec==0 && contains(buf,(unsigned)bl,"hi"));

    /* 5. unknown tool name -> host whitelist rejects (no dispatch) */
    int unk = (resolve_tool("frobnicate")==0);

    /* 6. path traversal -> host policy rejects (no dispatch) */
    int trav = bad_path("../etc/toolset0.txt");

    /* 7. oversize read (a >256 B binary) -> tool rejects (nonzero exit) */
    int ovr=0; prog=resolve_tool("read_file");
    if(prog && !bad_path("/sbin/echoargs") && dispatch(ctrl,rid++,prog,"/sbin/echoargs",0,buf,sizeof(buf),&bl,&ec,&rr))
        ovr = (!rr && ec!=0);

    /* 8. malformed TOOL_RUN -> runner rejects */
    int mal=0; prog=resolve_tool("read_file");
    if(prog && dispatch(ctrl,rid++,prog,"/etc/toolset0.txt",1,buf,sizeof(buf),&bl,&ec,&rr))
        mal = rr;

    { ch_msg_hdr sh; sh.type=MSG_SHUTDOWN; sh.flags=0; sh.len=0; sh.request_id=999; ch_sendmsg(ctrl,&sh,(const void*)0); }

    int ok = (ls && stt && rdx && run && unk && mal && ovr && trav);
    outfd(1,"TOOLSET: "); outfd(1, ok?"PASS":"FAIL");
    outfd(1," ls=");                 outfd(1, ls?"1":"0");
    outfd(1," stat=");               outfd(1, stt?"1":"0");
    outfd(1," read_exact=");         outfd(1, rdx?"1":"0");
    outfd(1," run=");                outfd(1, run?"1":"0");
    outfd(1," unknown_rejected=");   outfd(1, unk?"1":"0");
    outfd(1," malformed_rejected="); outfd(1, mal?"1":"0");
    outfd(1," oversize_rejected=");  outfd(1, ovr?"1":"0");
    outfd(1," traversal_rejected="); outfd(1, trav?"1":"0");
    outfd(1,"\n");
    ch_close(ctrl);
    return ok?0:1;
}

int main(int argc, char** argv){
    if(argc>=2 && argv[1] && argv[1][0]=='r') return runner_mode(parse_pid(argv[1]));
    return agent_mode();
}
