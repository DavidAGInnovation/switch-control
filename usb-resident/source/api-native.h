#pragma once
#include <netinet/tcp.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/ecp.h>
#include "api-http.h"
static int api_listener=-1;
static bool api_tls_ready;
static bool api_open_listener(void);
static mbedtls_ssl_config api_tls_config;
static mbedtls_x509_crt api_certificate;
static mbedtls_pk_context api_private_key;
static unsigned char api_cert_pem[4096];
static bool api_backend(int fd,const char *command){return dispatch(fd,command);}
static bool api_store(const char *path,const unsigned char *data,size_t n){int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return false;bool ok=write(fd,data,n)==(ssize_t)n&&!fsync(fd);if(close(fd))ok=false;if(!ok)unlink(path);return ok;}
static bool api_certificate_prepare(void){
    mbedtls_x509_crt_init(&api_certificate);mbedtls_pk_init(&api_private_key);
    const char *cert="sdmc:/config/za-usb-control/api-cert.pem",*key="sdmc:/config/za-usb-control/api-key.pem";
    FILE *f=fopen(cert,"rb");if(f){size_t n=fread(api_cert_pem,1,sizeof(api_cert_pem)-1,f);bool ok=!ferror(f)&&feof(f);fclose(f);api_cert_pem[n]=0;if(!ok||mbedtls_x509_crt_parse(&api_certificate,api_cert_pem,n+1)||mbedtls_pk_parse_keyfile(&api_private_key,key,NULL,mbedtls_ctr_drbg_random,&wifi_tls_rng))return false;return !mbedtls_pk_check_pair(&api_certificate.pk,&api_private_key,mbedtls_ctr_drbg_random,&wifi_tls_rng);}
    if(access(key,F_OK)==0||!disc_identity())return false;
    int rc=mbedtls_pk_setup(&api_private_key,mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if(!rc)rc=mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,mbedtls_pk_ec(api_private_key),mbedtls_ctr_drbg_random,&wifi_tls_rng);
    if(rc)return false;
    mbedtls_x509write_cert writer;mbedtls_x509write_crt_init(&writer);mbedtls_x509write_crt_set_version(&writer,MBEDTLS_X509_CRT_VERSION_3);mbedtls_x509write_crt_set_md_alg(&writer,MBEDTLS_MD_SHA256);mbedtls_x509write_crt_set_subject_key(&writer,&api_private_key);mbedtls_x509write_crt_set_issuer_key(&writer,&api_private_key);
    unsigned char serial[16];randomGet(serial,sizeof(serial));serial[0]&=0x7f;serial[0]|=1;char dn[128];snprintf(dn,sizeof(dn),"CN=%s,O=Switch Control",disc_host);
    rc=mbedtls_x509write_crt_set_serial_raw(&writer,serial,sizeof(serial));
    if(!rc)rc=mbedtls_x509write_crt_set_subject_name(&writer,dn);
    if(!rc)rc=mbedtls_x509write_crt_set_issuer_name(&writer,dn);
    if(!rc)rc=mbedtls_x509write_crt_set_validity(&writer,"20200101000000","20400101000000");
    if(!rc)rc=mbedtls_x509write_crt_set_basic_constraints(&writer,1,0);
    if(!rc)rc=mbedtls_x509write_crt_set_key_usage(&writer,MBEDTLS_X509_KU_DIGITAL_SIGNATURE|MBEDTLS_X509_KU_KEY_CERT_SIGN);
    if(!rc)rc=mbedtls_x509write_crt_set_subject_key_identifier(&writer);
    if(!rc)rc=mbedtls_x509write_crt_set_authority_key_identifier(&writer);
    u32 ip=0;nifmGetCurrentIpAddress(&ip);mbedtls_x509_san_list dns={0},address={0};dns.node.type=MBEDTLS_X509_SAN_DNS_NAME;dns.node.san.unstructured_name.p=(unsigned char*)disc_host;dns.node.san.unstructured_name.len=strlen(disc_host);dns.next=&address;address.node.type=MBEDTLS_X509_SAN_IP_ADDRESS;address.node.san.unstructured_name.p=(unsigned char*)&ip;address.node.san.unstructured_name.len=4;
    if(!rc)rc=mbedtls_x509write_crt_set_subject_alternative_name(&writer,&dns);
    if(!rc)rc=mbedtls_x509write_crt_pem(&writer,api_cert_pem,sizeof(api_cert_pem),mbedtls_ctr_drbg_random,&wifi_tls_rng);
    mbedtls_x509write_crt_free(&writer);
    if(rc)return false;
    unsigned char private_pem[2048];rc=mbedtls_pk_write_key_pem(&api_private_key,private_pem,sizeof(private_pem));
    if(rc)return false;
    bool ok=api_store(key,private_pem,strlen((char*)private_pem));memset(private_pem,0,sizeof(private_pem));if(!ok)return false;
    if(!api_store(cert,api_cert_pem,strlen((char*)api_cert_pem))||mbedtls_x509_crt_parse(&api_certificate,api_cert_pem,strlen((char*)api_cert_pem)+1))return false;
    return api_store("sdmc:/config/za-usb-control/api-cert.der",api_certificate.raw.p,api_certificate.raw.len);
}
static void api_start(void){
    if(wifi_listener<0||!api_certificate_prepare()){resident_log("HTTPS certificate setup failed; PSK control remains available\n");return;}
    mbedtls_ssl_config_init(&api_tls_config);int rc=mbedtls_ssl_config_defaults(&api_tls_config,MBEDTLS_SSL_IS_SERVER,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT);
    if(!rc)rc=mbedtls_ssl_conf_own_cert(&api_tls_config,&api_certificate,&api_private_key);
    if(rc)return;
    mbedtls_ssl_conf_rng(&api_tls_config,mbedtls_ctr_drbg_random,&wifi_tls_rng);mbedtls_ssl_conf_min_tls_version(&api_tls_config,MBEDTLS_SSL_VERSION_TLS1_3);mbedtls_ssl_conf_max_tls_version(&api_tls_config,MBEDTLS_SSL_VERSION_TLS1_3);mbedtls_ssl_conf_tls13_key_exchange_modes(&api_tls_config,MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL);
    api_tls_ready=true;(void)api_open_listener();
}
static bool api_open_listener(void){
    if(!api_tls_ready)return false;
    int fd=socket(AF_INET,SOCK_STREAM,0);int one=1;struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(7443),.sin_addr.s_addr=htonl(INADDR_ANY)};
    if(fd<0)return false;
    if(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one))||bind(fd,(void*)&a,sizeof(a))||listen(fd,4)||fcntl(fd,F_SETFL,O_NONBLOCK)){close(fd);return false;}api_listener=fd;resident_log("Native HTTPS REST/MCP listening on 7443\n");return true;
}
static bool api_info(int fd){unsigned char fingerprint[32];Sha256Context h;sha256ContextCreate(&h);sha256ContextUpdate(&h,api_certificate.raw.p,api_certificate.raw.len);sha256ContextGetHash(&h,fingerprint);char hex[65];sd_hex(hex,fingerprint,32);static unsigned char b64[6000];size_t n=0;if(api_listener<0||mbedtls_base64_encode(b64,sizeof(b64)-1,&n,api_cert_pem,strlen((char*)api_cert_pem)))return sd_error(fd,"https-unavailable");b64[n]=0;char prefix[256];snprintf(prefix,sizeof(prefix),"{\"port\":7443,\"hostname\":\"%s\",\"certificate_sha256\":\"%s\",\"certificate_base64\":\"",disc_host,hex);return sd_reply(fd,prefix)&&sd_send(fd,b64,n)&&sd_reply(fd,"\"}\n");}
