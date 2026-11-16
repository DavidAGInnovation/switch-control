"""Host tests against the actual resident TLS peer loop.

Set ZA_MBEDTLS_HOST_ROOT to a separately host-built Mbed TLS 3.6.7 tree.
"""
import concurrent.futures
import importlib.util
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
spec = importlib.util.spec_from_file_location('wifi_tls_controller', HERE / 'wifi_tls_controller.py')
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)
AUTH_KEY = bytes([0x24]) * 32
TLS_KEY = bytes([0x42]) * 32

PRELUDE = r'''
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/md.h>
#include <psa/crypto.h>
#include <mbedtls/sha256.h>
typedef uint8_t u8;typedef uint64_t u64;
#define ZA_WIFI_TLS 1
static int wifi_listener=-1,wifi_recovery;
static u8 wifi_key[32];
static mbedtls_ssl_config wifi_tls_config;
static u64 svcGetSystemTick(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (u64)t.tv_sec*1000000000ULL+(u64)t.tv_nsec;}
static u64 armTicksToNs(u64 v){return v;}
static void svcSleepThread(u64 v){struct timespec t={(time_t)(v/1000000000ULL),(long)(v%1000000000ULL)};nanosleep(&t,NULL);}
static void randomGet(void *b,size_t n){arc4random_buf(b,n);}
static void hmacSha256CalculateMac(u8 *out,const u8 *k,size_t kn,const u8 *data,size_t n){const mbedtls_md_info_t *m=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);if(mbedtls_md_hmac(m,k,kn,data,n,out))abort();}
static void wifiRecoveryAuthenticated(int *r){(void)r;}
static bool wifi_tls_write_all(int fd,const char *data,size_t size);
static bool send_all(int fd,const char *data,size_t size){return wifi_tls_write_all(fd,data,size);}
static bool wifi_tls_read_all(int fd,void *data,size_t size);
typedef mbedtls_sha256_context Sha256Context;
static void sha256ContextCreate(Sha256Context *c){mbedtls_sha256_init(c);if(mbedtls_sha256_starts(c,0))abort();}
static void sha256ContextUpdate(Sha256Context *c,const void *p,size_t n){if(mbedtls_sha256_update(c,p,n))abort();}
static void sha256ContextGetHash(Sha256Context *c,unsigned char *out){if(mbedtls_sha256_finish(c,out))abort();mbedtls_sha256_free(c);}
#define SD_ROOT "."
#include "sd-files.h"
static bool sd_send(int fd,const void *p,size_t n){return send_all(fd,p,n);}
static bool sd_receive(int fd,void *p,size_t n){return wifi_tls_read_all(fd,p,n);}
static void sd_random(void *p,size_t n){randomGet(p,n);}
static bool sd_mutation_allowed(const char *path){(void)path;return true;}
static bool dispatch(int fd,const char *line){if(!strncmp(line,"fs",2))return sd_files_command(fd,line);const char *s=!strcmp(line,"getReaderStatus")?"RESIDENT host-test\n":"ERR unknown\n";return send_all(fd,s,strlen(s));}
'''

MAIN = r'''
int main(void){
    signal(SIGPIPE,SIG_IGN);memset(wifi_key,0x24,sizeof(wifi_key));
    u8 psk[32];memset(psk,0x42,sizeof(psk));
    mbedtls_entropy_context entropy;mbedtls_ctr_drbg_context rng;
    mbedtls_entropy_init(&entropy);mbedtls_ctr_drbg_init(&rng);mbedtls_ssl_config_init(&wifi_tls_config);
    if(psa_crypto_init()!=PSA_SUCCESS)return 2;
    if(mbedtls_ctr_drbg_seed(&rng,mbedtls_entropy_func,&entropy,(const u8 *)"test",4))return 3;
    if(mbedtls_ssl_config_defaults(&wifi_tls_config,MBEDTLS_SSL_IS_SERVER,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT))return 4;
    mbedtls_ssl_conf_rng(&wifi_tls_config,mbedtls_ctr_drbg_random,&rng);
    mbedtls_ssl_conf_min_tls_version(&wifi_tls_config,MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&wifi_tls_config,MBEDTLS_SSL_VERSION_TLS1_3);
    static const int ciphers[]={MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,MBEDTLS_TLS1_3_AES_128_GCM_SHA256,0};
    mbedtls_ssl_conf_ciphersuites(&wifi_tls_config,ciphers);
    mbedtls_ssl_conf_tls13_key_exchange_modes(&wifi_tls_config,MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL);
    mbedtls_ssl_conf_new_session_tickets(&wifi_tls_config,0);
    if(mbedtls_ssl_conf_psk(&wifi_tls_config,psk,sizeof(psk),(const u8 *)"za-wifi-tls-v1",14))return 5;
    wifi_listener=socket(AF_INET,SOCK_STREAM,0);if(wifi_listener<0)return 6;
    struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK),.sin_port=0};
    if(bind(wifi_listener,(struct sockaddr *)&a,sizeof(a))||listen(wifi_listener,8)||fcntl(wifi_listener,F_SETFL,O_NONBLOCK))return 7;
    socklen_t n=sizeof(a);if(getsockname(wifi_listener,(struct sockaddr *)&a,&n))return 8;
    printf("%u\n",ntohs(a.sin_port));fflush(stdout);wifi_close_peers();
    for(;;){wifi_poll();svcSleepThread(1000000ULL);}
}
'''


