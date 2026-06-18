/* agentd -- the OS-side multi-step agentic loop (NEMOTRON-AGENT, run-everything).
 * =============================================================================
 *
 * Extends MODEL-BRIDGE-0 from a single tool-select to a real MULTI-STEP ReAct
 * loop over ONE persistent connection to the host agent broker
 * (scripts/nemotron_broker.js [Puter free] or nemotron_mock.py), slirp
 * 10.0.2.2:8433. The model lives on the host; agentd is the gated hands:
 *
 *     agentd -> broker:  "GOAL <request>\n"
 *     broker -> agentd:  "TOOL {\"tool\":\"<n>\",\"args\":\"<a>\"}\n"  (call a tool)
 *                        -- or -- "DONE <answer>\n"                    (finished)
 *     agentd -> broker:  "RESULT <tool stdout / status>\n"
 *     ... loop until DONE (bounded).
 *
 * EVERY byte from the broker is HOSTILE MODEL TEXT. Each TOOL is gated EXACTLY
 * like modelbridge: strict whitelist (resolve_tool) + path policy (bad_path)
 * BEFORE any dispatch; an unknown/denied tool returns an error RESULT the model
 * can react to -- it never reaches a spawn. Tool execution + exact stdout
 * capture reuse the proven CHANNEL-0/P6c runner rail. `args` carries the tool's
 * argv \t-separated (JSON-escaped on the wire), so write_file path\tBASE64 etc.
 * work. Bounded everywhere; SKIPs cleanly if net/broker absent.
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
#define TOOL_NAME_MAX 32

#define SYS_SOCKET    51
#define SYS_CONNECT   52
#define SYS_SEND      53
#define SYS_RECV      54
#define SYS_CLOSE_SK  55
#define SYS_SOCK_POLL 58
#define SYS_NET_INFO  59
#define SOCK_STREAM    1
#define EAGAIN_NEG   (-11)

#define MODEL_IP    0x0A000202u   /* 10.0.2.2 = the QEMU slirp host */
#define AGENT_PORT  8433          /* the Nemotron agent broker (free of 8431/8432/8434) */
#define LINE_CAP    8192          /* a TOOL/RESULT line                              */
#define MAX_STEPS   16            /* bounded ReAct loop                              */
#define NET_WAIT_MAX 3000000

/* ---- AGENTCOCKPIT-0: optional cockpit GUI seam (status + control over a SHM page) ----
 * agentd attaches the page LOOKUP-ONLY. When it is ABSENT (no cockpit running) g_cp stays
 * NULL and EVERY cockpit branch below is skipped -- so agentd behaves byte-identically to a
 * no-cockpit boot (all existing mock proofs: hostile/codetask/gui are unaffected). */
#define SYS_SHMGET_N 18
#define SYS_SHMAT_N  19
#define SYS_OPEN_N    4
#define SYS_READ_N    2
#define SYS_CLOSE_N   5
#define AGENTCOCKPIT_SHM_KEY  0x41434B50u
#define AGENTCOCKPIT_SHM_SIZE 4096u
#define AGENTCOCKPIT_MAGIC    0x41434B01u
#define AC_STATE_IDLE 0
#define AC_STATE_RUNNING 1
#define AC_STATE_CONFIRM 2
#define AC_STATE_DONE 3
#define AC_STATE_STOPPED 4
#define AC_CONFIRM_NONE 0
#define AC_CONFIRM_ALLOW 1
#define AC_CONFIRM_DENY 2
typedef struct {
    unsigned int magic, goal_seq, stop, grant_full, confirm;
    char goal[256];
    unsigned int state, step, run_seq;
    char tool[32]; char args[256]; char last[256];
} agentcockpit_shm_t;
static agentcockpit_shm_t* g_cp = 0;

static unsigned slen(const char* s){ unsigned n=0; while(s&&s[n]) n++; return n; }
static void outfd(int fd,const char* s){ long r,n=(long)slen(s);
    __asm__ volatile("syscall":"=a"(r):"a"((long)SYS_WRITE),"D"((long)fd),"S"((long)s),"d"(n):"rcx","r11","memory"); (void)r; }
