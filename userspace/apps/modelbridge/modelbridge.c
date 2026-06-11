/* modelbridge -- MODEL-BRIDGE-0: the model seam fed by an EXTERNAL endpoint.
 *
 * CHAINLAYER-HOST-0 proved the model<->host<->tool<->model plumbing with a
 * deterministic stub at the seam. This brick replaces ONLY the seam bodies:
 * model_select/model_answer now do a TCP exchange with an external model
 * endpoint (QEMU slirp guest -> 10.0.2.2:8431 -> host loopback, where
 * scripts/model_server_stub.py stands in for llama.cpp until the real brain
 * lands). EVERYTHING else is byte-for-byte the CHAINLAYER-HOST-0 trust
 * surface: the same strict one-shape JSON parser, the same TOOLSET-0
 * whitelist, the same path policy, the same read-only tools, the same
 * self-spawn runner + P6c stdout capability.
 *
 * THE MODEL IS HOSTILE TEXT, DAY ONE: every byte off the socket is untrusted.
 * The endpoint is scripted to ALSO return attacks (unparseable chatter, a
 * non-whitelisted delete_file selection) and the host gate must kill both
 * BEFORE any dispatch.
 *
 * Wire protocol (one request per connection; line in, line out, close):
 *   "SELECT <prompt>\n"      -> the model's tool-selection JSON (one line)
 *   "ANSWER <observation>\n" -> the model's final answer (one line)
 * Observation framing is SINGLE-LINE for this brick (the fixture is one
 * line); multi-line observation framing is a later brick.
 *
 * One prompt, one tool, one answer. NO loops, NO tool expansion, NO write
 * tools, NO shell. If networking or the endpoint is absent, modelbridge
 * SKIPs (bounded probes, exit 0) -- the default boot stays clean.
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

/* ---- socket syscalls (per AutomationOS ABI; see nc.c) ---- */
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
#define MODEL_PORT  8431
#define RESP_CAP    256
#define RECV_MAX    400000        /* nc.c's relay bound */
#define NET_WAIT_MAX 3000000      /* bounded wait for DHCP during the boot storm */

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

/* net_info_t mirror -- MUST match kernel net_info_ext_t (netif.h); the kernel
 * copies the FULL struct, so the buffer must be full-sized even though we
 * only read .ip / .up (same mirror as netinfo.c). */
typedef struct {
    char ifname[16];
    unsigned char mac[6]; unsigned char _pad[2];
    unsigned int ip, netmask, gateway, dns;
    unsigned char up, dhcp_active, _pad2[6];
    unsigned long long tx_packets, rx_packets, tx_bytes, rx_bytes;
} net_info_t;

/* bounded wait for a DHCP lease (autodhcp runs in parallel at boot).
 * returns 1 when the NIC is up with an IP, 0 when the bound expires. */
static int net_ready(void){
    net_info_t inf;
    for(long i=0;i<NET_WAIT_MAX;i++){
        long r=_ch_sc(SYS_NET_INFO,(long)&inf,0,0,0,0,0);
        if(r==0 && inf.up && inf.ip!=0) return 1;
        _ch_sc(SYS_SOCK_POLL,0,0,0,0,0,0);
        yield();
    }
    return 0;
}

/* bounded full-buffer send (nc.c's send_all pattern). */
static long send_all(long fd, const char* buf, long len){
    long off=0; int guard=0;
    while(off<len){
        long n=_ch_sc(SYS_SEND,fd,(long)(buf+off),len-off,0,0,0);
        if(n>0){ off+=n; guard=0; continue; }
        if(n==EAGAIN_NEG){ yield(); if(++guard>100000) break; continue; }
        return n;
    }
    return off;
}

/* ============ THE MODEL SEAM (now an EXTERNAL TCP exchange) ============
 * One request per connection: send "<VERB> <payload>\n", read the model's
 * one-line reply until the endpoint closes, strip the newline. Returns the
 * reply length (>=0), MB_NOCONN if the endpoint is unreachable, or -1 on a
 * transport failure. EVERY returned byte is UNTRUSTED MODEL TEXT -- the
 * caller's parse/whitelist/path gate decides what it is allowed to mean. */