class TlsContextTest(unittest.TestCase):
    def test_requires_distinct_32_byte_keys(self):
        with self.assertRaises(ValueError):
            client.command('127.0.0.1', AUTH_KEY, AUTH_KEY, 'getReaderStatus')
        with self.assertRaises(ValueError):
            client.tls_context(b'short')

    def test_truncated_tls_reply_is_rejected(self):
        if not getattr(ssl, 'HAS_PSK', False) or not hasattr(ssl.SSLContext, 'set_psk_server_callback'):
            self.skipTest('OpenSSL TLS-PSK support required')
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_3
        context.set_psk_server_callback(lambda identity: TLS_KEY if identity == client.IDENTITY else b'')
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        failures = []

        def server():
            try:
                with listener:
                    raw, _ = listener.accept()
                    tls = context.wrap_socket(raw, server_side=True)
                    tls.sendall(bytes(32))
                    length = int.from_bytes(client.receive_exact(tls, 2), 'big')
                    client.receive_exact(tls, length + 32)
                    tls.sendall(b'RESIDENT incomplete')
                    # Close the underlying socket without TLS close_notify.
                    os.close(tls.detach())
            except Exception as exc:
                failures.append(exc)

        thread = threading.Thread(target=server)
        thread.start()
        with self.assertRaises(ssl.SSLEOFError):
            client.command('127.0.0.1', AUTH_KEY, TLS_KEY, 'getReaderStatus', port)
        thread.join(timeout=5)
        self.assertFalse(thread.is_alive())
        self.assertFalse(failures, failures)

    def test_certificate_only_impostor_receives_no_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            key, cert = Path(tmp) / 'key.pem', Path(tmp) / 'cert.pem'
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
                            '-nodes', '-subj', '/CN=impostor', '-days', '1', '-keyout', str(key), '-out', str(cert)],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_3
            context.load_cert_chain(cert, key)
            listener = socket.socket()
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            port = listener.getsockname()[1]
            observed = []

            def impostor():
                try:
                    with listener:
                        raw, _ = listener.accept()
                        with context.wrap_socket(raw, server_side=True) as tls:
                            tls.settimeout(3)
                            observed.append(tls.recv(1))
                except (ssl.SSLError, OSError) as exc:
                    observed.append(type(exc).__name__)

            thread = threading.Thread(target=impostor)
            thread.start()
            with self.assertRaises(ssl.SSLError):
                client.command('127.0.0.1', AUTH_KEY, TLS_KEY, 'getReaderStatus', port)
            thread.join(timeout=5)
            self.assertFalse(thread.is_alive())
            self.assertTrue(observed and (observed[0] == b'' or isinstance(observed[0], str)), observed)


