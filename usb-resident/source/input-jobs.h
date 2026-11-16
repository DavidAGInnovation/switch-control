#pragma once
/* Single-owner cooperative controller jobs. Tokens are random capabilities,
 * never part of discovery/status. A lost client expires within ten seconds. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef struct { uint64_t buttons; int lx,ly,rx,ry; unsigned hold,wait; } InputStep;
static struct {
    bool active,lease,waiting; unsigned count,index; uint64_t due,expires;
    char token[33],phase[40]; InputStep steps[32];
} input_job;
static uint64_t input_now(void);
static bool input_apply(const InputStep *step);
static bool input_release(void);
static void input_random(void *data,size_t size);
static bool input_reply(int fd,const char *text);
static bool input_error(int fd,const char *text);
static bool input_available(void);
static void input_stop(const char *reason) {
    bool ok=input_release();input_job.active=false;
    snprintf(input_job.phase,sizeof(input_job.phase),"%s",ok?reason:"cleanup-error");
}
static void input_poll(void) {
    if(!input_job.active)return;
    uint64_t now=input_now();
    if(now>=input_job.expires){input_stop("expired");return;}
    if(input_job.lease||now<input_job.due)return;
    InputStep *step=&input_job.steps[input_job.index];
    if(!input_job.waiting){
        InputStep zero={0};if(!input_apply(&zero)){input_stop("input-error");return;}
        input_job.waiting=true;input_job.due=now+step->wait;
        if(step->wait)return;
    }
    if(++input_job.index==input_job.count){input_stop("completed");return;}
    step=&input_job.steps[input_job.index];input_job.waiting=false;
    if(!input_apply(step)){input_stop("input-error");return;}
    input_job.due=now+step->hold;
}
static bool input_state_reply(int fd,bool reveal) {
    uint64_t now=input_now();char out[320];snprintf(out,sizeof(out),"{\"active\":%s,\"state\":\"%s\",\"step\":%u,\"steps\":%u,\"remaining_lease_ms\":%llu%s%s%s}\n",
        input_job.active?"true":"false",input_job.phase[0]?input_job.phase:"idle",input_job.index,input_job.count,
        (unsigned long long)(input_job.active&&input_job.expires>now?input_job.expires-now:0),
        reveal?",\"action\":\"":"",reveal?input_job.token:"",reveal?"\"":"");return input_reply(fd,out);
}
static bool input_valid_token(const char *token){return strlen(token)==32&&!strcmp(token,input_job.token);}
static bool input_command(int fd,const char *line) {
    input_poll();
    if(!strcmp(line,"inputInfo"))return input_state_reply(fd,false);
    if(!strcmp(line,"inputClear")){input_stop("cancelled");return input_state_reply(fd,false);}
    if(!strncmp(line,"inputStatus ",12)||!strncmp(line,"inputCancel ",12)) {
        if(!input_valid_token(line+12))return input_error(fd,"not-owner");
        if(line[5]=='C'&&input_job.active){input_stop("cancelled");}return input_state_reply(fd,true);
    }
    if(!strncmp(line,"inputRenew ",11)) {
        char token[40],value[32],extra,*end;
        if(sscanf(line+11,"%39s %31s %c",token,value,&extra)!=2||!input_valid_token(token)||value[0]<'0'||value[0]>'9')return input_error(fd,"invalid-lease");
        errno=0;unsigned long ms=strtoul(value,&end,10);
        if(errno||*end||ms<100||ms>10000)return input_error(fd,"invalid-lease");
        if(input_job.active){input_job.expires=input_now()+ms;}return input_state_reply(fd,true);
    }
    bool lease=!strncmp(line,"inputLease ",11);
    if(!lease&&strncmp(line,"inputStart ",11))return input_error(fd,"unknown-input-command");
    if(input_job.active||!input_available())return input_error(fd,"input-busy-or-unavailable");
    char copy[2049];if(strlen(line)>=sizeof(copy))return input_error(fd,"input-too-long");strcpy(copy,line+11);
    InputStep parsed[32];unsigned count=0,total=0;char *save=NULL;
    for(char *row=strtok_r(copy,";",&save);row;row=strtok_r(NULL,";",&save)) {
        if(count==32)return input_error(fd,"too-many-steps");
        unsigned long long mask;InputStep *p=&parsed[count];
        if(row[0]=='-'||row[0]==':')return input_error(fd,"invalid-step");
        char *end;errno=0;mask=strtoull(row,&end,16);if(errno||end==row||*end++!=':')return input_error(fd,"invalid-step");
        int *axes[]={&p->lx,&p->ly,&p->rx,&p->ry};
        for(unsigned k=0;k<4;k++){char *start=end;errno=0;long v=strtol(start,&end,10);if(errno||end==start||v < -32767||v>32767||*end++!=(k==3?':':','))return input_error(fd,"invalid-step");*axes[k]=(int)v;}
        char *start=end;errno=0;unsigned long hold=strtoul(start,&end,10);if(errno||end==start||!hold||hold>10000||*end++!=':')return input_error(fd,"invalid-step");p->hold=(unsigned)hold;
        start=end;errno=0;unsigned long wait=strtoul(start,&end,10);if(errno||end==start||wait>10000||*end)return input_error(fd,"invalid-step");p->wait=(unsigned)wait;
        p->buttons=(uint64_t)mask;total+=p->hold+p->wait;if(total>120000)return input_error(fd,"sequence-too-long");count++;
    }
    if(!count||(lease&&count!=1))return input_error(fd,"invalid-step-count");
    memset(&input_job,0,sizeof(input_job));memcpy(input_job.steps,parsed,count*sizeof(parsed[0]));input_job.count=count;input_job.lease=lease;
    unsigned char random[16];const char *hex="0123456789abcdef";input_random(random,16);
    for(unsigned i=0;i<16;i++){input_job.token[2*i]=hex[random[i]>>4];input_job.token[2*i+1]=hex[random[i]&15];}
    strcpy(input_job.phase,lease?"holding":"running");input_job.active=true;input_job.due=input_now()+parsed[0].hold;
    input_job.expires=input_now()+(lease?parsed[0].hold:10000);
    if(!input_apply(&parsed[0])){input_stop("input-error");return input_error(fd,"input-error");}
    bool ok=input_state_reply(fd,true);if(!ok)input_stop("reply-lost");return ok;
}