static void yield(void){ _ch_sc(SYS_YIELD,0,0,0,0,0,0); }
static long getpid(void){ return _ch_sc(SYS_GETPID,0,0,0,0,0,0); }
static int eqn(const char* a,const char* b,unsigned n){ for(unsigned i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
static int streq(const char* a,const char* b){ while(*a&&*b){ if(*a!=*b) return 0; a++; b++; } return *a==*b; }
static unsigned parse_pid(const char* s){ unsigned v=0; while(*s&&*s!=':') s++; if(*s==':') s++;
    while(*s>='0'&&*s<='9'){ v=v*10+(unsigned)(*s-'0'); s++; } return v; }
static int recv_msg(int h, ch_msg_hdr* hdr, void* buf, unsigned cap){
    for(long i=0;i<10000000;i++){ int r=ch_recvmsg(h,hdr,buf,cap); if(r>=0) return 1; if(r==CH_EAGAIN){yield();continue;} return 0; } return 0; }
static void send_ack(int ctrl, unsigned long rid){ ch_msg_hdr a; a.type=MSG_ACK; a.flags=0; a.len=0; a.request_id=rid; ch_sendmsg(ctrl,&a,(const void*)0); }

typedef struct { char ifname[16]; unsigned char mac[6]; unsigned char _pad[2];
    unsigned int ip, netmask, gateway, dns; unsigned char up, dhcp_active, _pad2[6];
    unsigned long long tx_packets, rx_packets, tx_bytes, rx_bytes; } net_info_t;
static int net_ready(void){ net_info_t inf;
    for(long i=0;i<NET_WAIT_MAX;i++){ long r=_ch_sc(SYS_NET_INFO,(long)&inf,0,0,0,0,0);
        if(r==0 && inf.up && inf.ip!=0) return 1; _ch_sc(SYS_SOCK_POLL,0,0,0,0,0,0); yield(); } return 0; }
static long send_all(long fd, const char* buf, long len){ long off=0; int guard=0;
    while(off<len){ long n=_ch_sc(SYS_SEND,fd,(long)(buf+off),len-off,0,0,0);
        if(n>0){ off+=n; guard=0; continue; } if(n==EAGAIN_NEG){ yield(); if(++guard>100000) break; continue; } return n; }
    return off; }

/* read one '\n'-terminated line from the persistent socket; strip the newline.
 * returns line length (>=0), or -1 on close/error before any byte. bounded. */
static int recv_line(long fd, char* buf, int cap){
    int total=0;
    for(long it=0; it<4000000 && total<cap-1; it++){
        char c; _ch_sc(SYS_SOCK_POLL,0,0,0,0,0,0);
        long rn=_ch_sc(SYS_RECV,fd,(long)&c,1,0,0,0);
        if(rn==1){ if(c=='\n'){ buf[total]=0; return total; } if(c!='\r'){ buf[total++]=c; } continue; }
        if(rn==0) break;                       /* peer closed */
        if(rn==EAGAIN_NEG){ yield(); continue; }
        break;
    }
    buf[total]=0; return total>0?total:-1;
}

/* ---- AGENTCOCKPIT-0 helpers. All are no-ops unless a cockpit page is attached. ---- */
static void cp_attach(void){
    long id=_ch_sc(SYS_SHMGET_N,(long)AGENTCOCKPIT_SHM_KEY,(long)AGENTCOCKPIT_SHM_SIZE,0,0,0,0);
    if(id<0) return;
    long p=_ch_sc(SYS_SHMAT_N,id,0,0,0,0,0);
    if(p<=0) return;
    agentcockpit_shm_t* cp=(agentcockpit_shm_t*)p;
    if(cp->magic!=AGENTCOCKPIT_MAGIC) return;   /* page not published yet -> stay headless */
    g_cp=cp;
}
static void cp_setstr(char* dst,unsigned cap,const char* s){ unsigned i=0; for(;s&&s[i]&&i<cap-1;i++) dst[i]=s[i]; dst[i]=0; }
static int is_confirm_tool(const char* t){
    return streq(t,"remove")||streq(t,"spawn")||streq(t,"kill")||streq(t,"mouse")||streq(t,"key"); }
/* pre-mutation snapshot -> /var/snapshots/<base>.<seq> (ramfs => in-session rollback only).
 * Only when a cockpit session is active; non-fatal (logs + continues on any error). */
static void pre_snapshot(const char* path,unsigned seq){
    if(!g_cp || !path || !path[0]) return;
    static char sb[8192]; long n=0;
    long fd=_ch_sc(SYS_OPEN_N,(long)path,0,0,0,0,0);                 /* O_RDONLY */
    if(fd>=0){ n=_ch_sc(SYS_READ_N,fd,(long)sb,sizeof(sb)-1,0,0,0); if(n<0)n=0; _ch_sc(SYS_CLOSE_N,fd,0,0,0,0,0); }
    const char* base=path; for(unsigned i=0;path[i];i++) if(path[i]=='/') base=path+i+1;
    static char sp[256]; char* d=sp; const char* pre="/var/snapshots/";
    while(*pre) *d++=*pre++; const char* b=base; while(*b && d<sp+sizeof(sp)-16) *d++=*b++;
    *d++='.'; { unsigned v=seq; char t[12]; int i=0; if(!v)t[i++]='0'; while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i) *d++=t[--i]; } *d=0;
    long o=_ch_sc(SYS_OPEN_N,(long)sp,0x41,0600,0,0,0);             /* O_WRONLY|O_CREAT */
    if(o<0){ outfd(2,"AGENTD: snapshot open-fail\n"); return; }
    if(n>0) _ch_sc(SYS_WRITE,o,(long)sb,n,0,0,0);
    _ch_sc(SYS_CLOSE_N,o,0,0,0,0,0);
}
/* CONFIRM gate: 1=allow, 0=deny. Auto-allow when no cockpit / not CONFIRM-class / grant_full.
 * Else park in CONFIRM and bounded-poll the cockpit's Allow/Deny (iteration-capped per LAW:
 * any wall-clock wait needs a cap). STOP or timeout => deny (fail-safe). */
static int confirm_gate(const char* tool,const char* args,unsigned step){
    if(!g_cp || !is_confirm_tool(tool) || g_cp->grant_full) return 1;
    g_cp->state=AC_STATE_CONFIRM; g_cp->step=step;
    cp_setstr(g_cp->tool,sizeof(g_cp->tool),tool); cp_setstr(g_cp->args,sizeof(g_cp->args),args);
    g_cp->confirm=AC_CONFIRM_NONE;
    for(long it=0; it<2000000; it++){
        if(g_cp->stop) return 0;
        unsigned c=g_cp->confirm;
        if(c==AC_CONFIRM_ALLOW){ g_cp->confirm=AC_CONFIRM_NONE; g_cp->state=AC_STATE_RUNNING; return 1; }
        if(c==AC_CONFIRM_DENY){  g_cp->confirm=AC_CONFIRM_NONE; g_cp->state=AC_STATE_RUNNING; return 0; }
        yield();
    }
    g_cp->state=AC_STATE_RUNNING; return 0;
}

/* ---- whitelist + path gate ---- the FIRST line of defense: only a known tool name
 * resolves to a program; everything else is rejected before any dispatch. Each tool
 * ALSO self-gates its paths (path_write_allowed), so the policy is defended twice. */
static const char* resolve_tool(const char* name){
    /* read-only (TOOLSET-0) */
    if(streq(name,"read_file")) return "sbin/tool_read";
    if(streq(name,"list_dir"))  return "sbin/tool_ls";
    if(streq(name,"stat"))      return "sbin/tool_stat";
    if(streq(name,"ps"))        return "sbin/tool_ps";
    /* Phase 2 run-open-code (the hands) */
    if(streq(name,"write_file"))return "sbin/tool_write";
    if(streq(name,"compile"))   return "sbin/tool_cc";
    if(streq(name,"execute"))   return "sbin/tool_exec";
    /* Phase 2 files */
    if(streq(name,"mkdir"))     return "sbin/tool_mkdir";
    if(streq(name,"move"))      return "sbin/tool_mv";
    if(streq(name,"remove"))    return "sbin/tool_rm";
    if(streq(name,"rollback"))  return "sbin/tool_rollback";   /* C3: restore a file from its snapshot */
    /* Phase 2 apps/system */
    if(streq(name,"spawn"))     return "sbin/tool_spawn";
    if(streq(name,"kill"))      return "sbin/tool_kill";
    /* SECURITY: "shell" is deliberately NOT on the rail. tool_shell forwards its
     * argv[2..] UN-GATED to /bin coreutils (cc/touch/tee/head/...) which carry no
     * path policy of their own, and agentd's bad_path() only inspects av[0] (the
     * command NAME). So {"tool":"shell","args":"touch\t/etc/evil"} would defeat the
     * entire write allowlist. Re-enabling shell REQUIRES gating every /bin argument
     * that looks like a path -- until then it stays off. */
    /* Phase 4 synthetic input -- GUI takeover (CONFIRM-class in /etc/ai/policy.json).
     * Effects are inert unless the compositor has authorised injection (active=1). */
    if(streq(name,"mouse"))     return "sbin/tool_mouse";
    if(streq(name,"key"))       return "sbin/tool_key";
    return 0;                                  /* unknown/unsupported -> rejected */
}
static int bad_path(const char* p){ if(!p) return 0;       /* some tools take no path */
    for(unsigned i=0;p[i];i++) if(p[i]=='.'&&p[i+1]=='.') return 1; return 0; }

/* Parse a TOOL line body: {"tool":"<t>","args":"<a>"} with JSON-escape decoding
 * of the values (\t \n \r \" \\ \/). Returns 1 on success. */
static int json_str(const char** s, char* out, unsigned cap){
    const char* p=*s; unsigned i=0;
    if(*p++!='"') return 0;
    while(*p && *p!='"'){
        char c=*p++;
        if(c=='\\'){ char e=*p++; switch(e){ case 't':c='\t';break; case 'n':c='\n';break;
            case 'r':c='\r';break; case '"':c='"';break; case '\\':c='\\';break; case '/':c='/';break;
            default: c=e; } }
        if(i+1>=cap) return 0; out[i++]=c;
    }
    if(*p!='"') return 0; out[i]=0; *s=p+1; return 1;
}
static int parse_tool(const char* json, char* tool, unsigned tcap, char* args, unsigned acap){
    const char* s=json;
    while(*s==' ') s++;
    if(*s++!='{') return 0;
    while(*s==' '||*s==',') s++;
    if(!eqn(s,"\"tool\":",7)) return 0; s+=7; while(*s==' ')s++;
    if(!json_str(&s,tool,tcap)) return 0;
    while(*s==' '||*s==',') s++;
    if(!eqn(s,"\"args\":",7)) return 0; s+=7; while(*s==' ')s++;
    if(!json_str(&s,args,acap)) return 0;
    return 1;
}

/* ===== RUNNER child: dispatch one validated tool_run_t over the rail (P6c) ===== */
static int runner_mode(unsigned agent_pid){
    /* tool_run_t is ~3KB (v2 args). The userspace stack is tiny -- keep it in BSS
     * (single in-flight tool, single-threaded) so the frame can't overflow. */
    static tool_run_t tr;
    for(;;){
        ch_msg_hdr hdr;
        if(!recv_msg(1,&hdr,&tr,sizeof(tr))){ outfd(2,"AGENTDRUN: FAIL recv\n"); return 1; }
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
        } else { res.flags=TOOL_F_ERR; res.stdout_token=0; }
        for(long i=0;i<2000000;i++){ int w=ch_sendmsg(2,&rh,&res); if(w>=0)break; if(w==CH_EAGAIN){yield();continue;} break; }
        { ch_msg_hdr ah; char d8[8]; recv_msg(1,&ah,d8,sizeof(d8)); }
        if(out>0) ch_close(out);
    }
    return 0;
}

