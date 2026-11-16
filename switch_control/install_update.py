#!/usr/bin/env python3
"""Send a reviewed upgrade to hbmenu NetLoader, health-check, then commit it."""
import argparse,hashlib,hmac,json,socket,subprocess,time,zipfile,tempfile
from pathlib import Path
from .wifi_tls_controller import command
from .wifi_controller import receive_exact
from .native_client import NativeClient
from .discovery import discover

def confirmation(host,key,op):
    with socket.create_connection((host,6015),timeout=10) as s:
        nonce=receive_exact(s,32);data=op.encode().ljust(8,b'\0');s.sendall(data+hmac.digest(key,nonce+data,'sha256'));return s.recv(256).decode().strip()

def health(host,key,tls,expected,version='0.5.0'):
    import base64,ssl
    info=json.loads(command(host,key,tls,'getApiInfo'));pem=base64.b64decode(info['certificate_base64'],validate=True)
    if hashlib.sha256(ssl.PEM_cert_to_DER_cert(pem.decode())).hexdigest()!=info['certificate_sha256']:raise ValueError('Certificate mismatch')
    with tempfile.TemporaryDirectory() as t:
        p=Path(t);(p/'cert.pem').write_bytes(pem);(p/'token').write_text(key.hex());client=NativeClient({'host':host,'hostname':info['hostname'],'port':7443,'certificate':str(p/'cert.pem'),'token_file':str(p/'token')})
        status=json.loads(client.request('GET','/status'))
        if status.get('version')!=version:raise ValueError('Wrong service version')
        init=json.loads(client.request('POST','/mcp',{'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2025-11-25','capabilities':{},'clientInfo':{'name':'installer','version':version}}}))
        if 'result' not in init:raise ValueError('MCP initialization failed')
        tools=json.loads(client.request('POST','/mcp',{'jsonrpc':'2.0','id':2,'method':'tools/list'}))
        if not any(x['name']=='set_stick' for x in tools['result']['tools']):raise ValueError('Missing native tools')
        client.request('POST','/input/stick',{'side':'left','x':0,'y':0,'duration_ms':1})
        if version in ('0.4.0','0.5.0'):
            lifecycle=json.loads(client.request('GET','/lifecycle'))
            if not lifecycle.get('power_registered') or lifecycle.get('power_result'):raise ValueError('Power lifecycle registration failed')
            action=json.loads(client.request('POST','/input/hold',{'lease_ms':1000}))
            if not action.get('active') or 'action' not in action:raise ValueError('Leased input unavailable')
            ended=json.loads(client.request('POST','/input/cancel',{'action':action['action']}))
            if ended.get('active') or ended.get('state')!='cancelled':raise ValueError('Input cleanup failed')
        if version=='0.5.0':
            clock=json.loads(client.request('GET','/settings/datetime'))
            if not isinstance(clock.get('timestamp'),int) or not isinstance(clock.get('automatic_sync'),bool):raise ValueError('Clock read unavailable')
            if any(x['name']=='set_dns' for x in tools['result']['tools']) is False:raise ValueError('DNS control missing')
        from urllib.parse import quote
        actual=json.loads(client.request('GET','/hash?path='+quote('/atmosphere/contents/430000000000000D/exefs.nsp',safe='')))['sha256']
        if actual!=expected:raise ValueError('Service hash mismatch')
        return {'passed':True,'native_https':True,'mcp':True,'status':status,'api_info':{k:v for k,v in info.items() if k!='certificate_base64'}}

def install(bundle,host,key,tls,nxlink):
    bundle=Path(bundle)
    with zipfile.ZipFile(bundle) as z:
        manifest=json.loads(z.read('manifest.json'));name=manifest.get('updater_path')
        if not name:raise ValueError('This bundle uses SD installation and contains no guarded updater')
        data=z.read(name)
        if hashlib.sha256(data).hexdigest()!=manifest['files'][name]:raise ValueError('Updater hash mismatch')
    if host=='auto':
        candidates=[]
        for p in discover():
            try:command(p['host'],key,tls,'getVersion',p['port'])
            except OSError:continue
            candidates.append(p)
        if len(candidates)!=1:raise ValueError('Choose one paired console')
        host=candidates[0]['host']
    with tempfile.TemporaryDirectory() as t:
        file=Path(t)/Path(name).name;file.write_bytes(data);subprocess.run([str(Path(nxlink).resolve()),'--address',host,'--path',file.name,file.name],cwd=t,check=True)
        # Handoff to the loaded app; no command is replayed on failure.
        time.sleep(2)
        try:result=health(host,key,tls,manifest['service_sha256'],manifest.get('version','0.3.0'));reply=confirmation(host,key,'commit')
        except BaseException:
            try:confirmation(host,key,'rollback')
            except OSError:pass
            raise
        if reply!='COMMIT':raise RuntimeError('Update was not committed; prior executable will be restored')
        return result
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('bundle',type=Path);p.add_argument('--host',default='auto');p.add_argument('--key-file',type=Path,required=True);p.add_argument('--tls-key-file',type=Path,required=True);p.add_argument('--nxlink',type=Path,required=True);a=p.parse_args();print(json.dumps(install(a.bundle,a.host,a.key_file.read_bytes(),a.tls_key_file.read_bytes(),a.nxlink),indent=2))
