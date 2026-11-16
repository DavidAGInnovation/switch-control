#!/usr/bin/env python3
"""Optional client for the Switch's native HTTPS endpoints; no bridge server."""
import argparse,base64,hashlib,http.client,json,os,socket,ssl,time,secrets
from pathlib import Path
from .discovery import discover

def write_private(path,data):
    fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as f:f.write(data);f.flush();os.fsync(f.fileno())

def configure(host,key_file,tls_key_file,output):
    from .wifi_tls_controller import command
    output=Path(output)
    if output.exists():raise FileExistsError(output)
    key=Path(key_file).read_bytes();tls=Path(tls_key_file).read_bytes()
    automatic=host=='auto'
    if host=='auto':
        peers=discover();paired=[]
        for p in peers:
            try:command(p['host'],key,tls,'getVersion',p['port'])
            except OSError:continue
            paired.append(p)
        if len(paired)!=1:raise ValueError('Select one reachable paired Switch')
        host=paired[0]['host']
    info=json.loads(command(host,key,tls,'getApiInfo'));pem=base64.b64decode(info['certificate_base64'],validate=True);der=ssl.PEM_cert_to_DER_cert(pem.decode())
    if hashlib.sha256(der).hexdigest()!=info['certificate_sha256']:raise ValueError('Certificate fingerprint mismatch')
    output.mkdir(parents=True,exist_ok=False)
    cert=output/'switch-cert.pem';token=output/'api.token';write_private(cert,pem);write_private(token,key.hex().encode())
    config={'host':'auto' if automatic else host,'hostname':info['hostname'],'port':info['port'],'certificate':str(cert.resolve()),'token_file':str(token.resolve()),'certificate_sha256':info['certificate_sha256']};write_private(output/'config.json',(json.dumps(config,indent=2)+'\n').encode());return {'url':f'https://{info["hostname"]}:{info["port"]}','configuration':str((output/'config.json').resolve())}

class NativeClient:
    def __init__(self,config):self._address=None;self._connection=None;self._last_request=0;self.config=json.loads(Path(config).read_text()) if isinstance(config,(str,Path)) else config
    def request(self,method,path,body=None,headers={}):
        c=self.config;host=c['host']
        if host=='auto' and self._address is None:
            peers=[p for p in discover() if p['hostname'].lower()==c['hostname'].lower()]
            if len(peers)!=1:raise ValueError('Paired Switch not uniquely discoverable')
            self._address=peers[0]['host']
        if host=='auto':host=self._address
        hostname=c['hostname'];port=c.get('port',7443)
        context=ssl.create_default_context(cafile=c['certificate']) if self._connection is None else None
        class Direct(http.client.HTTPSConnection):
            def connect(self):
                raw=socket.create_connection((host,port),self.timeout)
                raw.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
                try:self.sock=context.wrap_socket(raw,server_hostname=hostname)
                except BaseException:raw.close();raise
        if self._connection is not None and time.monotonic()-self._last_request>3:self.close();context=ssl.create_default_context(cafile=c['certificate'])
        connection=self._connection or Direct(hostname,port,timeout=15)
        token=Path(c['token_file']).read_text().strip()
        supplied={'Authorization':'Bearer '+token,'Accept':'application/json, text/event-stream',**headers}
        if isinstance(body,dict):body=json.dumps(body).encode();supplied['Content-Type']='application/json'
        try:
            connection.request(method,path,body,supplied);response=connection.getresponse();data=response.read()
            if response.status>=400:raise RuntimeError(f'HTTP {response.status}: {data[:200].decode("utf-8","replace")}')
            if response.getheader('Connection','').lower()=='close':connection.close();self._connection=None
            else:self._connection=connection
            self._last_request=time.monotonic();return data
        except BaseException:
            connection.close();self._connection=None;raise
    def close(self):
        if self._connection is not None:self._connection.close();self._connection=None
    def __del__(self):self.close()
    def call(self,name,arguments={}):return json.loads(self.request('POST','/mcp',{'jsonrpc':'2.0','id':1,'method':'tools/call','params':{'name':name,'arguments':arguments}}))
    def wait_file_job(self,result,progress=None,cancel=None,timeout=10800):
        if 'job' not in result:return result
        token=result['job'];deadline=time.monotonic()+timeout
        try:
            while result.get('active'):
                if cancel and cancel():raise InterruptedError('Transfer cancelled')
                if time.monotonic()>deadline:raise TimeoutError('File verification deadline exceeded')
                if progress:progress({'operation':'verify',**result})
                time.sleep(.05);result=json.loads(self.request('GET','/file-jobs/'+token))
            if result.get('state')!='completed':raise RuntimeError('File job '+result.get('state','failed'))
            return {**result,'ok':True}
        except BaseException:
            try:self.request('DELETE','/file-jobs/'+token)
            except Exception:pass
            raise
    def hash(self,remote,progress=None,cancel=None):
        from urllib.parse import quote
        r=json.loads(self.request('GET','/hash?path='+quote(remote,safe='')))
        return self.wait_file_job(r,progress,cancel)['sha256']
    def wait_input(self,action,progress=None,cancel=None):
        token=action['action']
        try:
            while action.get('active'):
                if cancel and cancel():raise InterruptedError('Input cancelled')
                if progress:progress(action)
                # Explicit renewal is independent of the HTTP connection lifetime.
                action=json.loads(self.request('POST','/input/renew',{'action':token,'lease_ms':10000}))
                time.sleep(.1)
                action=json.loads(self.request('POST','/input/status',{'action':token}))
            if action.get('state')!='completed':raise RuntimeError('Input '+action.get('state','failed'))
            return action
        except BaseException:
            try:self.request('POST','/input/cancel',{'action':token})
            except Exception:pass
            raise
    def download(self,remote,local,progress=None,cancel=None):
        from urllib.parse import quote
        local=Path(local)
        if local.exists():raise FileExistsError(local)
        path=quote(remote,safe='');meta=json.loads(self.request('GET','/stat?path='+path))
        if not meta.get('exists') or meta.get('type')!='file':raise ValueError('Not a regular file')
        total=meta['size']
        if total>8*1024**3:raise ValueError('Maximum verified file is 8 GiB')
        expected=self.hash(remote,progress,cancel);stage=local.with_name(local.name+'.part-'+secrets.token_hex(8));offset=0;digest=hashlib.sha256()
        try:
            with stage.open('xb') as f:
                while offset<total:
                    if cancel and cancel():raise InterruptedError('Download cancelled')
                    count=min(65536,total-offset);data=self.request('GET',f'/files?path={path}&offset={offset}&size={count}')
                    if len(data)!=count:raise ValueError('File changed or read truncated')
                    f.write(data);digest.update(data);offset+=len(data)
                    if progress:progress({'operation':'download','bytes':offset,'total':total})
                f.flush();os.fsync(f.fileno())
            if digest.hexdigest()!=expected or self.hash(remote,progress,cancel)!=expected:raise ValueError('File changed during download')
            os.link(stage,local)
        finally:stage.unlink(missing_ok=True)
        return {'ok':True,'bytes':offset,'sha256':expected}
    def upload(self,local,remote,expected_sha256='missing',progress=None,cancel=None):
        from urllib.parse import quote
        local=Path(local);size=local.stat().st_size
        if size>8*1024**3:raise ValueError('Maximum upload is 8 GiB')
        with local.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
        token=json.loads(self.request('POST','/uploads',{'path':remote,'size':size,'sha256':digest,'expected_sha256':expected_sha256}))['upload'];offset=0
        try:
            with local.open('rb') as f:
                while data:=f.read(65536):
                    if cancel and cancel():raise InterruptedError('Upload cancelled')
                    self.request('PUT',f'/uploads/{token}?offset={offset}',data);offset+=len(data)
                    if progress:progress({'operation':'upload','bytes':offset,'total':size})
            if offset!=size:raise ValueError('Local file changed size during upload')
            result=self.wait_file_job(json.loads(self.request('POST',f'/uploads/{token}/commit',b'')),progress,cancel)
        except BaseException:
            try:self.request('DELETE',f'/uploads/{token}')
            except Exception:pass
            raise
        actual=self.hash(remote,progress,cancel)
        if actual!=digest:raise ValueError('Uploaded hash mismatch')
        return result