@unittest.skipUnless(os.environ.get('ZA_MBEDTLS_HOST_ROOT'), 'host-built Mbed TLS 3.6.7 required')
class ResidentTlsIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(os.environ['ZA_MBEDTLS_HOST_ROOT'])
        source = (HERE / 'usb-resident/source/main.c').read_text()
        start = source.index('// Each unauthenticated peer has an absolute deadline')
        end = source.index('int main(int argc,char **argv)', start)
        cfile = Path(cls.tmp.name) / 'server.c'
        cls.exe = Path(cls.tmp.name) / 'server'
        cfile.write_text(PRELUDE + source[start:end] + MAIN)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'include'), '-I'+str(HERE/'usb-resident/source'),
                        str(cfile), '-L' + str(root / 'library'), '-lmbedtls', '-lmbedx509', '-lmbedcrypto',
                        '-o', str(cls.exe)], check=True)
        cls.sdroot=Path(cls.tmp.name)/'sd';cls.sdroot.mkdir();(cls.sdroot/'switch').mkdir()
        cls.server = subprocess.Popen([str(cls.exe)], stdout=subprocess.PIPE, text=True,cwd=cls.sdroot)
        cls.port = int(cls.server.stdout.readline())

    @classmethod
    def tearDownClass(cls):
        cls.server.terminate()
        cls.server.wait(timeout=5)
        cls.server.stdout.close()
        cls.tmp.cleanup()

    def test_truncated_binary_chunk_aborts_staging_over_tls(self):
        from sd_files import Files,FileError,path_arg
        import hashlib
        files=Files('127.0.0.1',AUTH_KEY,TLS_KEY,self.port)
        target='/switch/truncated.bin';wanted=hashlib.sha256(b'1234').hexdigest()
        token=files.json(f'fsBegin {path_arg(target)} 4 {wanted} missing')['upload']
        with self.assertRaises(FileError):files.json(f'fsChunk {token} 0 4',b'x')
        self.assertFalse(files.info()['upload_active'])
        self.assertFalse(files.stat(target)['exists'])

    def test_verified_binary_files_over_actual_tls_peer_loop(self):
        from sd_files import Files,FileError
        files=Files('127.0.0.1',AUTH_KEY,TLS_KEY,self.port)
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);original=p/'original';original.write_bytes(bytes(range(256))*800)
            remote='/switch/'+'é test '*24+'.bin'
            self.assertTrue(files.upload(original,remote)['ok'])
            with self.assertRaises(FileError):files.upload(original,remote)
            download=p/'download';files.download(remote,download);self.assertEqual(download.read_bytes(),original.read_bytes())
            previous=files.hash(remote);original.write_bytes(b'replacement\x00\xff')
            files.upload(original,remote,previous);self.assertEqual(files.hash(remote),__import__('hashlib').sha256(original.read_bytes()).hexdigest())
            files.move(remote,'/switch/moved.bin',files.hash(remote))
            self.assertEqual(files.list('/switch')[0]['name'],'moved.bin')
            files.delete('/switch/moved.bin',files.hash('/switch/moved.bin'));self.assertFalse(files.stat('/switch/moved.bin')['exists'])
            files.mkdir('/switch/newdir');self.assertEqual(files.stat('/switch/newdir')['type'],'directory')

    def test_tls_status_and_wrong_keys(self):
        self.assertEqual(client.command('127.0.0.1', AUTH_KEY, TLS_KEY, 'getReaderStatus', self.port), b'RESIDENT host-test\n')
        with self.assertRaises((ssl.SSLError, ConnectionError, OSError)):
            client.command('127.0.0.1', AUTH_KEY, bytes([0x43]) * 32, 'getReaderStatus', self.port)
        with self.assertRaises((ssl.SSLError, ConnectionError, OSError)):
            client.command('127.0.0.1', bytes([0x25]) * 32, TLS_KEY, 'getReaderStatus', self.port)

    def test_stalled_handshake_does_not_block_other_clients(self):
        with socket.create_connection(('127.0.0.1', self.port), 3):
            start = time.monotonic()
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                futures = [pool.submit(client.command, '127.0.0.1', AUTH_KEY, TLS_KEY, 'getReaderStatus', self.port) for _ in range(4)]
                self.assertTrue(all(f.result(timeout=5) == b'RESIDENT host-test\n' for f in futures))
            self.assertLess(time.monotonic() - start, 4)

    def test_plaintext_is_not_accepted(self):
        with socket.create_connection(('127.0.0.1', self.port), 3) as s:
            s.settimeout(5)
            s.sendall(b'getReaderStatus\n')
            try:
                reply = s.recv(1024)
            except ConnectionResetError:
                reply = b''
            self.assertNotIn(b'RESIDENT', reply)

    def test_tampered_encrypted_server_record_is_rejected(self):
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        client_finished = threading.Event()
        tampered = threading.Event()

        def read_record(sock):
            header = client.receive_exact(sock, 5)
            return header, client.receive_exact(sock, int.from_bytes(header[3:5], 'big'))

        def proxy():
            with listener:
                inbound, _ = listener.accept()
                with inbound, socket.create_connection(('127.0.0.1', self.port), 3) as upstream:
                    inbound.settimeout(5)
                    upstream.settimeout(5)

                    def to_server():
                        try:
                            while True:
                                header, body = read_record(inbound)
                                # The client's first encrypted TLS 1.3 record is
                                # Finished, forwarded before application data.
                                if header[0] == 23:
                                    client_finished.set()
                                upstream.sendall(header + body)
                        except (ConnectionError, OSError):
                            pass

                    thread = threading.Thread(target=to_server)
                    thread.start()
                    try:
                        while True:
                            header, body = read_record(upstream)
                            if header[0] == 23 and client_finished.is_set() and not tampered.is_set():
                                body = body[:-1] + bytes([body[-1] ^ 1])
                                tampered.set()
                            inbound.sendall(header + body)
                    except (ConnectionError, OSError):
                        pass
                    finally:
                        try:
                            inbound.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                        thread.join(timeout=5)

        thread = threading.Thread(target=proxy)
        thread.start()
        with self.assertRaises((ssl.SSLError, ConnectionError, OSError)):
            client.command('127.0.0.1', AUTH_KEY, TLS_KEY, 'getReaderStatus', port)
        thread.join(timeout=7)
        self.assertFalse(thread.is_alive())
        self.assertTrue(tampered.is_set(), 'the proxy did not alter a TLS application record')


if __name__ == '__main__':
    unittest.main()
