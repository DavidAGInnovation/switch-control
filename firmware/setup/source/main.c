#include <switch.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
static bool read_exact(const char *path,void *data,size_t n){FILE *f=fopen(path,"rb");if(!f)return false;bool ok=fread(data,1,n,f)==n&&fgetc(f)==EOF&&!ferror(f);fclose(f);return ok;}
static bool create(const char *path,const void *data,size_t n){int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return false;bool ok=write(fd,data,n)==(ssize_t)n&&!fsync(fd);if(close(fd))ok=false;if(!ok)unlink(path);return ok;}
static bool provision(void){
    unsigned char a[32],b[32];const char *first="sdmc:/config/za-usb-control/wifi.key",*second="sdmc:/config/za-usb-control/tls.key";
    if(read_exact(first,a,32)&&read_exact(second,b,32)){memset(a,0,32);memset(b,0,32);return true;}
    if(access(first,F_OK)==0||access(second,F_OK)==0)return false;
    randomGet(a,32);randomGet(b,32);bool ok=create(first,a,32);if(ok&&!create(second,b,32)){unlink(first);ok=false;}memset(a,0,32);memset(b,0,32);return ok;
}
static bool start_service(void){Result dm=pmdmntInitialize(),pm=pmshellInitialize();bool ok=false;u64 pid=0;const u64 id=0x430000000000000DULL;if(R_SUCCEEDED(dm)&&R_SUCCEEDED(pm)){
    Result rc=pmdmntGetProcessId(&pid,id);if(R_SUCCEEDED(rc)){if(R_FAILED(pmshellTerminateProgram(id)))goto done;for(unsigned i=0;i<100;i++){svcSleepThread(50000000ULL);rc=pmdmntGetProcessId(&pid,id);if(rc==0x20f)break;}}
    if(rc==0x20f){NcmProgramLocation location={.program_id=id,.storageID=NcmStorageId_None};ok=R_SUCCEEDED(pmshellLaunchProgram(0,&location,&pid));}}
 done:if(R_SUCCEEDED(pm))pmshellExit();if(R_SUCCEEDED(dm))pmdmntExit();return ok;}
int main(void){consoleInit(NULL);padConfigureInput(1,HidNpadStyleSet_NpadStandard);PadState pad;padInitializeDefault(&pad);bool show=false;char message[128]="A: provision missing keys and restart service. X: show/hide bearer token.";mkdir("sdmc:/config",0777);mkdir("sdmc:/config/za-usb-control",0700);Result net=nifmInitialize(NifmServiceType_User);
    while(appletMainLoop()){padUpdate(&pad);u64 down=padGetButtonsDown(&pad);if(down&HidNpadButton_Plus)break;if(down&HidNpadButton_X)show=!show;if(down&HidNpadButton_A)snprintf(message,sizeof(message),"%s",provision()&&start_service()?"Service started. Wait a moment for certificate creation.":"Setup failed; existing keys were preserved.");
        consoleClear();puts("Switch Control Setup v0.5.1\n\nDirect HTTPS REST + MCP; no computer bridge required.\n");char id[13]={0};if(read_exact("sdmc:/config/za-usb-control/device.id",id,12))printf("API:     https://switch-%s.local:7443/\nMCP:     https://switch-%s.local:7443/mcp\n",id,id);u32 ip=0;if(R_SUCCEEDED(net)&&R_SUCCEEDED(nifmGetCurrentIpAddress(&ip))){struct in_addr a={.s_addr=ip};printf("IP:      https://%s:7443/\n",inet_ntoa(a));}
        FILE *f=fopen("sdmc:/config/za-usb-control/api-cert.der","rb");if(f){Sha256Context hash;sha256ContextCreate(&hash);unsigned char block[1024],digest[32];size_t n;while((n=fread(block,1,sizeof(block),f)))sha256ContextUpdate(&hash,block,n);fclose(f);sha256ContextGetHash(&hash,digest);puts("\nVerify certificate SHA-256 on your client:");for(unsigned i=0;i<32;i++)printf("%02x%s",digest[i],i==15?"\n":"");puts("");}
        if(show){unsigned char key[32];if(read_exact("sdmc:/config/za-usb-control/wifi.key",key,32)){puts("\nBearer token (private; do not share):");for(unsigned i=0;i<32;i++)printf("%02x%s",key[i],i==15?"\n":"");puts("");memset(key,0,sizeof(key));}}
        printf("\n%s\n\n+: exit\n",message);consoleUpdate(NULL);svcSleepThread(200000000ULL);
    }if(R_SUCCEEDED(net))nifmExit();consoleExit(NULL);return 0;}
