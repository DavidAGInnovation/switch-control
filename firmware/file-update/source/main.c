/* One service at a time. The host verifies TLS + files before authenticating
 * commit on temporary TCP 6015. Timeout, PLUS or authenticated rollback restores
 * the exact previous executable. No keys are transmitted or changed. */
#include <switch.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "payloads.h"
static const u64 program=0x430000000000000DULL;
static const char *exe="sdmc:/atmosphere/contents/430000000000000D/exefs.nsp";
static const char *stage="sdmc:/atmosphere/contents/430000000000000D/exefs.file-update.part";
#ifndef UPDATE_PREVIOUS_PATH
#define UPDATE_PREVIOUS_PATH "sdmc:/diagnostic-tools/2026-11-16/switch-control-before-files.nsp"
#endif
#ifndef UPDATE_RECOVERY_PATH
#define UPDATE_RECOVERY_PATH "sdmc:/diagnostic-tools/2026-11-16/switch-control-recovery-v3.nro"
#endif
#ifndef UPDATE_SELF_NAME
#define UPDATE_SELF_NAME "switch-control-file-update.nro"
#endif
static const char *previous=UPDATE_PREVIOUS_PATH;
static const char *recovery=UPDATE_RECOVERY_PATH;
static bool absent(const char *p){struct stat st;return lstat(p,&st)&&errno==ENOENT;}
static bool hash_ok(const char *path,const unsigned char hash[32]) {
    FILE *f=fopen(path,"rb");if(!f)return false;Sha256Context c;sha256ContextCreate(&c);unsigned char b[8192],out[32];size_t n;
    while((n=fread(b,1,sizeof(b),f)))sha256ContextUpdate(&c,b,n);
    bool ok=!ferror(f);if(fclose(f))ok=false;sha256ContextGetHash(&c,out);return ok&&!memcmp(out,hash,32);
}
static bool write_new(const char *path,const unsigned char *bytes,size_t size,const unsigned char hash[32]) {
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return false;bool ok=true;size_t done=0;
    while(done<size){ssize_t n=write(fd,bytes+done,size-done);if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}done+=(size_t)n;}
    if(fsync(fd))ok=false;
    if(close(fd))ok=false;
    if(!ok){unlink(path);return false;}return hash_ok(path,hash);
}
static bool state(u64 id,bool *live) {
    u64 pid=0,actual=0;Result rc=pmdmntGetProcessId(&pid,id);
    if(rc==0x20f){*live=false;return true;}
    if(R_FAILED(rc)||R_FAILED(pmdmntGetProgramId(&actual,pid))||actual!=id)return false;
    *live=true;return true;
}
static bool stop(void) {
    bool live=false;if(!state(program,&live))return false;if(!live)return true;
    if(R_FAILED(pmshellTerminateProgram(program)))return false;
    for(int i=0;i<100;i++){svcSleepThread(50000000ULL);if(!state(program,&live))return false;if(!live)return true;}return false;
}
static bool start(void) {
    u64 pid=0,actual=0;NcmProgramLocation location={.program_id=program,.storageID=NcmStorageId_None};
    return R_SUCCEEDED(pmshellLaunchProgram(0,&location,&pid))&&R_SUCCEEDED(pmdmntGetProgramId(&actual,pid))&&actual==program;
}
static bool restore(void) {
    if(!hash_ok(previous,old_hash)||!stop())return false;
    if(!absent(exe)&&(!hash_ok(exe,new_hash)||unlink(exe)))return false;
    return !rename(previous,exe)&&hash_ok(exe,old_hash)&&start();
}
static bool boot_enabled(void){FILE *f=fopen("sdmc:/atmosphere/contents/430000000000000D/flags/boot2.flag","rb");if(!f)return false;bool ok=fgetc(f)==EOF&&!ferror(f);fclose(f);return ok;}
static int listener(void) {
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -1;int one=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(6015),.sin_addr.s_addr=htonl(INADDR_ANY)};
    if(bind(fd,(void*)&address,sizeof(address))||listen(fd,1)||fcntl(fd,F_SETFL,O_NONBLOCK)){close(fd);return -1;}return fd;
}
static int decision(int server,const unsigned char key[32]) {
    int fd=accept(server,NULL,NULL);if(fd<0)return 0;
    if(fcntl(fd,F_SETFL,0)){close(fd);return 0;}
    struct timeval timeout={.tv_sec=2};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    unsigned char nonce[32],packet[40],mac[32],message[40];randomGet(nonce,sizeof(nonce));int result=0;
    if(send(fd,nonce,32,0)==32){size_t used=0;while(used<sizeof(packet)){ssize_t n=recv(fd,packet+used,sizeof(packet)-used,0);if(n<=0)break;used+=(size_t)n;}
        /* Eight-byte operation, then HMAC(nonce || operation). */
        if(used==40){memcpy(message,nonce,32);memcpy(message+32,packet,8);hmacSha256CalculateMac(mac,key,32,message,sizeof(message));unsigned diff=0;for(size_t i=0;i<32;i++)diff|=mac[i]^packet[8+i];
            if(!diff){if(!memcmp(packet,"commit\0\0",8))result=1;else if(!memcmp(packet,"rollback",8))result=-1;}}
    }
    const char *out=result==1?"COMMIT\n":result==-1?"ROLLBACK\n":"REJECTED\n";send(fd,out,strlen(out),0);close(fd);return result;
}
int main(void) {
    consoleInit(NULL);appletSetAutoSleepDisabled(true);padConfigureInput(1,HidNpadStyleSet_NpadStandard);PadState pad;padInitializeDefault(&pad);
    puts("Switch Control: native HTTPS / MCP update\nGame must be closed. Keys and boot flags stay in place.\n");
    Result dm=pmdmntInitialize(),pm=pmshellInitialize(),net=socketInitializeDefault();int server=-1;bool pending=false,committed=false,moved=false;const char *result="NO CHANGES";unsigned char key[32]={0};
    if(R_FAILED(dm)||R_FAILED(pm)||R_FAILED(net)){printf("Initialization: dm=%08X pm=%08X net=%08X\n",dm,pm,net);goto done;}
    u64 app=0;if(pmdmntGetApplicationProcessId(&app)!=0x20f){result="APPLICATION OPEN; NO CHANGES";goto done;}
    for(unsigned i=0;i<3;i++){const u64 ids[]={0x430000000000000BULL,0x430000000000000CULL,0x430000000000000EULL};bool live;if(!state(ids[i],&live)||live){result="PEER SERVICE PRESENT; NO CHANGES";goto done;}}
    if(!hash_ok(exe,old_hash)||!boot_enabled()||!absent(stage)||!absent(previous)||(!absent(recovery)&&!hash_ok(recovery,recovery_hash))){result="GUARD MISMATCH; NO CHANGES";goto done;}
    FILE *f=fopen("sdmc:/config/za-usb-control/wifi.key","rb");if(!f){result="AUTHENTICATION KEY UNAVAILABLE; NO CHANGES";goto done;}size_t count=fread(key,1,32,f);bool key_ok=count==32&&fgetc(f)==EOF&&!ferror(f);fclose(f);if(!key_ok){result="AUTHENTICATION KEY INVALID; NO CHANGES";goto done;}
    server=listener();if(server<0){result="HEALTH LISTENER FAILED; NO CHANGES";goto done;}
    if(absent(recovery)&&!write_new(recovery,recovery_bytes,sizeof(recovery_bytes),recovery_hash)){result="RECOVERY PREPARATION FAILED; SERVICE UNCHANGED";goto done;}
    if(!write_new(stage,new_bytes,sizeof(new_bytes),new_hash)){result="STAGING FAILED; SERVICE UNCHANGED";goto done;}
    puts("Recovery and staged service verified. Stopping prior service...");consoleUpdate(NULL);
    if(!stop()){result="STOP FAILED; EXECUTABLE UNCHANGED";goto done;}
    if(rename(exe,previous)){result=start()?"RETAIN FAILED; PRIOR SERVICE RESTARTED":"RETAIN FAILED; RESTART NEEDS ATTENTION";goto done;}moved=true;
    if(rename(stage,exe)||!hash_ok(exe,new_hash)||!start()){result=restore()?"INSTALL FAILED; PRIOR SERVICE RESTORED":"RECOVERY REQUIRED";goto done;}pending=true;
    puts("New service started. Waiting up to 120s for authenticated host health check.");
    u64 began=svcGetSystemTick();
    while(appletMainLoop()&&armTicksToNs(svcGetSystemTick()-began)<120000000000ULL){padUpdate(&pad);int action=decision(server,key);if(action==1&&hash_ok(exe,new_hash)&&hash_ok(recovery,recovery_hash)){committed=true;break;}if(action==-1||(padGetButtonsDown(&pad)&HidNpadButton_Plus))break;consoleUpdate(NULL);svcSleepThread(5000000ULL);}
    result=committed?"VERIFIED: TLS + SD FILE HEALTH CHECK COMMITTED":restore()?"HEALTH CHECK NOT COMMITTED; PRIOR SERVICE RESTORED":"RECOVERY REQUIRED";
 done:
    memset(key,0,sizeof(key));if(server>=0)close(server);if(!pending&&absent(exe)&&moved)(void)restore();
    if(!absent(stage)&&hash_ok(stage,new_hash))unlink(stage);
    if(R_SUCCEEDED(pm))pmshellExit();
    if(R_SUCCEEDED(dm))pmdmntExit();
    if(R_SUCCEEDED(net))socketExit();
    puts(result);
    if(committed){if(!rename("sdmc:/switch/" UPDATE_SELF_NAME,"sdmc:/diagnostic-tools/2026-11-16/" UPDATE_SELF_NAME))puts("Updater archived outside Homebrew.");}
    puts("Press + to exit.");while(appletMainLoop()){padUpdate(&pad);if(padGetButtonsDown(&pad)&HidNpadButton_Plus)break;consoleUpdate(NULL);}
    appletSetAutoSleepDisabled(false);consoleExit(NULL);return 0;
}
