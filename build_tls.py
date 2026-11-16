#!/usr/bin/env python3
"""Build the TLS service in a new folder with the pinned external Mbed TLS tree."""
import argparse,hashlib,json,os,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parent
MBED_COMMIT='068ff080b369adfac81509f9b57b2afabaf82dc5'
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--mbedtls-root',type=Path,required=True);p.add_argument('output',type=Path);a=p.parse_args()
    mbed=a.mbedtls_root.resolve(strict=True);out=a.output.resolve();assert not out.exists(),'Output must be a new folder'
    assert subprocess.check_output(['git','-C',str(mbed),'rev-parse','HEAD'],text=True).strip()==MBED_COMMIT
    assert (mbed/'library/libmbedtls.a').is_file(),'Prepare the Switch dependency first'
    module=out/'usb-resident';module.mkdir(parents=True)
    inputs={}
    for name in ['Makefile','usb-control.json','source/main.c','source/wifi-recovery.h','source/sd-files.h','source/stick.h','source/system-controls.h','source/discovery.h','source/api-json.h','source/api-tools.h','source/api-core.h','source/api-http.h','source/api-native.h','source/input-jobs.h','source/api-input.h','source/file-jobs.h','source/system-info.h','source/power-lifecycle.h','source/utf8-text.h','source/config-parse.h','source/network-config.h','source/clock-controls.h','source/api-config.h']:
        source=ROOT/'usb-resident'/name;target=module/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,target);inputs[name]=hashlib.sha256(source.read_bytes()).hexdigest()
    reader=ROOT/'reader/source/main.c';target=out/'reader/source/main.c';target.parent.mkdir(parents=True);shutil.copyfile(reader,target);inputs['reader']=hashlib.sha256(reader.read_bytes()).hexdigest()
    descriptor=json.loads((module/'usb-control.json').read_text());descriptor.update(name='switch-control',program_id='0x430000000000000D',program_id_range_min='0x430000000000000D',program_id_range_max='0x430000000000000D',version='0.5.0',main_thread_stack_size='0x40000');(module/'control-tls-files.json').write_text(json.dumps(descriptor,indent=2)+'\n')
    subprocess.run(['docker','run','--rm','-u',f'{os.getuid()}:{os.getgid()}','-v',f'{out}:/work','-v',f'{mbed}:/mbedtls:ro','-w','/work/usb-resident','devkitpro/devkita64:20260219@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282','make','-j4','CONFIG_JSON=control-tls-files.json','ZA_MBEDTLS_ROOT=/mbedtls','DEFINES=-DZA_WIFI_TLS_PROGRAM_D=1 -DCONTROL_NATIVE_HTTP=1'],check=True)
    binary=module/'usb-control.nsp';result={'image':'devkitpro/devkita64:20260219@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282','mbedtls_commit':MBED_COMMIT,'source_sha256':inputs,'service_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'service_bytes':binary.stat().st_size,'program_id':'430000000000000D','installed':False};(out/'build.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
