#pragma once
/* Cooperative verification: at most one 64 KiB read per main-loop turn. */
static struct {
    bool active,published,had_old,cleanup_pending;unsigned phase,op;
    FILE *file;SdHash hash;struct stat snapshot;
    uint64_t read,total;char id[33],path[SD_PATH],destination[SD_PATH];
    char expected[65],digest[65],state[48];
} sd_task;
static bool sd_task_info(int fd) {
    char out[400];snprintf(out,sizeof(out),"{\"job\":\"%s\",\"active\":%s,\"state\":\"%s\",\"phase\":%u,\"bytes\":%llu,\"total\":%llu,\"sha256\":\"%s\",\"cleanup_pending\":%s}\n",
        sd_task.id,sd_task.active?"true":"false",sd_task.state,sd_task.phase,(unsigned long long)sd_task.read,(unsigned long long)sd_task.total,sd_task.digest,sd_task.cleanup_pending?"true":"false");return sd_reply(fd,out);
}
static void sd_task_end(const char *state) {
    if(sd_task.file){fclose(sd_task.file);sd_task.file=NULL;}
    bool good=!strcmp(state,"completed");
    if(sd_task.op==1){
        if(!good&&sd_task.published){
            if(unlink(sd_upload.path)||(sd_task.had_old&&rename(sd_upload.previous,sd_upload.path)))state="recovery-required";
        }
        if(good){sd_task.cleanup_pending=sd_task.had_old&&unlink(sd_upload.previous)!=0;memset(&sd_upload,0,sizeof(sd_upload));}
        else sd_abort();
    }
    sd_task.active=false;snprintf(sd_task.state,sizeof(sd_task.state),"%s",state);
}
static bool sd_task_open(const char *path) {
    if(lstat(path,&sd_task.snapshot)||!S_ISREG(sd_task.snapshot.st_mode)||sd_task.snapshot.st_size<0||(uint64_t)sd_task.snapshot.st_size>SD_MAX_FILE)return false;
    sd_task.file=fopen(path,"rb");if(!sd_task.file)return false;
    sd_task.read=0;sd_task.total=(uint64_t)sd_task.snapshot.st_size;strcpy(sd_task.path,path);sd_hash_init(&sd_task.hash);return true;
}
static bool sd_task_start(int fd,const char *path,unsigned op,const char *expected,const char *destination) {
    if(sd_task.active)return sd_error(fd,"file-job-busy");
    memset(&sd_task,0,sizeof(sd_task));unsigned char id[16];sd_random(id,sizeof(id));sd_hex(sd_task.id,id,sizeof(id));
    sd_task.op=op;sd_task.phase=op==1?1:0;
    if(expected)strcpy(sd_task.expected,expected);
    if(destination)strcpy(sd_task.destination,destination);
    if(!sd_task_open(path))return sd_error(fd,"hash-open");
    sd_task.active=true;strcpy(sd_task.state,"running");return sd_task_info(fd);
}
static inline void sd_task_poll(void) {
    if(!sd_task.active)return;
    size_t want=sd_task.total-sd_task.read<SD_CHUNK?(size_t)(sd_task.total-sd_task.read):SD_CHUNK;
    size_t n=fread(sd_buffer,1,want,sd_task.file);
    if(n!=want||ferror(sd_task.file)){sd_task_end("read-failed");return;}
    sd_hash_update(&sd_task.hash,sd_buffer,n);sd_task.read+=n;
    if(sd_task.read<sd_task.total)return;
    struct stat st;bool same=!lstat(sd_task.path,&st)&&st.st_size==sd_task.snapshot.st_size&&st.st_mtime==sd_task.snapshot.st_mtime;
    unsigned char digest[32];sd_hash_finish(digest,&sd_task.hash);sd_hex(sd_task.digest,digest,32);fclose(sd_task.file);sd_task.file=NULL;
    if(!same){sd_task_end("file-changed");return;}
    if(sd_task.op==0){sd_task_end("completed");return;}
    if(sd_task.op==2||sd_task.op==3){
        if(strcasecmp(sd_task.digest,sd_task.expected)||!sd_mutation_allowed(sd_task.path+strlen(SD_ROOT))){sd_task_end("destination-changed");return;}
        if(sd_task.op==3){if(!sd_mutation_allowed(sd_task.destination+strlen(SD_ROOT))||!lstat(sd_task.destination,&st)||errno!=ENOENT){sd_task_end("destination-changed");return;}}
        int rc=sd_task.op==2?unlink(sd_task.path):rename(sd_task.path,sd_task.destination);
        sd_task_end(rc?"mutation-failed":"completed");return;
    }
    if(sd_task.phase==1){
        if(strcasecmp(sd_task.digest,sd_upload.wanted)){sd_task_end("upload-hash-mismatch");return;}
        sd_task.had_old=strcmp(sd_upload.expected,"missing")!=0;
        if(sd_task.had_old){sd_task.phase=2;if(!sd_task_open(sd_upload.path))sd_task_end("destination-changed");return;}
    }else if(sd_task.phase==2){
        if(strcasecmp(sd_task.digest,sd_upload.expected)){sd_task_end("destination-changed");return;}
    }else {
        if(strcasecmp(sd_task.digest,sd_upload.wanted)){sd_task_end("publish-verification-failed");return;}
        sd_task_end("completed");return;
    }
    if(!sd_mutation_allowed(sd_upload.path+strlen(SD_ROOT))){sd_task_end("path-denied");return;}
    if(!sd_task.had_old){if(!lstat(sd_upload.path,&st)||errno!=ENOENT){sd_task_end("destination-changed");return;}}
    else if(rename(sd_upload.path,sd_upload.previous)){sd_task_end("retain-original-failed");return;}
    if(rename(sd_upload.stage,sd_upload.path)){
        if(sd_task.had_old&&rename(sd_upload.previous,sd_upload.path)){sd_task_end("recovery-required");return;}
        sd_task_end("publish-failed-original-restored");return;
    }
    sd_task.published=true;sd_task.phase=3;
    if(!sd_task_open(sd_upload.path))sd_task_end("publish-verification-failed");
}
static bool sd_large(const char *path){struct stat st;return !lstat(path,&st)&&(uint64_t)st.st_size>SD_SYNC_LIMIT;}