def main():
    import sys
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--config',type=Path);sub=p.add_subparsers(dest='op',required=True)
    sub.add_parser('discover')
    q=sub.add_parser('configure');q.add_argument('--host',default='auto');q.add_argument('--key-file',type=Path,required=True);q.add_argument('--tls-key-file',type=Path,required=True);q.add_argument('output',type=Path)
    sub.add_parser('status');q=sub.add_parser('call');q.add_argument('name');q.add_argument('arguments',nargs='?',default='{}')
    q=sub.add_parser('screenshot');q.add_argument('output',type=Path)
    q=sub.add_parser('get');q.add_argument('remote');q.add_argument('local',type=Path);q.add_argument('--progress',action='store_true')
    q=sub.add_parser('put');q.add_argument('local',type=Path);q.add_argument('remote');q.add_argument('--expected-sha256',default='missing');q.add_argument('--progress',action='store_true')
    a=p.parse_args();last=[0]
    def progress(r):
        now=time.monotonic()
        if now-last[0]>=1:
            print(f"{r.get('operation','transfer')}: {r.get('bytes',0)} / {r.get('total',0)} bytes (phase {r.get('phase',0)})",file=sys.stderr);last[0]=now
    callback=progress if getattr(a,'progress',False) else None
    if a.op=='discover':result=discover()
    elif a.op=='configure':result=configure(a.host,a.key_file,a.tls_key_file,a.output)
    else:
        if not a.config:p.error('--config is required')
        client=NativeClient(a.config)
        try:
            if a.op=='status':result=json.loads(client.request('GET','/status'))
            elif a.op=='call':result=client.call(a.name,json.loads(a.arguments))
            elif a.op=='get':result=client.download(a.remote,a.local,progress=callback)
            elif a.op=='put':result=client.upload(a.local,a.remote,a.expected_sha256,progress=callback)
            else:write_private(a.output,client.request('GET','/screenshot'));result={'saved':str(a.output)}
        finally:client.close()
    print(json.dumps(result,ensure_ascii=False,indent=2))
if __name__=='__main__':main()
