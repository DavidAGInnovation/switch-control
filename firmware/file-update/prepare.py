#!/usr/bin/env python3
"""Embed reviewed service/recovery executables in the guarded updater."""
import argparse,hashlib,json,shutil,re
from pathlib import Path
ROOT=Path(__file__).resolve().parent
p=argparse.ArgumentParser(description=__doc__);p.add_argument('new_service',type=Path);p.add_argument('recovery',type=Path);p.add_argument('output',type=Path);p.add_argument('--version',required=True);p.add_argument('--old-sha256',required=True);a=p.parse_args();assert not a.output.exists();old=bytes.fromhex(a.old_sha256);assert len(old)==32
shutil.copytree(ROOT,a.output,ignore=shutil.ignore_patterns('__pycache__','build','*.nro','*.elf','*.nacp','*.map','payloads.h'))
assert re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+',a.version)
fingerprint=hashlib.sha256(a.new_service.read_bytes()).hexdigest()[:12]
name='switch-control-update-'+a.version+'-'+fingerprint+'.nro';updater_name=name
recovery_name='switch-control-recovery-'+fingerprint+'.nro'
rows=['#define UPDATE_PREVIOUS_PATH '+json.dumps('sdmc:/diagnostic-tools/2026-11-16/switch-control-before-'+a.version+'-'+a.old_sha256[:12]+'.nsp')+'\n','#define UPDATE_RECOVERY_PATH '+json.dumps('sdmc:/diagnostic-tools/2026-11-16/'+recovery_name)+'\n','#define UPDATE_SELF_NAME '+json.dumps(name)+'\n'];hashes={}
make=a.output/'Makefile';make.write_text(make.read_text().replace('switch-control-file-update',name.removesuffix('.nro')))
for label,path in [('new_bytes',a.new_service),('recovery_bytes',a.recovery)]:
 data=path.read_bytes();hashes[label]=hashlib.sha256(data).hexdigest();rows.append('static const unsigned char '+label+'[]={'+','.join(map(str,data))+'};\n');name='new_hash' if label=='new_bytes' else 'recovery_hash';rows.append('static const unsigned char '+name+'[32]={'+','.join(map(str,bytes.fromhex(hashes[label])))+'};\n')
rows.append('static const unsigned char old_hash[32]={'+','.join(map(str,old))+'};\n');(a.output/'source/payloads.h').write_text(''.join(rows));(a.output/'payloads.json').write_text(json.dumps({'previous_service_sha256':a.old_sha256,'updater_name':updater_name,'recovery_name':recovery_name,**hashes},indent=2)+'\n')
