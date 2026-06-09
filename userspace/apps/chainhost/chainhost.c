/* chainhost -- CHAINLAYER-HOST-0: the first model-in-the-loop tool decision.
 *
 *   prompt -> MODEL chooses a tool (JSON) -> HOST parses + validates it
 *     -> tool runs safely over the AGENT-RPC-0 rail -> MODEL sees the result
 *     -> MODEL makes the final decision -> host checks it.
 *
 * THE MODEL SEAM (the whole point of this brick): model_select() and
 * model_answer() are the ONLY two places a model touches this program. In
 * CHAINLAYER-HOST-0 they are DETERMINISTIC STUBS -- AutomationOS is freestanding,
 * the real chainlayer2 brain is an EXTERNAL llama.cpp/GGUF host; it plugs into
 * this exact seam later (same JSON in, same observation out). Everything model-
 * emitted is UNTRUSTED TEXT: the host strictly parses the one JSON shape
 * {"tool":"...","path":"..."} and re-validates name+path with the TOOLSET-0
 * trust surface (whitelist + traversal denial) BEFORE anything is dispatched.
 *
 * One prompt, one selection shape, one tool run, one final decision.
 * NO autonomous loops, NO write/delete tools, NO networking, NO recursive
 * planning, NO self-modifying code, NO new tools (TOOLSET-0 whitelist only).
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

/* ================== THE MODEL SEAM (deterministic stub) ==================
 * Contract: model_select(prompt) returns the tool-selection JSON as text.
 * model_answer(observation) returns the final answer as text. A real model
 * replaces ONLY these two bodies; the host around them never trusts either. */
static const char* model_select(const char* prompt){
    /* the stub "reasons": the prompt asks what is inside a file -> read it. */
    if(streq(prompt,"What is inside /etc/toolset0.txt?"))
        return "{\"tool\":\"read_file\",\"path\":\"/etc/toolset0.txt\"}";
    return "{\"tool\":\"none\",\"path\":\"\"}";
}
static void model_answer(const char* obs, unsigned obs_len, char* out, unsigned cap){
    /* the stub "answers": the file's content IS the answer (first line, no \n). */
    unsigned i=0; while(i<obs_len && obs[i]!='\n' && i+1<cap){ out[i]=obs[i]; i++; } out[i]=0;
}
/* ========================================================================= */

/* strict parser for the ONE selection shape: {"tool":"<t>","path":"<p>"}
 * exact key order, no whitespace, no escapes, bounded values. Model output is
 * untrusted -- any deviation from the shape is a parse REJECT (return 0). */
static int expect(const char** s, const char* lit){ unsigned n=slen(lit);
    if(!eqn(*s,lit,n)) return 0; if(slen(*s)<n) return 0; *s+=n; return 1; }
static int take_str(const char** s, char* out, unsigned cap){
    const char* p=*s; unsigned i=0;
    if(*p++!='"') return 0;
    while(*p && *p!='"'){ if(*p=='\\') return 0; if(i+1>=cap) return 0; out[i++]=*p++; }
    if(*p!='"') return 0; out[i]=0; *s=p+1; return 1; }
static int parse_selection(const char* json, char* tool, unsigned tcap, char* path, unsigned pcap){
    const char* s=json;
    if(!expect(&s,"{\"tool\":"))   return 0;
    if(!take_str(&s,tool,tcap))    return 0;
    if(!expect(&s,",\"path\":"))   return 0;
    if(!take_str(&s,path,pcap))    return 0;
    if(!expect(&s,"}"))            return 0;
    return *s==0;                                          /* no trailing bytes */
}

/* ---- the TOOLSET-0 trust surface (host-side; same policy as toolset_host) ---- */
static const char* resolve_tool(const char* name){
    if(streq(name,"read_file")) return "sbin/tool_read";
    if(streq(name,"list_dir"))  return "sbin/tool_ls";
    if(streq(name,"stat"))      return "sbin/tool_stat";
    return 0;                                              /* unknown tool -> rejected */
}
static int bad_path(const char* p){ if(!p||!p[0]) return 1;       /* empty rejected */
    for(unsigned i=0;p[i];i++) if(p[i]=='.'&&p[i+1]=='.') return 1; return 0; }  /* ".." rejected */

/* host gate: parse the model's UNTRUSTED selection, then validate name+path.
 * returns the whitelisted program path, or 0 (rejected, nothing dispatched). */
static const char* validate_selection(const char* json, char* tool, unsigned tcap, char* path, unsigned pcap){
    if(!parse_selection(json,tool,tcap,path,pcap)) return 0;
    const char* prog = resolve_tool(tool);
    if(!prog) return 0;
    if(bad_path(path)) return 0;
    return prog;
}