#define MB_NOCONN (-1000)
static int model_exchange(const char* req, char* resp, int cap){
    long fd=_ch_sc(SYS_SOCKET,SOCK_STREAM,0,0,0,0,0);
    if(fd<0) return -1;
    long cr=_ch_sc(SYS_CONNECT,fd,(long)MODEL_IP,MODEL_PORT,0,0,0);   /* kernel-bounded */
    if(cr<0){ _ch_sc(SYS_CLOSE_SK,fd,0,0,0,0,0); return MB_NOCONN; }
    long sr=send_all(fd,req,(long)slen(req));
    if(sr<(long)slen(req)){ _ch_sc(SYS_CLOSE_SK,fd,0,0,0,0,0); return -1; }
    int total=0;
    for(long it=0; it<RECV_MAX; it++){
        _ch_sc(SYS_SOCK_POLL,0,0,0,0,0,0);
        long rn=_ch_sc(SYS_RECV,fd,(long)(resp+total),(long)(cap-1-total),0,0,0);
        if(rn>0){ total+=(int)rn; if(total>=cap-1) break; continue; }
        if(rn==0) break;                                  /* endpoint closed = done */
        if(rn==EAGAIN_NEG){ yield(); continue; }
        break;                                            /* hard error */
    }
    _ch_sc(SYS_CLOSE_SK,fd,0,0,0,0,0);
    while(total>0 && (resp[total-1]=='\n'||resp[total-1]=='\r')) total--;   /* strip line end */
    resp[total]=0;
    return total;
}
/* ======================================================================= */

/* strict parser for the ONE selection shape: {"tool":"<t>","path":"<p>"}
 * exact key order, no whitespace, no escapes, bounded values, no trailing
 * bytes. UNCHANGED from CHAINLAYER-HOST-0 -- any deviation is a REJECT. */
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

/* ---- the TOOLSET-0 trust surface (UNCHANGED: whitelist + path policy) ---- */
static const char* resolve_tool(const char* name){
    if(streq(name,"read_file")) return "sbin/tool_read";
    if(streq(name,"list_dir"))  return "sbin/tool_ls";
    if(streq(name,"stat"))      return "sbin/tool_stat";
    return 0;                                              /* unknown tool -> rejected */
}
static int bad_path(const char* p){ if(!p||!p[0]) return 1;       /* empty rejected */
    for(unsigned i=0;p[i];i++) if(p[i]=='.'&&p[i+1]=='.') return 1; return 0; }  /* ".." rejected */

