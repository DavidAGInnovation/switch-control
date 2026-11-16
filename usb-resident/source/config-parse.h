#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
static inline bool config_uint(const char *s,uint64_t low,uint64_t high,uint64_t *out){
    if(!s||!*s)return false;
    for(const char *p=s;*p;p++)if(*p<'0'||*p>'9')return false;
    errno=0;char *end;unsigned long long v=strtoull(s,&end,10);
    if(errno||*end||v<low||v>high)return false;
    *out=v;return true;
}
static inline bool config_ipv4(const char *s,unsigned char out[4],bool primary){
    for(unsigned i=0;i<4;i++){
        if(!*s||*s<'0'||*s>'9')return false;
        unsigned n=0,digits=0;const char *start=s;
        while(*s>='0'&&*s<='9'){n=n*10+(unsigned)(*s++-'0');if(++digits>3||n>255)return false;}
        if(digits>1&&*start=='0')return false;
        out[i]=(unsigned char)n;
        if(i<3){if(*s++!='.')return false;}else if(*s)return false;
    }
    bool zero=!(out[0]|out[1]|out[2]|out[3]);
    return (!primary||!zero)&&out[0]<224;
}
static inline bool config_date(unsigned year,unsigned month,unsigned day,unsigned hour,unsigned minute,unsigned second){
    static const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(year<2000||year>2099||month<1||month>12||!day||hour>23||minute>59||second>59)return false;
    unsigned max=days[month-1]+(month==2&&year%4==0&&(year%100!=0||year%400==0));return day<=max;
}
