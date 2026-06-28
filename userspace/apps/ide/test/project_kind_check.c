/*
 * project_kind_check.c -- HOST-side proof that project.json carries the `kind`
 * field through load + round-trip, so the IDE can tell a prebuilt/native game
 * project (Build skips cc, Run launches run_target) from a normal compiled one.
 *
 * NOT shipped. Links the REAL ide_project.c; stubs the 4 ide_sys functions it
 * needs (file IO + tiny string helpers) against host fopen/strings, so we test
 * the actual manifest parser/writer the OS uses. Run (from repo root, Arch WSL):
 *   gcc -I userspace/apps/ide -w -o /tmp/project_kind_check \
 *       userspace/apps/ide/test/project_kind_check.c userspace/apps/ide/ide_project.c
 *   /tmp/project_kind_check
 */
#include <stdio.h>
#include <string.h>
#include "../ide_project.h"

/* ---- ide_sys stubs (host-backed) ---- */
int  ide_strlen(const char* s){ int n=0; if(!s)return 0; while(s[n])n++; return n; }
int  ide_streq(const char* a,const char* b){ if(!a||!b)return a==b; return strcmp(a,b)==0; }
int  ide_strneq(const char* a,const char* b,int n){ return strncmp(a,b,(size_t)n)==0; }
void ide_strlcpy(char* d,const char* s,int cap){ if(!d||cap<=0)return; int i=0; if(s) while(s[i]&&i<cap-1){d[i]=s[i];i++;} d[i]=0; }
int  ide_read_file(const char* path,char* buf,int cap){
    FILE* f=fopen(path,"rb"); if(!f) return -1;
    int n=(int)fread(buf,1,(size_t)cap,f); fclose(f); return n;
}
int  ide_write_file(const char* path,const char* buf,int len){
    FILE* f=fopen(path,"wb"); if(!f) return -1;
    int n=(int)fwrite(buf,1,(size_t)len,f); fclose(f); return n;
}

static int g_pass=0,g_fail=0;
static void ck(int c,const char* w){ if(c){g_pass++;printf("  [PASS] %s\n",w);} else {g_fail++;printf("  [FAIL] %s\n",w);} }

int main(void){
    const char* root="/tmp/dzproj_test";   /* caller pre-creates this dir */
    /* arrange: a DeadZone-style manifest with kind=prebuilt */
    { FILE* f=fopen("/tmp/dzproj_test/project.json","wb");
      if(!f){ printf("  [FAIL] cannot create test manifest\n"); return 2; }
      const char* m="name=DeadZone\nlang=c\nentry=src/deadzone.c\nrun_target=build/deadzone.elf\nkind=prebuilt\n";
      fwrite(m,1,strlen(m),f); fclose(f);
    }

    printf("### project.json kind round-trip ###\n");

    IdeProject p; memset(&p,0,sizeof p);
    int r=ide_project_load(&p,root);
    ck(r==0, "ide_project_load read the manifest");
    ck(ide_streq(p.name,"DeadZone"), "name=DeadZone");
    ck(ide_streq(p.entry,"src/deadzone.c"), "entry=src/deadzone.c");
    ck(ide_streq(p.run_target,"build/deadzone.elf"), "run_target=build/deadzone.elf");
    ck(ide_streq(p.kind,"prebuilt"), "kind=prebuilt PARSED (the new field)");

    /* round-trip: write then reload, kind must survive */
    ck(ide_project_write_manifest(&p)>=0, "write_manifest ok");
    IdeProject q; memset(&q,0,sizeof q);
    ide_project_load(&q,root);
    ck(ide_streq(q.kind,"prebuilt"), "kind survives write->reload");
    ck(ide_streq(q.run_target,"build/deadzone.elf"), "run_target survives write->reload");

    /* a manifest with NO kind line -> kind defaults empty (compiled) */
    { FILE* f=fopen("/tmp/dzproj_test/project.json","wb");
      const char* m="name=Hello\nlang=c\nentry=src/main.c\nrun_target=build/Hello.elf\n";
      fwrite(m,1,strlen(m),f); fclose(f);
    }
    IdeProject c; memset(&c,0,sizeof c);
    ide_project_load(&c,root);
    ck(c.kind[0]==0, "absent kind -> empty (compiled) default");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    printf("%s\n", g_fail==0 ? "PROJECT-KIND: PASS" : "PROJECT-KIND: FAIL");
    return g_fail==0?0:1;
}