/* ===== RUNNER (child): dispatch the validated tool until SHUTDOWN ===== */
static int runner_mode(unsigned agent_pid){
    for(;;){
        ch_msg_hdr hdr; tool_run_t tr;
        if(!recv_msg(1,&hdr,&tr,sizeof(tr))){ outfd(2,"MBRUNNER: FAIL recv\n"); return 1; }
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
    outfd(2,"MBRUNNER: shutdown\n");
    return 0;
}

/* dispatch ONE validated tool over the rail; read its stdout into buf.
 * UNCHANGED from CHAINLAYER-HOST-0. */
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

/* ===== HOST (parent): one prompt -> one external decision, every boundary checked ===== */
static int host_mode(void){
    /* 0. bounded probes: no net or no endpoint -> SKIP, default boot stays clean */
    if(!net_ready()){ outfd(1,"MODELBRIDGE: SKIP no_net\n"); return 0; }
    char resp[RESP_CAP];
    int rl = model_exchange("SELECT What is inside /etc/toolset0.txt?\n", resp, sizeof(resp));
    if(rl==MB_NOCONN){ outfd(1,"MODELBRIDGE: SKIP no_endpoint\n"); return 0; }

    unsigned my_pid=(unsigned)getpid();
    int ctrl=ch_create(CH_MSG,CH_PAGE);
    if(ctrl<=0){ outfd(1,"MODELBRIDGE: FAIL ctrl\n"); return 1; }
    char arg[24]; { char* p=arg; *p++='r'; *p++=':'; char t[12]; int i=0; unsigned v=my_pid;
        if(!v) t[i++]='0'; while(v){ t[i++]=(char)('0'+v%10); v/=10; } while(i) *p++=t[--i]; *p=0; }
    long pid=spawn_ex("sbin/modelbridge", arg, ctrl, ctrl, 0);
    if(pid<=0){ outfd(1,"MODELBRIDGE: FAIL spawn-runner\n"); ch_close(ctrl); return 1; }

    /* 1. the EXTERNAL model's selection (already fetched): strict-parse it */
    char tool[TOOL_NAME_MAX]; char path[TOOL_PATH_MAX]; tool[0]=0; path[0]=0;
    int select_parse = (rl>0 && parse_selection(resp,tool,sizeof(tool),path,sizeof(path)));

    /* 2. the host policy gate: whitelist + path policy (unchanged) */
    const char* prog = select_parse ? resolve_tool(tool) : 0;
    int policy_ok = (prog!=0 && !bad_path(path) && streq(tool,"read_file"));

    /* 3. the validated tool runs over the rail; exact stdout via the P6c token */
    char buf[256]; int bl=0, ec=-999; unsigned long rid=1;
    int read_exact=0;
    if(policy_ok && dispatch(ctrl,rid++,prog,path,buf,sizeof(buf),&bl,&ec))
        read_exact = (ec==0 && bl==15 && eqn(buf,"TOOLSET-0-FILE\n",15));

    /* 4. send the observation back; the EXTERNAL model answers (single-line framing) */
    char answer[RESP_CAP]; answer[0]=0;
    int answer_exact=0;
    if(read_exact){
        int al = model_exchange("ANSWER TOOLSET-0-FILE\n", answer, sizeof(answer));
        answer_exact = (al>0 && streq(answer,"TOOLSET-0-FILE"));
    }

    /* 5. HOSTILE model output, scripted at the endpoint, must die at the gate:
     *    (a) unparseable chatter -> the strict parser rejects (no dispatch) */
    char t2[TOOL_NAME_MAX], p2[TOOL_PATH_MAX];
    int malformed_rej=0;
    { char r2[RESP_CAP];
      int l2=model_exchange("SELECT __malformed__\n", r2, sizeof(r2));
      malformed_rej = (l2>0 && !parse_selection(r2,t2,sizeof(t2),p2,sizeof(p2))); }
    /*    (b) valid shape, NON-WHITELISTED tool -> the whitelist rejects (no dispatch) */
    int badtool_rej=0;
    { char r3[RESP_CAP];
      int l3=model_exchange("SELECT __badtool__\n", r3, sizeof(r3));
      badtool_rej = (l3>0 && parse_selection(r3,t2,sizeof(t2),p2,sizeof(p2)) && resolve_tool(t2)==0); }

    { ch_msg_hdr sh; sh.type=MSG_SHUTDOWN; sh.flags=0; sh.len=0; sh.request_id=999; ch_sendmsg(ctrl,&sh,(const void*)0); }

    int ok = (select_parse && policy_ok && read_exact && answer_exact && malformed_rej && badtool_rej);
    outfd(1,"MODELBRIDGE: "); outfd(1, ok?"PASS":"FAIL");
    outfd(1," select_parse=");            outfd(1, select_parse?"1":"0");
    outfd(1," policy_ok=");               outfd(1, policy_ok?"1":"0");
    outfd(1," read_exact=");              outfd(1, read_exact?"1":"0");
    outfd(1," answer_exact=");            outfd(1, answer_exact?"1":"0");
    outfd(1," malformed_model_rejected=");outfd(1, malformed_rej?"1":"0");
    outfd(1," bad_tool_rejected=");       outfd(1, badtool_rej?"1":"0");
    outfd(1,"\n");
    ch_close(ctrl);
    return ok?0:1;
}

int main(int argc, char** argv){
    if(argc>=2 && argv[1] && argv[1][0]=='r') return runner_mode(parse_pid(argv[1]));
    return host_mode();
}
