#pragma once
static bool api_input_step(const ApiJson *j,int args,char *out,size_t size,bool single) {
    u64 mask=0;char name[32];double hold,wait,axes[4]={0};
    if(!api_args(j,args,single?"|button||buttons||duration_ms||wait_ms||left_x||left_y||right_x||right_y||lease_ms|":"|button||buttons||duration_ms||wait_ms||left_x||left_y||right_x||right_y|"))return false;
    int key=aj_get(j,args,"button"),list=aj_get(j,args,"buttons");
    if(key>=0&&list>=0)return false;
    if(key>=0){if(!aj_text(j,key,name,sizeof(name))||!button(name,&mask))return false;}
    if(list>=0){if(j->nodes[list].kind!='[')return false;unsigned count=0;for(int at=j->nodes[list].child;at>=0&&at<j->nodes[list].next;at=j->nodes[at].next){u64 bit;if(++count>8||!aj_text(j,at,name,sizeof(name))||!button(name,&bit)||(mask&bit))return false;mask|=bit;}}
    const char *names[]={"left_x","left_y","right_x","right_y"};
    for(unsigned i=0;i<4;i++)if(!api_num(j,args,names[i],&axes[i],-1,1,0))return false;
    if(!api_num(j,args,"duration_ms",&hold,1,10000,100)||!api_num(j,args,"wait_ms",&wait,0,10000,100)||floor(hold)!=hold||floor(wait)!=wait)return false;
    if(aj_get(j,args,"lease_ms")>=0){if(!single||!api_num(j,args,"lease_ms",&hold,100,10000,1000)||floor(hold)!=hold)return false;wait=0;}
    int n=snprintf(out,size,"%llx:%d,%d,%d,%d:%u:%u",(unsigned long long)mask,(int)round(axes[0]*32767),(int)round(axes[1]*32767),(int)round(axes[2]*32767),(int)round(axes[3]*32767),(unsigned)hold,(unsigned)wait);
    return n>0&&(size_t)n<size;
}
static bool api_input_job(int fd,const char *name,const ApiJson *j,int args) {
    char command[2048],step[128],token[40];
    if(!strcmp(name,"input_status")||!strcmp(name,"cancel_input")||!strcmp(name,"renew_input")) {
        if(!api_args(j,args,!strcmp(name,"renew_input")?"|action||lease_ms|":"|action|")||!api_property(j,args,"action",token,sizeof(token))||strlen(token)!=32)return false;
        for(unsigned i=0;i<32;i++)if(sd_nibble(token[i])<0)return false;
        if(!strcmp(name,"renew_input")){double lease;if(!api_num(j,args,"lease_ms",&lease,100,10000,1000)||floor(lease)!=lease)return false;snprintf(command,sizeof(command),"inputRenew %s %u",token,(unsigned)lease);}
        else snprintf(command,sizeof(command),"%s %s",!strcmp(name,"input_status")?"inputStatus":"inputCancel",token);
        return api_run(fd,command);
    }
    if(!strcmp(name,"hold_input")) {
        if(aj_get(j,args,"lease_ms")<0||!api_input_step(j,args,step,sizeof(step),true))return false;
        snprintf(command,sizeof(command),"inputLease %s",step);return api_run(fd,command);
    }
    if(!api_args(j,args,"|steps|"))return false;
    int list=aj_get(j,args,"steps");if(list<0||j->nodes[list].kind!='[')return false;
    strcpy(command,"inputStart ");unsigned count=0;
    for(int at=j->nodes[list].child;at>=0&&at<j->nodes[list].next;at=j->nodes[at].next){
        if(++count>32||!api_input_step(j,at,step,sizeof(step),false)||strlen(command)+strlen(step)+2>=sizeof(command))return false;
        if(count>1){strcat(command,";");}strcat(command,step);
    }
    return count&&api_run(fd,command);
}
