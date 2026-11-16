#pragma once
#include <ctype.h>
#include <math.h>
/* Strict bounded JSON tokenizer; input is never modified. */
typedef struct {int start,end,next,child;char kind;} ApiNode;
typedef struct {const char *text;int size,at,count;ApiNode nodes[1024];} ApiJson;
static void aj_space(ApiJson *j){while(j->at<j->size&&strchr(" \t\r\n",j->text[j->at]))j->at++;}
static int aj_value(ApiJson *j,unsigned depth);
static int aj_string(ApiJson *j){int first=j->at++;while(j->at<j->size){unsigned char c=j->text[j->at++];if(c=='"')return first;if(c<32)return -1;if(c=='\\'){if(j->at>=j->size)return -1;c=j->text[j->at++];if(c=='u'){for(unsigned i=0;i<4;i++)if(j->at>=j->size||sd_nibble(j->text[j->at++])<0)return -1;}else if(!strchr("\"\\/bfnrt",c))return -1;}}return -1;}
static int aj_value(ApiJson *j,unsigned depth){
    aj_space(j);if(depth>16||j->at>=j->size||j->count>=1024)return -1;int id=j->count++;ApiNode *n=&j->nodes[id];*n=(ApiNode){.start=j->at,.child=-1};char c=j->text[j->at];
    if(c=='{'||c=='['){n->kind=c;j->at++;aj_space(j);if(j->at<j->size&&j->text[j->at]==(c=='{'?'}':']'))j->at++;else for(;;){if(n->child<0)n->child=j->count;if(c=='{'){aj_space(j);if(j->at>=j->size||j->text[j->at]!='"'||aj_value(j,depth+1)<0)return -1;aj_space(j);if(j->at>=j->size||j->text[j->at++]!=':')return -1;}if(aj_value(j,depth+1)<0)return -1;aj_space(j);if(j->at>=j->size)return -1;char end=j->text[j->at++];if(end==(c=='{'?'}':']'))break;if(end!=',')return -1;}}
    else if(c=='"'){n->kind='s';if(aj_string(j)<0)return -1;}
    else if(c=='t'||c=='f'||c=='n'){const char *word=c=='t'?"true":c=='f'?"false":"null";size_t z=strlen(word);if(j->at+(int)z>j->size||memcmp(j->text+j->at,word,z))return -1;j->at+=(int)z;n->kind=c;}
    else {n->kind='n';int begin=j->at;if(c=='-')j->at++;if(j->at>=j->size)return -1;if(j->text[j->at]=='0')j->at++;else{if(j->text[j->at]<'1'||j->text[j->at]>'9')return -1;while(j->at<j->size&&isdigit((unsigned char)j->text[j->at]))j->at++;}if(j->at<j->size&&j->text[j->at]=='.'){j->at++;int at=j->at;while(j->at<j->size&&isdigit((unsigned char)j->text[j->at]))j->at++;if(j->at==at)return -1;}if(j->at<j->size&&strchr("eE",j->text[j->at])){j->at++;if(j->at<j->size&&strchr("+-",j->text[j->at]))j->at++;int at=j->at;while(j->at<j->size&&isdigit((unsigned char)j->text[j->at]))j->at++;if(j->at==at)return -1;}if(j->at-begin>64)return -1;}
    n->end=j->at;n->next=j->count;return id;
}
static bool aj_text(const ApiJson *j,int id,char *out,size_t cap){
    if(id<0||j->nodes[id].kind!='s'||!cap)return false;
    size_t used=0;int at=j->nodes[id].start+1,end=j->nodes[id].end-1;
    while(at<end){unsigned char c=j->text[at++];if(c=='\\'){c=j->text[at++];if(c=='u'){unsigned code=0;for(unsigned i=0;i<4;i++)code=(code<<4)|(unsigned)sd_nibble(j->text[at++]);if(code>=0xd800&&code<=0xdbff){if(at+6>end||j->text[at++]!='\\'||j->text[at++]!='u')return false;unsigned low=0;for(unsigned i=0;i<4;i++)low=(low<<4)|(unsigned)sd_nibble(j->text[at++]);if(low<0xdc00||low>0xdfff)return false;code=0x10000+((code-0xd800)<<10)+(low-0xdc00);}else if(code>=0xdc00&&code<=0xdfff)return false;if(!code)return false;unsigned char b[4];unsigned n;if(code<128){b[0]=(unsigned char)code;n=1;}else if(code<2048){b[0]=0xc0|(code>>6);b[1]=0x80|(code&63);n=2;}else if(code<65536){b[0]=0xe0|(code>>12);b[1]=0x80|((code>>6)&63);b[2]=0x80|(code&63);n=3;}else{b[0]=0xf0|(code>>18);b[1]=0x80|((code>>12)&63);b[2]=0x80|((code>>6)&63);b[3]=0x80|(code&63);n=4;}if(used+n>=cap)return false;memcpy(out+used,b,n);used+=n;continue;}if(c=='b')c='\b';else if(c=='f')c='\f';else if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';}if(used+1>=cap)return false;out[used++]=(char)c;}out[used]=0;return true;
}
static int aj_get(const ApiJson *j,int object,const char *key){if(object<0||j->nodes[object].kind!='{')return -1;int found=-1;for(int at=j->nodes[object].child;at>=0&&at<j->nodes[object].next;){char name[128];if(!aj_text(j,at,name,sizeof(name)))return -1;int value=at+1;if(!strcmp(name,key)){if(found>=0)return -1;found=value;}at=j->nodes[value].next;}return found;}
static bool aj_parse(ApiJson *j,const char *text,size_t n){memset(j,0,sizeof(*j));if(n>65536||memchr(text,0,n))return false;j->text=text;j->size=(int)n;if(aj_value(j,0)!=0)return false;aj_space(j);if(j->at!=j->size)return false;
    for(int obj=0;obj<j->count;obj++)if(j->nodes[obj].kind=='{')for(int at=j->nodes[obj].child;at>=0&&at<j->nodes[obj].next;at=j->nodes[at+1].next){char key[128];if(!aj_text(j,at,key,sizeof(key))||aj_get(j,obj,key)!=at+1)return false;}
    return true;
}
static bool aj_number(const ApiJson *j,int id,double *out){if(id<0||j->nodes[id].kind!='n'||j->text[j->nodes[id].start]=='n')return false;char number[65];size_t n=(size_t)(j->nodes[id].end-j->nodes[id].start);if(n>=sizeof(number))return false;memcpy(number,j->text+j->nodes[id].start,n);number[n]=0;char *end;errno=0;*out=strtod(number,&end);return !errno&&!*end&&isfinite(*out);}
