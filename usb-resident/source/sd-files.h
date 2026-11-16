#pragma once
/* SD-only file API. The caller authenticates the entire connection with TLS
 * and the command MAC before calling here. Binary chunks stay inside TLS.
 * No save-data or NAND mounts. Replacement preserves a recoverable prior file
 * across Horizon's non-overwriting rename; power loss is not atomic. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef SD_ROOT
#define SD_ROOT "sdmc:"
#endif
#define SD_PATH 768
#define SD_CHUNK 65536
#define SD_MAX_FILE (8ULL*1024*1024*1024)
#ifndef SD_SYNC_LIMIT
#define SD_SYNC_LIMIT (1024ULL*1024)
#endif
#ifndef SD_COOPERATE
#define SD_COOPERATE() ((void)0)
#endif
#ifdef SD_HOST_TEST
#include <openssl/sha.h>
typedef SHA256_CTX SdHash;
#define sd_hash_init SHA256_Init
#define sd_hash_update SHA256_Update
#define sd_hash_finish(out,ctx) SHA256_Final(out,ctx)
#else
typedef Sha256Context SdHash;
#define sd_hash_init sha256ContextCreate
#define sd_hash_update sha256ContextUpdate
#define sd_hash_finish(out,ctx) sha256ContextGetHash(ctx,out)
#endif
/* Supplied by the transport / host fixture. */
static bool sd_send(int fd,const void *data,size_t n);
static bool sd_receive(int fd,void *data,size_t n);
static void sd_random(void *data,size_t n);
static bool sd_mutation_allowed(const char *path);
static unsigned char sd_buffer[SD_CHUNK];
static struct {
    bool active;
    FILE *file;
    char token[33],path[SD_PATH],stage[SD_PATH],previous[SD_PATH];
    char wanted[65],expected[65];
    uint64_t size,written;
    SdHash hash;
} sd_upload;

