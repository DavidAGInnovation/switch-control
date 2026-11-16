#!/usr/bin/env python3
"""SD file client over the authenticated TLS control transport."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import time
from wifi_tls_controller import command

class FileError(RuntimeError):
    pass

def path_arg(path):
    if not path.startswith('/') or '\0' in path:
        raise ValueError('Use an absolute SD path, such as /switch/example.nro')
    return path.encode('utf-8').hex()

def digest_file(path):
    with open(path,'rb') as f:
        return hashlib.file_digest(f,'sha256').hexdigest()

class Files:
    def __init__(self,host,auth_key,tls_key,port=6012):
        self.host,self.auth_key,self.tls_key,self.port=host,auth_key,tls_key,port
    def request(self,text,body=b''):
        reply=command(self.host,self.auth_key,self.tls_key,text,self.port,body=body)
        if reply.startswith(b'ERR'):
            raise FileError(reply.decode('ascii','replace').strip())
        return reply
    def json(self,text,body=b''):
        try:result=json.loads(self.request(text,body))
        except (ValueError,UnicodeError) as e:raise FileError('Malformed file-service reply') from e
        if not isinstance(result,dict):raise FileError('Malformed file-service reply')
        if 'error' in result:raise FileError(f'{result["error"]} (errno {result.get("errno")})')
        return result
    def info(self):return self.json('fsInfo')
    def stat(self,path):return self.json('fsStat '+path_arg(path))
    def finish(self,result,progress=None,cancel=None):
        if 'job' not in result:return result
        token=result['job'];deadline=time.monotonic()+10800
        try:
            while result.get('active'):
                if cancel and cancel():raise InterruptedError('File job cancelled')
                if time.monotonic()>deadline:raise TimeoutError('File job timed out')
                if progress:progress(result)
                time.sleep(.05);result=self.json('fsTaskInfo '+token)
            if result.get('state')!='completed':raise FileError('File job '+result.get('state','failed'))
            return {**result,'ok':True}
        except BaseException:
            try:self.json('fsTaskCancel '+token)
            except Exception:pass
            raise
    def hash(self,path):return self.finish(self.json('fsHash '+path_arg(path)))['sha256']
    def tail(self,path,size=4096):return self.request(f'fsTail {path_arg(path)} {size}')
    def list(self,path):
        result=[];cursor=0
        while True:
            page=self.json(f'fsList {path_arg(path)} {cursor}')
            result.extend(page['entries']);next_cursor=page['next']
            if next_cursor==-1:return result
            if not isinstance(next_cursor,int) or next_cursor<=cursor:raise FileError('Non-progressing directory page')
            cursor=next_cursor
    def mkdir(self,path):return self.json('fsMkdir '+path_arg(path))
    def delete(self,path,expected):return self.finish(self.json(f'fsDelete {path_arg(path)} {expected}'))
    def move(self,source,destination,expected):return self.finish(self.json(f'fsMove {path_arg(source)} {path_arg(destination)} {expected}'))
    def abort(self,token):return self.json('fsAbort '+token)
    def upload(self,local,remote,expected='missing'):
        local=Path(local);info=self.info();size=local.stat().st_size
        if size>info['max_file_bytes']:raise ValueError('File exceeds server upload limit')
        wanted=digest_file(local)
        start=self.json(f'fsBegin {path_arg(remote)} {size} {wanted} {expected}')
        token=start['upload'];offset=0
        try:
            with local.open('rb') as f:
                while block:=f.read(info['chunk_bytes']):
                    self.json(f'fsChunk {token} {offset} {len(block)}',block);offset+=len(block)
            if offset!=size:raise FileError('Local file changed size during upload')
            result=self.finish(self.json('fsCommit '+token))
        except BaseException:
            try:self.abort(token)
            except Exception:pass # Preserve the original failure; never replay a write.
            raise
        # The server hashes the incoming stream and the staged/published files.
        # Verify once more through the public API before reporting success.
        if self.hash(remote)!=wanted:raise FileError('Published file changed before final verification')
        return {**result,'sha256':wanted,'bytes':size}
    def download(self,remote,local):
        local=Path(local)
        if local.exists():raise FileExistsError(local)
        info=self.info();stat=self.stat(remote)
        if not stat.get('exists') or stat.get('type')!='file':raise FileError('Remote path is not a file')
        if stat['size']>info['max_file_bytes']:raise ValueError('File exceeds server verified-download limit')
        expected=self.hash(remote);stage=local.with_name(local.name+'.part-'+secrets.token_hex(8));offset=0;digest=hashlib.sha256()
        try:
            with stage.open('xb') as f:
                while offset<stat['size']:
                    reply=self.request(f'fsRead {path_arg(remote)} {offset} {min(info["chunk_bytes"],stat["size"]-offset)}')
                    header,sep,body=reply.partition(b'\n')
                    if not sep or not header.startswith(b'FILE '):
                        try:detail=json.loads(reply)
                        except (ValueError,UnicodeError):detail='Malformed file response'
                        raise FileError(str(detail))
                    try:at,n,total=map(int,header.split()[1:])
                    except ValueError as e:raise FileError('Malformed file header') from e
                    if at!=offset or n!=len(body) or not n or total!=stat['size'] or n>info['chunk_bytes']:raise FileError('Remote file changed or read was truncated')
                    f.write(body);digest.update(body);offset+=n
                f.flush();os.fsync(f.fileno())
            if digest.hexdigest()!=expected or self.hash(remote)!=expected:raise FileError('Remote file changed during download')
            # Hard-link publication refuses a destination created concurrently.
            os.link(stage,local);stage.unlink()
        finally:
            stage.unlink(missing_ok=True)
        return {'sha256':expected,'bytes':offset}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host',required=True);p.add_argument('--key-file',type=Path,required=True);p.add_argument('--tls-key-file',type=Path,required=True);p.add_argument('--port',type=int,default=6012)
    sub=p.add_subparsers(dest='operation',required=True)
    sub.add_parser('info')
    for op in ['list','stat','hash','mkdir']:
        q=sub.add_parser(op);q.add_argument('remote')
    q=sub.add_parser('get');q.add_argument('remote');q.add_argument('local',type=Path)
    q=sub.add_parser('put');q.add_argument('local',type=Path);q.add_argument('remote');q.add_argument('--expected-sha256',default='missing',help='Required old hash to replace an existing file; default creates only')
    q=sub.add_parser('delete');q.add_argument('remote');q.add_argument('--expected-sha256',required=True)
    q=sub.add_parser('move');q.add_argument('remote');q.add_argument('destination');q.add_argument('--expected-sha256',required=True)
    q=sub.add_parser('abort');q.add_argument('upload')
    a=p.parse_args();files=Files(a.host,a.key_file.read_bytes(),a.tls_key_file.read_bytes(),a.port)
    if a.operation=='put':result=files.upload(a.local,a.remote,a.expected_sha256)
    elif a.operation=='get':result=files.download(a.remote,a.local)
    elif a.operation=='delete':result=files.delete(a.remote,a.expected_sha256)
    elif a.operation=='move':result=files.move(a.remote,a.destination,a.expected_sha256)
    elif a.operation=='abort':result=files.abort(a.upload)
    elif a.operation=='info':result=files.info()
    else:result=getattr(files,a.operation)(a.remote)
    print(json.dumps(result,ensure_ascii=False,indent=2))
if __name__=='__main__':main()
