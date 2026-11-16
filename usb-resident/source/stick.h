#pragma once
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
static bool stick_parse(const char *line,char *side,int *x,int *y,unsigned *ms) {
    char copy[128];if(strlen(line)>=sizeof(copy))return false;strcpy(copy,line);
    char *v[6],*save=NULL;unsigned n=0;for(char *p=strtok_r(copy," ",&save);p;p=strtok_r(NULL," ",&save)){if(n==6)return false;v[n++]=p;}
    if(n!=5||strcmp(v[0],"stick")||strlen(v[1])!=1||(v[1][0]!='L'&&v[1][0]!='R'))return false;
    long values[3];for(unsigned i=0;i<3;i++){char *end;errno=0;values[i]=strtol(v[i+2],&end,10);if(errno||!*v[i+2]||*end)return false;}
    if(values[0]<-32767||values[0]>32767||values[1]<-32767||values[1]>32767||values[2]<1||values[2]>1000)return false;
    *side=v[1][0];*x=(int)values[0];*y=(int)values[1];*ms=(unsigned)values[2];return true;
}