static bool sd_reply(int fd,const char *text){return sd_send(fd,text,strlen(text));}
static bool sd_error(int fd,const char *code) {
    char out[160];snprintf(out,sizeof(out),"{\"error\":\"%s\",\"errno\":%d}\n",code,errno);return sd_reply(fd,out);
}
static int sd_nibble(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static void sd_hex(char *out,const unsigned char *b,size_t n){const char *h="0123456789abcdef";for(size_t i=0;i<n;i++){out[2*i]=h[b[i]>>4];out[2*i+1]=h[b[i]&15];}out[2*n]=0;}
static bool sd_digest(const char *text) {if(strlen(text)!=64)return false;for(size_t i=0;i<64;i++)if(sd_nibble(text[i])<0)return false;return true;}
static bool sd_number(const char *s,uint64_t *out) {
    if(!s||!*s)return false;
    uint64_t n=0;for(;*s;s++){if(*s<'0'||*s>'9'||n>(UINT64_MAX-(*s-'0'))/10)return false;n=n*10+(*s-'0');}*out=n;return true;
}
/* Require canonical absolute SD paths and refuse internal credentials / the
 * running service. lstat on every existing component rejects symbolic links. */
static bool sd_path(const char *hex,char out[SD_PATH],bool mutation) {
    size_t n=strlen(hex);if(!n||(n&1)||n/2+strlen(SD_ROOT)+1>=SD_PATH)return false;
    char raw[SD_PATH];for(size_t i=0;i<n/2;i++){int a=sd_nibble(hex[2*i]),b=sd_nibble(hex[2*i+1]);if(a<0||b<0)return false;raw[i]=(char)((a<<4)|b);if((unsigned char)raw[i]<32||raw[i]==127||raw[i]=='\\'||raw[i]==':')return false;}raw[n/2]=0;
    if(raw[0]!='/'||(n>2&&raw[n/2-1]=='/'))return false;
    char copy[SD_PATH];strcpy(copy,raw);char *save=NULL;
    for(char *c=strtok_r(copy,"/",&save);c;c=strtok_r(NULL,"/",&save))
        if(!strcmp(c,".")||!strcmp(c,"..")||!strncasecmp(c,".switch-control-",16))return false;
    if(strstr(raw,"//"))return false;
    if(!strncasecmp(raw,"/config/za-usb-control",22)&&(raw[22]==0||raw[22]=='/'))return false;
    if(mutation) {
        if(!strcmp(raw,"/")||(!strncasecmp(raw,"/Nintendo",9)&&(raw[9]==0||raw[9]=='/')))return false;
        const char *ids[]={"430000000000000b","430000000000000c","430000000000000d","430000000000000e"};
        for(size_t i=0;i<4;i++){char prefix[80];snprintf(prefix,sizeof(prefix),"/atmosphere/contents/%s",ids[i]);size_t z=strlen(prefix);if(!strncasecmp(raw,prefix,z)&&(raw[z]==0||raw[z]=='/'))return false;}
        if(!sd_mutation_allowed(raw))return false;
    }
    size_t root=strlen(SD_ROOT);memcpy(out,SD_ROOT,root);memcpy(out+root,raw,n/2+1);
    for(char *c=out+root+1;;c++)if(*c=='/'||!*c){char saved=*c;*c=0;struct stat st;int rc=lstat(out,&st);*c=saved;if(!rc&&S_ISLNK(st.st_mode))return false;if(rc&&errno!=ENOENT)return false;if(!saved)break;}
    return true;
}
static bool sd_hash_path(const char *path,char out[65]) {
    struct stat st;if(lstat(path,&st)||!S_ISREG(st.st_mode)||st.st_size<0||(uint64_t)st.st_size>SD_MAX_FILE)return false;
    FILE *file=fopen(path,"rb");if(!file)return false;SdHash hash;sd_hash_init(&hash);size_t n;uint64_t total=0;bool ok=true;
    while((n=fread(sd_buffer,1,sizeof(sd_buffer),file))){total+=n;if(total>SD_MAX_FILE){ok=false;break;}sd_hash_update(&hash,sd_buffer,n);SD_COOPERATE();}
    if(ferror(file)||total!=(uint64_t)st.st_size)ok=false;
    if(fclose(file))ok=false;
    if(!ok)return false;
    unsigned char digest[32];sd_hash_finish(digest,&hash);sd_hex(out,digest,32);return true;
}
static bool sd_expected(const char *path,const char *expected) {
    struct stat st;if(lstat(path,&st))return errno==ENOENT&&!strcmp(expected,"missing");
    char hash[65];return sd_digest(expected)&&sd_hash_path(path,hash)&&!strcasecmp(hash,expected);
}
static void sd_abort(void) {
    if(sd_upload.file)fclose(sd_upload.file);
    if(sd_upload.active&&sd_upload.stage[0])unlink(sd_upload.stage);
    memset(&sd_upload,0,sizeof(sd_upload));
}
static bool sd_token(const char *token){return sd_upload.active&&!strcmp(sd_upload.token,token);}
static bool sd_string(int fd,const char *text) {
    if(!sd_reply(fd,"\""))return false;
    const unsigned char *start=(const unsigned char*)text,*p=start;
    for(;;p++)if(*p<' '||*p=='"'||*p=='\\'||!*p){
        if(p>start&&!sd_send(fd,start,(size_t)(p-start)))return false;
        if(!*p)break;
        char escaped[8];snprintf(escaped,sizeof(escaped),"\\u%04x",*p);if(!sd_reply(fd,escaped))return false;start=p+1;
    }
    return sd_reply(fd,"\"");
}
static bool sd_listing(int fd,const char *path,uint64_t skip) {
    DIR *dir=opendir(path);if(!dir)return sd_error(fd,"open-directory");
    if(!sd_reply(fd,"{\"entries\":[")){closedir(dir);return false;}
    struct dirent *entry;uint64_t seen=0;unsigned emitted=0;bool ok=true,more=false;
    while((entry=readdir(dir))) {
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")||!strncasecmp(entry->d_name,".switch-control-",16))continue;
        if(seen++<skip)continue;
        if(emitted==64){more=true;break;}
        if(emitted&&!sd_reply(fd,",")){ok=false;break;}
        if(!sd_reply(fd,"{\"name\":")||!sd_string(fd,entry->d_name)){ok=false;break;}
        char child[SD_PATH];int z=snprintf(child,sizeof(child),"%s/%s",path,entry->d_name);struct stat st;
        if(z<0||(size_t)z>=sizeof(child)||lstat(child,&st)){ok=false;break;}
        char row[160];snprintf(row,sizeof(row),",\"type\":\"%s\",\"size\":%llu,\"mtime\":%lld}",S_ISREG(st.st_mode)?"file":S_ISDIR(st.st_mode)?"directory":"other",(unsigned long long)st.st_size,(long long)st.st_mtime);
        if(!sd_reply(fd,row)){ok=false;break;}emitted++;
    }
    closedir(dir);char tail[96];snprintf(tail,sizeof(tail),"],\"next\":%lld}\n",more?(long long)(skip+emitted):-1LL);return ok&&sd_reply(fd,tail);
}
#include "file-jobs.h"
static bool sd_commit(int fd) {
    if(!sd_upload.file||sd_task.active)return sd_error(fd,"upload-not-ready");
    if(sd_upload.written!=sd_upload.size)return sd_error(fd,"incomplete-upload");
    unsigned char bytes[32];char hash[65];sd_hash_finish(bytes,&sd_upload.hash);sd_hex(hash,bytes,32);
    bool valid=!strcasecmp(hash,sd_upload.wanted);if(fflush(sd_upload.file))valid=false;if(fsync(fileno(sd_upload.file)))valid=false;if(fclose(sd_upload.file))valid=false;sd_upload.file=NULL;
    if(valid&&(sd_upload.size>SD_SYNC_LIMIT||sd_large(sd_upload.path)))return sd_task_start(fd,sd_upload.stage,1,NULL,NULL);
    if(!valid||!sd_hash_path(sd_upload.stage,hash)||strcasecmp(hash,sd_upload.wanted)){sd_abort();return sd_error(fd,"upload-hash-mismatch");}
    const char *relative=sd_upload.path+strlen(SD_ROOT);
    if(!sd_mutation_allowed(relative)||!sd_expected(sd_upload.path,sd_upload.expected)){sd_abort();return sd_error(fd,"destination-changed");}
    bool had_old=strcmp(sd_upload.expected,"missing")!=0;
    if(had_old&&rename(sd_upload.path,sd_upload.previous)){sd_abort();return sd_error(fd,"retain-original");}
    bool published=rename(sd_upload.stage,sd_upload.path)==0;
    if(!published||!sd_hash_path(sd_upload.path,hash)||strcasecmp(hash,sd_upload.wanted)) {
        if(published)unlink(sd_upload.path);
        bool restored=!had_old||rename(sd_upload.previous,sd_upload.path)==0;
        sd_abort();return sd_error(fd,restored?"publish-failed-original-restored":"publish-failed-recovery-required");
    }
    bool clean=!had_old||unlink(sd_upload.previous)==0;
    memset(&sd_upload,0,sizeof(sd_upload));return sd_reply(fd,clean?"{\"ok\":true}\n":"{\"ok\":true,\"cleanup_pending\":true}\n");
}
/* Commands carry hexadecimal UTF-8 paths. This avoids whitespace, newline,
 * colon/device and shell ambiguity. Maximum command length is 2048 bytes. */
static bool sd_files_command(int fd,const char *line) {
    char copy[2049];if(strlen(line)>=sizeof(copy))return sd_error(fd,"command-too-long");strcpy(copy,line);
    char *v[8],*save=NULL;size_t n=0;for(char *x=strtok_r(copy," ",&save);x;x=strtok_r(NULL," ",&save)){if(n==8)return sd_error(fd,"arguments");v[n++]=x;}
    if(!n)return sd_error(fd,"arguments");
    if(!strcmp(v[0],"fsInfo")&&n==1){char out[256];snprintf(out,sizeof(out),"{\"version\":1,\"chunk_bytes\":65536,\"max_file_bytes\":8589934592,\"upload_active\":%s,\"upload\":\"%s\"}\n",sd_upload.active?"true":"false",sd_upload.active?sd_upload.token:"");return sd_reply(fd,out);}

    char path[SD_PATH];
    if((!strcmp(v[0],"fsTaskInfo")||!strcmp(v[0],"fsTaskCancel"))&&n==2){
        if(strlen(v[1])!=32||strcmp(v[1],sd_task.id))return sd_error(fd,"unknown-job");
        if(!strcmp(v[0],"fsTaskCancel")&&sd_task.active){sd_task_end("cancelled");}return sd_task_info(fd);
    }
    if(!strcmp(v[0],"fsUploadInfo")&&n==2){
        if(!sd_token(v[1]))return sd_error(fd,"unknown-upload");
        char out[240];snprintf(out,sizeof(out),"{\"upload\":\"%s\",\"bytes\":%llu,\"total\":%llu,\"verifying\":%s}\n",sd_upload.token,(unsigned long long)sd_upload.written,(unsigned long long)sd_upload.size,sd_task.active&&sd_task.op==1?"true":"false");return sd_reply(fd,out);
    }
    if(!strcmp(v[0],"fsAbort")&&n==2){if(!sd_token(v[1]))return sd_error(fd,"unknown-upload");if(sd_task.active&&sd_task.op==1)sd_task_end("cancelled");else sd_abort();return sd_reply(fd,"{\"ok\":true}\n");}
    if(!strcmp(v[0],"fsCommit")&&n==2){if(!sd_token(v[1]))return sd_error(fd,"unknown-upload");return sd_commit(fd);}
    if(!strcmp(v[0],"fsChunk")&&n==4) {
        uint64_t offset,count;if(!sd_upload.file||!sd_token(v[1])||!sd_number(v[2],&offset)||!sd_number(v[3],&count)||!count||count>SD_CHUNK||offset!=sd_upload.written||count>sd_upload.size-sd_upload.written)return sd_error(fd,"chunk-range");
        if(!sd_receive(fd,sd_buffer,(size_t)count)||fwrite(sd_buffer,1,(size_t)count,sd_upload.file)!=count){sd_abort();return sd_error(fd,"chunk-incomplete");}
        sd_hash_update(&sd_upload.hash,sd_buffer,(size_t)count);sd_upload.written+=count;return sd_reply(fd,"{\"ok\":true}\n");
    }
    bool mutation=!strcmp(v[0],"fsBegin")||!strcmp(v[0],"fsDelete")||!strcmp(v[0],"fsMkdir")||!strcmp(v[0],"fsMove");
    if(mutation&&sd_task.active)return sd_error(fd,"file-job-busy");
    if(n<2||!sd_path(v[1],path,mutation))return sd_error(fd,"path-denied");
    if(!strcmp(v[0],"fsList")&&n==3){uint64_t cursor;if(!sd_number(v[2],&cursor)||cursor>INT64_MAX)return sd_error(fd,"cursor");return sd_listing(fd,path,cursor);}
    if(!strcmp(v[0],"fsStat")&&n==2){struct stat st;if(lstat(path,&st)){if(errno==ENOENT)return sd_reply(fd,"{\"exists\":false}\n");return sd_error(fd,"stat");}char out[192];snprintf(out,sizeof(out),"{\"exists\":true,\"type\":\"%s\",\"size\":%llu,\"mtime\":%lld}\n",S_ISREG(st.st_mode)?"file":S_ISDIR(st.st_mode)?"directory":"other",(unsigned long long)st.st_size,(long long)st.st_mtime);return sd_reply(fd,out);}
    if((!strcmp(v[0],"fsHashStart")||!strcmp(v[0],"fsHash"))&&n==2){if(!strcmp(v[0],"fsHashStart")||sd_large(path))return sd_task_start(fd,path,0,NULL,NULL);char hash[65],out[96];if(!sd_hash_path(path,hash))return sd_error(fd,"hash-file");snprintf(out,sizeof(out),"{\"sha256\":\"%s\"}\n",hash);return sd_reply(fd,out);}
    if(!strcmp(v[0],"fsTail")&&n==3){
        uint64_t count;struct stat st;if(!sd_number(v[2],&count)||!count||count>SD_CHUNK||lstat(path,&st)||!S_ISREG(st.st_mode)||st.st_size<0)return sd_error(fd,"tail-range");
        uint64_t offset=(uint64_t)st.st_size>count?(uint64_t)st.st_size-count:0;
        char cmd[2049];snprintf(cmd,sizeof(cmd),"fsRead %s %llu %llu",v[1],(unsigned long long)offset,(unsigned long long)count);return sd_files_command(fd,cmd);
    }
    if(!strcmp(v[0],"fsRead")&&n==4){uint64_t offset,count;struct stat st;if(!sd_number(v[2],&offset)||!sd_number(v[3],&count)||!count||count>SD_CHUNK||lstat(path,&st)||!S_ISREG(st.st_mode)||st.st_size<0||offset>(uint64_t)st.st_size)return sd_error(fd,"read-range");if(count>(uint64_t)st.st_size-offset)count=(uint64_t)st.st_size-offset;FILE *file=fopen(path,"rb");if(!file)return sd_error(fd,"open-file");bool ok=!fseeko(file,(off_t)offset,SEEK_SET)&&fread(sd_buffer,1,(size_t)count,file)==count&&!ferror(file);if(fclose(file))ok=false;if(!ok)return sd_error(fd,"read-file");char header[96];snprintf(header,sizeof(header),"FILE %llu %llu %llu\n",(unsigned long long)offset,(unsigned long long)count,(unsigned long long)st.st_size);return sd_reply(fd,header)&&sd_send(fd,sd_buffer,(size_t)count);}
    if(!strcmp(v[0],"fsMkdir")&&n==2){if(mkdir(path,0700)){struct stat st;if(errno!=EEXIST||lstat(path,&st)||!S_ISDIR(st.st_mode))return sd_error(fd,"mkdir");}return sd_reply(fd,"{\"ok\":true}\n");}
    if(!strcmp(v[0],"fsDelete")&&n==3){if(sd_digest(v[2])&&sd_large(path))return sd_task_start(fd,path,2,v[2],NULL);if(!sd_digest(v[2])||!sd_expected(path,v[2]))return sd_error(fd,"file-changed");if(unlink(path))return sd_error(fd,"delete");return sd_reply(fd,"{\"ok\":true}\n");}
    if(!strcmp(v[0],"fsMove")&&n==4){char dest[SD_PATH];struct stat st;if(!sd_digest(v[3])||!sd_path(v[2],dest,true))return sd_error(fd,"move-source");if(!lstat(dest,&st)||errno!=ENOENT)return sd_error(fd,"destination-exists");if(sd_large(path))return sd_task_start(fd,path,3,v[3],dest);if(!sd_expected(path,v[3]))return sd_error(fd,"move-source");if(rename(path,dest))return sd_error(fd,"move");return sd_reply(fd,"{\"ok\":true}\n");}
    if(!strcmp(v[0],"fsBegin")&&n==5) {
        if(sd_upload.active)return sd_error(fd,"upload-busy");
        uint64_t size;
        if(!sd_number(v[2],&size)||size>SD_MAX_FILE||!sd_digest(v[3])||(strcmp(v[4],"missing")&&!sd_digest(v[4])))return sd_error(fd,"upload-arguments");
        if(!sd_large(path)&&!sd_expected(path,v[4]))return sd_error(fd,"destination-changed");
        if(sd_large(path)&&!sd_digest(v[4]))return sd_error(fd,"destination-changed");
        char *slash=strrchr(path,'/');if(!slash)return sd_error(fd,"path-denied");size_t parent=(size_t)(slash-path);
        if(parent+64>=SD_PATH)return sd_error(fd,"path-too-long");
        unsigned char token[16];sd_random(token,sizeof(token));sd_hex(sd_upload.token,token,sizeof(token));
        snprintf(sd_upload.stage,sizeof(sd_upload.stage),"%.*s/.switch-control-%s.part",(int)parent,path,sd_upload.token);
        snprintf(sd_upload.previous,sizeof(sd_upload.previous),"%.*s/.switch-control-%s.previous",(int)parent,path,sd_upload.token);
        struct stat st;if(!lstat(sd_upload.previous,&st)||errno!=ENOENT){memset(&sd_upload,0,sizeof(sd_upload));return sd_error(fd,"staging-exists");}
        int handle=open(sd_upload.stage,O_WRONLY|O_CREAT|O_EXCL,0600);if(handle<0){memset(&sd_upload,0,sizeof(sd_upload));return sd_error(fd,"stage-open");}
        sd_upload.file=fdopen(handle,"wb");if(!sd_upload.file){close(handle);unlink(sd_upload.stage);memset(&sd_upload,0,sizeof(sd_upload));return sd_error(fd,"stage-open");}
        sd_upload.active=true;strcpy(sd_upload.path,path);strcpy(sd_upload.wanted,v[3]);strcpy(sd_upload.expected,v[4]);sd_upload.size=size;sd_hash_init(&sd_upload.hash);
        char out[128];snprintf(out,sizeof(out),"{\"upload\":\"%s\"}\n",sd_upload.token);if(!sd_reply(fd,out)){sd_abort();return false;}return true;
    }
    return sd_error(fd,"unknown-file-command");
}