/* ===== RUNNER (child): dispatch the validated tool until SHUTDOWN ===== */
static int runner_mode(unsigned agent_pid){
    for(;;){
        ch_msg_hdr hdr; tool_run_t tr;
        if(!recv_msg(1,&hdr,&tr,sizeof(tr))){ outfd(2,"CHRUNNER: FAIL recv\n"); return 1; }
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
        { ch_msg_hdr ah; char d8[8]; recv_msg(1,&ah,d8,sizeof(d8)); }   /* wait the host's ACK */
        if(out>0) ch_close(out);
    }
    outfd(2,"CHRUNNER: shutdown\n");
    return 0;
}

/* dispatch ONE validated tool over the rail; read its stdout into buf. */
static int dispatch(int ctrl, unsigned long rid, const char* prog, const char* arg,
                    char* buf, int cap, int* bl, int* ec){
    *bl=0; *ec=-999;
    tool_run_t tr; tool_run_encode(&tr, prog, "");
    const char* av[1]={arg}; tool_run_set_argv(&tr, av, 1);
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

/* ===== HOST (parent): one prompt -> one decision, every boundary checked ===== */
static int host_mode(void){
    unsigned my_pid=(unsigned)getpid();
    int ctrl=ch_create(CH_MSG,CH_PAGE);
    if(ctrl<=0){ outfd(1,"CHAINHOST: FAIL ctrl\n"); return 1; }
    char arg[24]; { char* p=arg; *p++='r'; *p++=':'; char t[12]; int i=0; unsigned v=my_pid;
        if(!v) t[i++]='0'; while(v){ t[i++]=(char)('0'+v%10); v/=10; } while(i) *p++=t[--i]; *p=0; }
    long pid=spawn_ex("sbin/chainhost", arg, ctrl, ctrl, 0);
    if(pid<=0){ outfd(1,"CHAINHOST: FAIL spawn-runner\n"); ch_close(ctrl); return 1; }

    /* 1. the prompt -> the MODEL selects a tool (untrusted JSON text) */
    const char* prompt = "What is inside /etc/toolset0.txt?";
    const char* json = model_select(prompt);

    /* 2. the HOST parses + validates the selection (whitelist + path policy) */
    char tool[TOOL_NAME_MAX]; char path[TOOL_PATH_MAX]; tool[0]=0; path[0]=0;
    const char* prog = validate_selection(json, tool, sizeof(tool), path, sizeof(path));
    int sel_read_file = (prog && streq(tool,"read_file"));
    int policy_ok = (prog != 0);

    /* 3. the validated tool runs over the rail; the host reads the EXACT stdout */
    char buf[256]; int bl=0, ec=-999; unsigned long rid=1;
    int read_exact=0;
    if(prog && dispatch(ctrl,rid++,prog,path,buf,sizeof(buf),&bl,&ec))
        read_exact = (ec==0 && bl==15 && eqn(buf,"TOOLSET-0-FILE\n",15));

    /* 4. the MODEL sees the observation and makes the final decision */
    char answer[64]; answer[0]=0;
    if(read_exact) model_answer(buf,(unsigned)bl,answer,sizeof(answer));
    int answer_exact = streq(answer,"TOOLSET-0-FILE");

    /* 5. a BAD model selection is rejected by the host BEFORE any dispatch:
     *    an unknown tool, a traversal path, and a shape violation -- all three
     *    must die at the gate (validate_selection==0, nothing spawned). */
    char t2[TOOL_NAME_MAX], p2[TOOL_PATH_MAX];
    int rej_unknown = (validate_selection("{\"tool\":\"delete_file\",\"path\":\"/etc/toolset0.txt\"}",t2,sizeof(t2),p2,sizeof(p2))==0);
    int rej_traversal = (validate_selection("{\"tool\":\"read_file\",\"path\":\"../etc/toolset0.txt\"}",t2,sizeof(t2),p2,sizeof(p2))==0);
    int rej_shape = (validate_selection("{\"tool\":\"read_file\",\"path\":\"/etc/toolset0.txt\"} rm -rf",t2,sizeof(t2),p2,sizeof(p2))==0);
    int rejected_bad = (rej_unknown && rej_traversal && rej_shape);

    { ch_msg_hdr sh; sh.type=MSG_SHUTDOWN; sh.flags=0; sh.len=0; sh.request_id=999; ch_sendmsg(ctrl,&sh,(const void*)0); }

    int ok = (sel_read_file && policy_ok && read_exact && answer_exact && rejected_bad);
    outfd(1,"CHAINHOST: "); outfd(1, ok?"PASS":"FAIL");
    outfd(1," selected_tool=");      outfd(1, tool[0]?tool:"none");
    outfd(1," policy_ok=");          outfd(1, policy_ok?"1":"0");
    outfd(1," read_exact=");         outfd(1, read_exact?"1":"0");
    outfd(1," model_answer_exact="); outfd(1, answer_exact?"1":"0");
    outfd(1," rejected_bad_tool=");  outfd(1, rejected_bad?"1":"0");
    outfd(1,"\n");
    ch_close(ctrl);
    return ok?0:1;
}

int main(int argc, char** argv){
    if(argc>=2 && argv[1] && argv[1][0]=='r') return runner_mode(parse_pid(argv[1]));
    return host_mode();
}
