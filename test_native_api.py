"""Real native HTTPS/MCP implementation with host TLS and a temporary SD root."""
from pathlib import Path
import base64,hashlib,http.client,json,os,ssl,subprocess,tempfile,unittest
import test_wifi_tls as base
HERE=Path(__file__).parent
EXTRA=r'''
#define CONTROL_NATIVE_HTTP 1
typedef uint32_t u32;typedef uint32_t Result;
#define R_SUCCEEDED(x) ((x)==0)
#define resident_log(...) fprintf(stderr,__VA_ARGS__)
static mbedtls_ctr_drbg_context wifi_tls_rng;
static u32 disc_ip;
static char disc_host[64]="switch-test.local";
static bool disc_identity(void){return true;}
static Result nifmGetCurrentIpAddress(u32 *out){*out=htonl(INADDR_LOOPBACK);return 0;}
static bool api_info(int fd);
static bool button(const char *s,u64 *out){const char *v[]={"A","B","X","Y","L","R","ZL","ZR","PLUS","MINUS","UP","DOWN","LEFT","RIGHT","HOME","LSTICK","RSTICK"};for(unsigned i=0;i<sizeof(v)/sizeof(v[0]);i++)if(!strcmp(s,v[i])){*out=1ULL<<i;return true;}return false;}
'''
BACKEND=r'''static char last_command[2048];
static bool dispatch(int fd,const char *line){
 if(!strcmp(line,"getApiInfo"))return api_info(fd);
 if(!strncmp(line,"fs",2))return sd_files_command(fd,line);
 if(!strcmp(line,"systemStatus")){char out[2200];snprintf(out,sizeof(out),"{\"version\":\"0.5.0\",\"last\":\"%s\"}\n",last_command);return send_all(fd,out,strlen(out));}
 if(!strcmp(line,"pixelPeek"))return send_all(fd,"JPEG 4\n\xff\xd8\xff\xd9",11);
 strncpy(last_command,line,sizeof(last_command)-1);return send_all(fd,"OK\n",3);
}
'''
@unittest.skipUnless(os.environ.get('ZA_MBEDTLS_HOST_ROOT'),'host Mbed TLS required')
class NativeApiTest(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();p=Path(cls.tmp.name);cls.root=p/'sd';(cls.root/'switch').mkdir(parents=True);(cls.root/'sdmc:/config/za-usb-control').mkdir(parents=True)
  root=Path(os.environ['ZA_MBEDTLS_HOST_ROOT']);source=(HERE/'usb-resident/source/main.c').read_text();a=source.index('// Each unauthenticated peer has an absolute deadline');b=source.index('int main(int argc,char **argv)',a)
  pre=base.PRELUDE.replace('#define SD_ROOT', '#define SD_SYNC_LIMIT 65536\n#define SD_ROOT');start=pre.index('static bool dispatch(');pre=pre[:start]+EXTRA+BACKEND+'\n#include "api-native.h"\n'
  main=base.MAIN.replace('printf("%u\\n",ntohs(a.sin_port));fflush(stdout);wifi_close_peers();','''disc_ip=htonl(INADDR_LOOPBACK);mbedtls_ctr_drbg_init(&wifi_tls_rng);if(mbedtls_ctr_drbg_seed(&wifi_tls_rng,mbedtls_entropy_func,&entropy,NULL,0))return 8;api_start();if(api_listener<0)return 9;printf("%u\\n",ntohs(a.sin_port));fflush(stdout);wifi_close_peers();''')
  main=main.replace('wifi_poll();','wifi_poll();sd_task_poll();')
  (p/'server.c').write_text(pre+source[a:b]+main)
  subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(HERE/'usb-resident/source'),str(p/'server.c'),'-L'+str(root/'library'),'-lmbedtls','-lmbedx509','-lmbedcrypto','-lm','-o',str(p/'server')],check=True)
  cls.server=subprocess.Popen([str(p/'server')],stdout=subprocess.PIPE,text=True,cwd=cls.root);line=cls.server.stdout.readline()
  if not line:raise RuntimeError(f'Native server exited: {cls.server.poll()}')
  cls.psk_port=int(line);cls.context=ssl.create_default_context(cafile=str(cls.root/'sdmc:/config/za-usb-control/api-cert.pem'))
 @classmethod
 def tearDownClass(cls):
  if hasattr(cls,'server'):cls.server.terminate();cls.server.wait(timeout=5);cls.server.stdout.close()
  cls.tmp.cleanup()
 def request(self,method,path,body=None,*,token=True,headers={}):
  conn=http.client.HTTPSConnection('127.0.0.1',7443,context=self.context,timeout=5)
  h={'Host':'switch-test.local:7443','Accept':'application/json, text/event-stream',**headers}
  if token:h['Authorization']='Bearer '+base.AUTH_KEY.hex()
  if isinstance(body,dict):body=json.dumps(body).encode();h['Content-Type']='application/json'
  try:conn.request(method,path,body,h);r=conn.getresponse();return r.status,r.read(),dict(r.headers)
  finally:conn.close()
 def mcp(self,method,params=None):
  payload={'jsonrpc':'2.0','id':1,'method':method}
  if params is not None:payload['params']=params
  status,data,_=self.request('POST','/mcp',payload);self.assertEqual(status,200,data);return json.loads(data)
 def test_auth_origin_host_and_removed_browser(self):
  self.assertEqual(self.request('GET','/status',token=False)[0],401)
  self.assertEqual(self.request('GET','/status',headers={'Origin':'https://evil.example'})[0],403)
  self.assertEqual(self.request('GET','/status',headers={'Host':'evil.example'})[0],403)
  for token in [False,True]:
   status,data,headers=self.request('GET','/',token=token)
   self.assertEqual(status,404);self.assertEqual(headers['Content-Type'],'application/json')
   self.assertNotIn(b'<script>',data)
  self.assertEqual(self.request('GET','/certificate',token=False)[0],200)
 def test_keepalive_has_per_request_auth_and_honors_close(self):
  headers={'Host':'switch-test.local:7443','Authorization':'Bearer '+base.AUTH_KEY.hex()}
  conn=http.client.HTTPSConnection('127.0.0.1',7443,context=self.context,timeout=5)
  try:
   conn.request('GET','/status',headers=headers);r=conn.getresponse();self.assertEqual(r.status,200);r.read();port=conn.sock.getsockname()[1]
   conn.request('GET','/tools',headers=headers);r=conn.getresponse();self.assertEqual(r.status,200);self.assertIn('tools',json.loads(r.read()));self.assertEqual(conn.sock.getsockname()[1],port)
   conn.request('GET','/status',headers={'Host':'switch-test.local:7443'});r=conn.getresponse();self.assertEqual(r.status,401);self.assertEqual(r.getheader('Connection'),'close');r.read()
  finally:conn.close()
  conn=http.client.HTTPSConnection('127.0.0.1',7443,context=self.context,timeout=5)
  try:
   conn.request('GET','/status',headers={**headers,'Connection':'close'});r=conn.getresponse();self.assertEqual(r.status,200);self.assertEqual(r.getheader('Connection'),'close');r.read()
  finally:conn.close()
 def test_initialize_tools_notifications_and_unknown_method(self):
  init=self.mcp('initialize',{'protocolVersion':'2025-11-25','clientInfo':{'name':'test','version':'1'},'capabilities':{}});self.assertEqual(init['result']['protocolVersion'],'2025-11-25')
  tools=self.mcp('tools/list')['result']['tools'];self.assertIn('set_stick',[t['name'] for t in tools])
  status,data,_=self.request('POST','/mcp',{'jsonrpc':'2.0','method':'notifications/initialized'});self.assertEqual((status,data),(202,b''))
  self.assertEqual(self.mcp('unknown')['error']['code'],-32601)
  self.assertEqual(self.request('GET','/mcp')[0],405)
 def test_tools_stick_image_and_range(self):
  result=self.mcp('tools/call',{'name':'set_stick','arguments':{'side':'left','x':1,'y':-.5,'duration_ms':10}});self.assertFalse(result['result']['isError'])
  status=json.loads(self.request('GET','/status')[1]);self.assertEqual(status['last'],'stick L 32767 -16384 10')
  bad=self.mcp('tools/call',{'name':'set_stick','arguments':{'side':'left','x':2,'y':0}});self.assertEqual(bad['error']['code'],-32602)
  image=self.mcp('tools/call',{'name':'screenshot','arguments':{}})['result']['content'][0];self.assertEqual(base64.b64decode(image['data']),b'\xff\xd8\xff\xd9')
 def test_rest_binary_file_and_hash_guard(self):
  status,data,headers=self.request('PUT','/files?path=%2Fswitch%2Fapi.bin',b'binary\0payload');self.assertEqual(headers['Content-Type'],'application/json');self.assertEqual(status,200,data);self.assertTrue(json.loads(data)['ok'])
  self.assertEqual(self.request('GET','/files?path=%2Fswitch%2Fapi.bin')[1],b'binary\0payload')
  self.assertEqual(self.request('PUT','/files?path=%2Fswitch%2Fapi.bin',b'new')[0],409)
  old=hashlib.sha256(b'binary\0payload').hexdigest();self.assertEqual(self.request('PUT','/files?path=%2Fswitch%2Fapi.bin&expected_sha256='+old,b'new')[0],200)
  self.assertEqual(self.request('DELETE','/files?path=%2Fswitch%2Fapi.bin&expected_sha256='+hashlib.sha256(b'new').hexdigest())[0],200)
 def test_32_step_api_and_large_file_job_transport(self):
  result=self.mcp('tools/call',{'name':'run_input','arguments':{'steps':[{'buttons':['A','L'],'left_x':1,'right_y':-1,'duration_ms':1,'wait_ms':0}]*32}})
  self.assertFalse(result['result']['isError'])
  status=json.loads(self.request('GET','/status')[1]);self.assertTrue(status['last'].startswith('inputStart '));self.assertEqual(status['last'].count(';'),31)
  self.assertEqual(self.mcp('tools/call',{'name':'run_input','arguments':{'steps':[{'duration_ms':1}]*33}})['error']['code'],-32602)
  (self.root/'switch/log.txt').write_bytes(b'first\nlast\n')
  self.assertEqual(self.request('GET','/files?path=%2Fswitch%2Flog.txt&tail=5')[1],b'last\n')
 def test_real_https_client_cooperative_transfer_and_cancel(self):
  from native_client import NativeClient
  p=Path(self.tmp.name);(p/'token').write_text(base.AUTH_KEY.hex())
  c=NativeClient({'host':'127.0.0.1','hostname':'switch-test.local','port':7443,'certificate':str(self.root/'sdmc:/config/za-usb-control/api-cert.pem'),'token_file':str(p/'token')})
  source=p/'payload';payload=bytes(range(256))*800;source.write_bytes(payload);events=[]
  result=c.upload(source,'/switch/native-client.bin',progress=events.append);self.assertTrue(result['ok']);self.assertTrue(any(e['operation']=='upload' for e in events));self.assertTrue(any(e['operation']=='verify' for e in events))
  result=c.download('/switch/native-client.bin',p/'download',progress=events.append);self.assertEqual((p/'download').read_bytes(),payload)
  def stop(event):
   if event['operation']=='upload':raise InterruptedError('cancelled by client')
  with self.assertRaises(InterruptedError):c.upload(source,'/switch/cancelled.bin',progress=stop)
  self.assertFalse((self.root/'switch/cancelled.bin').exists())
 def test_clock_dns_api_validation_and_command_mapping(self):
  for route,body,expected in [('/network/dns',{'automatic':True},'dnsSet true - -'),('/network/dns',{'automatic':False,'primary':'1.1.1.1'},'dnsSet false 1.1.1.1 0.0.0.0'),('/settings/auto-time',{'enabled':False},'clockSync false'),('/settings/datetime',{'timestamp':1800000000},'clockSetUtc 1800000000'),('/settings/datetime',{'year':2028,'month':2,'day':29,'hour':23,'minute':59,'second':0},'clockSetLocal 2028 2 29 23 59 0')]:
   self.assertEqual(self.request('POST',route,body)[0],200);self.assertEqual(json.loads(self.request('GET','/status')[1])['last'],expected)
  for route,body in [('/network/dns',{'automatic':True,'primary':'1.1.1.1'}),('/network/dns',{'automatic':False,'primary':'0.0.0.0'}),('/network/dns',{'automatic':False,'primary':'1.1.1.1','secondary':'not-an-ip'}),('/settings/auto-time',{'enabled':1}),('/settings/datetime',{'timestamp':1800000000,'year':2026}),('/settings/datetime',{'year':2026,'month':2,'day':29,'hour':0,'minute':0,'second':0})]:
   self.assertEqual(self.request('POST',route,body)[0],400,body)
  r=self.mcp('tools/call',{'name':'set_auto_time','arguments':{'enabled':True}});self.assertFalse(r['result']['isError'])
 def test_json_rejects_duplicate_and_invalid_input(self):
  for text in [b'{"jsonrpc":"2.0","jsonrpc":"2.0","id":1,"method":"ping"}',b'{"id":1,}',b'[1,2]',b'{"id":01}',b'{"id":"\\uZZZZ"}']:
   self.assertEqual(self.request('POST','/mcp',text,headers={'Content-Type':'application/json'})[0],400)