/* dispatch a validated tool (prog) with up to ARGV_MAX \t-split args; read its
 * stdout into buf. Returns 1 on a clean run (ec/bl set). */
static int dispatch(int ctrl, unsigned long rid, const char* prog,
                    const char* const* av, int ac, char* buf, int cap, int* bl, int* ec){
    *bl=0; *ec=-999;
    static tool_run_t tr;                                  /* ~3KB: BSS, not the tiny stack */
    tool_run_encode(&tr, prog, "");
    if(tool_run_set_argv(&tr, av, ac) != AR_OK){ *ec=-1000; return 0; }   /* args overflow the rail */
    ch_msg_hdr h; h.type=MSG_TOOL_RUN; h.flags=0; h.len=sizeof(tr); h.request_id=rid;
    if(ch_sendmsg(ctrl,&h,&tr)!=(int)(sizeof(ch_msg_hdr)+sizeof(tr))) return 0;
    ch_msg_hdr rh; tool_result_t res;
    if(!recv_msg(ctrl,&rh,&res,sizeof(res))) return 0;
    if(!(rh.type==MSG_TOOL_RESULT && rh.len==sizeof(res) && tool_result_validate(&res,rh.len)==AR_OK) ||
       (res.flags & TOOL_F_ERR)){ send_ack(ctrl,rid); return 0; }
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

static int host_mode(const char* goal){
    cp_attach();                 /* AGENTCOCKPIT-0: attach lookup-only; g_cp NULL if no cockpit */
    if(!net_ready()){ outfd(1,"AGENTD: SKIP no_net\n"); return 0; }
    long fd=_ch_sc(SYS_SOCKET,SOCK_STREAM,0,0,0,0,0);
    if(fd<0){ outfd(1,"AGENTD: SKIP no_socket\n"); return 0; }
    long cr=_ch_sc(SYS_CONNECT,fd,(long)MODEL_IP,AGENT_PORT,0,0,0);
    if(cr<0){ _ch_sc(SYS_CLOSE_SK,fd,0,0,0,0,0);
        outfd(1,"AGENTD: SKIP no_broker (run scripts/nemotron_mock.py or nemotron_broker.js)\n"); return 0; }

    /* spawn the gated tool runner (self) over a control channel */
    unsigned my_pid=(unsigned)getpid();
    int ctrl=ch_create(CH_MSG,CH_PAGE);
    if(ctrl<=0){ outfd(1,"AGENTD: FAIL ctrl\n"); _ch_sc(SYS_CLOSE_SK,fd,0,0,0,0,0); return 1; }
    char rarg[24]; { char* p=rarg; *p++='r'; *p++=':'; char t[12]; int i=0; unsigned v=my_pid;
        if(!v) t[i++]='0'; while(v){ t[i++]=(char)('0'+v%10); v/=10; } while(i) *p++=t[--i]; *p=0; }
    long pid=spawn_ex("sbin/agentd", rarg, ctrl, ctrl, 0);
    if(pid<=0){ outfd(1,"AGENTD: FAIL spawn-runner\n"); ch_close(ctrl); _ch_sc(SYS_CLOSE_SK,fd,0,0,0,0,0); return 1; }

    /* GOAL */
    send_all(fd,"GOAL ",5); send_all(fd,goal,(long)slen(goal)); send_all(fd,"\n",1);
    outfd(1,"AGENTD: GOAL sent; running gated agent loop\n");
    if(g_cp){ g_cp->run_seq=g_cp->goal_seq; g_cp->state=AC_STATE_RUNNING; g_cp->step=0; }

    /* line/args/buf are multi-KB -- keep them in BSS, NOT on the tiny userspace stack
     * (single-threaded, one tool in flight at a time, so reuse across iterations is safe). */
    static char line[LINE_CAP];
    static char args[LINE_CAP];
    unsigned long rid=1; int steps=0, done=0;
    for(int step=0; step<MAX_STEPS; step++){
        if(g_cp && g_cp->stop){ g_cp->state=AC_STATE_STOPPED; outfd(1,"AGENTD: STOP (operator)\n"); break; }
        int ll=recv_line(fd,line,sizeof(line));
        if(ll<0){ outfd(1,"AGENTD: broker closed\n"); break; }
        if(eqn(line,"DONE",4)){ outfd(1,"AGENTD: DONE "); outfd(1,line+ (line[4]==' '?5:4)); outfd(1,"\n"); done=1; break; }
        if(!eqn(line,"TOOL ",5)){ outfd(1,"AGENTD: (ignoring non-TOOL line)\n"); continue; }

        char tool[TOOL_NAME_MAX];
        if(!parse_tool(line+5,tool,sizeof(tool),args,sizeof(args))){
            send_all(fd,"RESULT [malformed tool json -- rejected]\n",41); continue; }

        /* GATE: whitelist + path policy, BEFORE any dispatch */
        const char* prog=resolve_tool(tool);
        /* split args on \t into argv[1..] (first arg is treated as the path for bad_path);
         * skip empty entries -- tool_run_set_argv rejects them and would drop ALL args. */
        const char* av[TOOL_ARGV_MAX]; int ac=0;
        { char* seg=args;
          for(int i=0; ; i++){
              if(args[i]=='\t' || args[i]==0){
                  int last=(args[i]==0); args[i]=0;
                  if(seg[0] && ac<TOOL_ARGV_MAX) av[ac++]=seg;
                  seg=&args[i+1]; if(last) break;
              }
          } }
        int gated_ok = (prog!=0 && !bad_path(ac>0?av[0]:0));
        steps++;
        if(!gated_ok){
            outfd(1,"AGENTD: DENY tool="); outfd(1,tool); outfd(1,"\n");
            send_all(fd,"RESULT [denied by policy: unknown tool or bad path]\n",52);
            continue;
        }
        outfd(1,"AGENTD: TOOL "); outfd(1,tool); outfd(1," "); outfd(1,ac>0?av[0]:""); outfd(1,"\n");
        /* cockpit status: publish the current step/tool/args (no-op without a cockpit) */
        if(g_cp){ g_cp->state=AC_STATE_RUNNING; g_cp->step=(unsigned)steps;
                  cp_setstr(g_cp->tool,sizeof(g_cp->tool),tool);
                  cp_setstr(g_cp->args,sizeof(g_cp->args),ac>0?av[0]:""); }
        /* CONFIRM gate: dangerous tools wait for the operator's Allow/Deny (unless grant_full) */
        if(!confirm_gate(tool, ac>0?av[0]:"", (unsigned)steps)){
            outfd(1,"AGENTD: CONFIRM-DENY tool="); outfd(1,tool); outfd(1,"\n");
            send_all(fd,"RESULT [denied by operator]\n",28);
            continue;
        }
        /* pre-mutation snapshot for rollback (cockpit session only) */
        if(streq(tool,"write_file")||streq(tool,"remove")) pre_snapshot(ac>0?av[0]:0,(unsigned)steps);
        static char buf[1024]; int bl=0, ec=-999;
        if(dispatch(ctrl,rid++,prog,av,ac,buf,sizeof(buf)-1,&bl,&ec)){
            if(bl<0) bl=0; if(bl>(int)sizeof(buf)-1) bl=sizeof(buf)-1; buf[bl]=0;
            /* RESULT is ONE newline-framed line: collapse embedded \n/\r in the tool
             * output to spaces so they cannot desync the broker's line parser. */
            for(int i=0;i<bl;i++) if(buf[i]=='\n'||buf[i]=='\r') buf[i]=' ';
            send_all(fd,"RESULT ",7);
            if(bl>0) send_all(fd,buf,bl);
            send_all(fd,"\n",1);
        } else if(ec==-1000){
            send_all(fd,"RESULT [tool args too large for rail]\n",38);
        } else {
            send_all(fd,"RESULT [tool execution failed]\n",31);
        }
        if(g_cp && bl>0) cp_setstr(g_cp->last,sizeof(g_cp->last),buf);  /* publish last result */
    }
    if(g_cp) g_cp->state = done ? AC_STATE_DONE : AC_STATE_STOPPED;

    { ch_msg_hdr sh; sh.type=MSG_SHUTDOWN; sh.flags=0; sh.len=0; sh.request_id=999; ch_sendmsg(ctrl,&sh,(const void*)0); }
    ch_close(ctrl); _ch_sc(SYS_CLOSE_SK,fd,0,0,0,0,0);
    outfd(1, done?"AGENTD: PASS loop_completed steps=":"AGENTD: ended steps=");
    { char t[8]; int i=0,v=steps; char o[8]; int j=0; if(!v)t[i++]='0'; while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)o[j++]=t[--i]; o[j]=0; outfd(1,o); }
    outfd(1,"\n");
    return 0;
}

int main(int argc, char** argv){
    if(argc>=2 && argv[1] && argv[1][0]=='r' && argv[1][1]==':') return runner_mode(parse_pid(argv[1]));
    const char* goal = (argc>=2 && argv[1] && argv[1][0]) ? argv[1]
                       : "List /etc, then read and report the contents of /etc/toolset0.txt.";
    return host_mode(goal);
}
