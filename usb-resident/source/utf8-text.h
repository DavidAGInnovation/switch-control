#pragma once
#include <stdbool.h>
#include <stddef.h>
/* Validate bounded, printable UTF-8 from untrusted NACP metadata. */
static bool utf8_text_valid(const char *text,size_t size) {
    const unsigned char *p=(const unsigned char*)text;size_t at=0;
    while(at<size){unsigned c=p[at++],code,count;
        if(c<128){if(c<32||c==127)return false;continue;}
        if(c>=0xc2&&c<=0xdf){code=c&31;count=1;}
        else if(c>=0xe0&&c<=0xef){code=c&15;count=2;}
        else if(c>=0xf0&&c<=0xf4){code=c&7;count=3;}
        else return false;
        if(size-at<count)return false;
        for(unsigned k=0;k<count;k++){unsigned next=p[at++];if((next&0xc0)!=0x80)return false;code=(code<<6)|(next&63);}
        if((count==1&&code<0x80)||(count==2&&code<0x800)||(count==3&&code<0x10000)||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return false;
    }
    return true;
}
