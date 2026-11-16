"""Typed client actions and schemas; REST and MCP run natively on the Switch."""
import base64,json,math,threading
from pathlib import Path
from .wifi_tls_controller import command
from .sd_files import Files,path_arg,FileError
from .discovery import discover
VERSION='0.5.1'
BUTTONS={'A','B','X','Y','L','R','ZL','ZR','PLUS','MINUS','UP','DOWN','LEFT','RIGHT','LSTICK','RSTICK','HOME','CAPTURE'}
ALIASES={'UP':'DUP','DOWN':'DDOWN','LEFT':'DLEFT','RIGHT':'DRIGHT'}

# libnx HID ABI, including hiddbg's HOME/CAPTURE bits.
MASKS={name:1<<i for i,name in enumerate(['A','B','X','Y','LSTICK','RSTICK','L','R','ZL','ZR','PLUS','MINUS','LEFT','UP','RIGHT','DOWN'])}
MASKS.update(HOME=1<<18,CAPTURE=1<<19)
def job_wire(steps):
    if not isinstance(steps,list) or not 1<=len(steps)<=32:raise ValueError('Expected 1..32 steps')
    result=[];total=0
    for step in steps:
        buttons=step.get('buttons',[step['button']] if 'button' in step else [])
        if 'buttons' in step and 'button' in step:raise ValueError('Use button or buttons')
        if len(buttons)>8 or len(set(buttons))!=len(buttons) or any(b not in MASKS for b in buttons):raise ValueError('Invalid button chord')
        mask=sum(MASKS[b] for b in buttons);axes=[round(number(step.get(k,0),-1,1)*32767) for k in ['left_x','left_y','right_x','right_y']]
        hold=integer(step.get('duration_ms',100),1,10000);wait=integer(step.get('wait_ms',100),0,10000);total+=hold+wait
        result.append(f'{mask:x}:'+','.join(map(str,axes))+f':{hold}:{wait}')
    if total>120000:raise ValueError('Sequence exceeds 120 seconds')
    return ';'.join(result)

def number(value,low,high):
    if isinstance(value,bool) or not isinstance(value,(int,float)) or not math.isfinite(value) or not low<=value<=high:raise ValueError(f'Expected a number in {low}..{high}')
    return value

def integer(value,low,high):
    number(value,low,high)
    if not isinstance(value,int):raise ValueError('Expected an integer')
    return value

class Switch:
    def __init__(self,host,auth_key,tls_key,port=6012):
        self.host,self.auth_key,self.tls_key,self.port=host,auth_key,tls_key,port;self.lock=threading.RLock()
    def resolve(self):
        if self.host not in (None,'auto'):return
        paired=[]
        for item in discover():
            try:command(item['host'],self.auth_key,self.tls_key,'getVersion',item['port'])
            except (OSError,ValueError):continue
            paired.append(item)
        if len(paired)!=1:raise ValueError(f'Expected one paired Switch; discovered {len(paired)}. Set host explicitly when using several consoles.')
        self.host,self.port=paired[0]['host'],paired[0]['port']
    def raw(self,text):
        self.resolve();reply=command(self.host,self.auth_key,self.tls_key,text,self.port)
        if reply.startswith(b'ERR'):raise FileError(reply.decode('ascii','replace').strip())
        return reply
    def files(self):self.resolve();return Files(self.host,self.auth_key,self.tls_key,self.port)
    def status(self):return json.loads(self.raw('systemStatus'))
    def screenshot(self):
        raw=self.raw('pixelPeek');header,sep,data=raw.partition(b'\n')
        if not sep or header!=b'JPEG '+str(len(data)).encode() or not data.startswith(b'\xff\xd8') or not data.endswith(b'\xff\xd9'):raise ValueError('Invalid screenshot reply')
        return data
    def tap(self,buttons,duration_ms=100):
        integer(duration_ms,1,10000)
        if not isinstance(buttons,list) or not 1<=len(buttons)<=8 or len(set(buttons))!=len(buttons) or any(b not in BUTTONS for b in buttons):raise ValueError('Expected 1..8 distinct known buttons')
        if duration_ms>1000:return self.sequence([{'buttons':buttons,'duration_ms':duration_ms,'wait_ms':0}])
        if buttons==['HOME'] and duration_ms==100:return {'result':self.raw('click HOME').decode().strip()}
        names=[ALIASES.get(x,x) for x in buttons];steps=['+'+x for x in names]+[f'W{duration_ms}']+['-'+x for x in names]+['W100']
        return {'result':self.raw('clickSeq '+','.join(steps)).decode().strip()}
    def sequence(self,steps):return json.loads(self.raw('inputStart '+job_wire(steps)))
    def stick(self,side,x,y,duration_ms=100):
        if side not in ('left','right'):raise ValueError('Side must be left or right')
        number(x,-1,1);number(y,-1,1);integer(duration_ms,1,10000)
        if duration_ms>1000:return self.sequence([{('left_x' if side=='left' else 'right_x'):x,('left_y' if side=='left' else 'right_y'):y,'duration_ms':duration_ms,'wait_ms':0}])
        return {'result':self.raw(f'stick {"L" if side=="left" else "R"} {round(x*32767)} {round(y*32767)} {duration_ms}').decode().strip()}
    def setting(self,name,value):
        if name in ('brightness','volume'):wire=str(number(value,0,1))
        elif name=='auto_brightness':
            if not isinstance(value,bool):raise ValueError('Expected boolean')
            wire=str(value).lower()
        elif name=='theme':
            if value not in ('light','dark'):raise ValueError('Expected light or dark')
            wire=value
        elif name=='nickname':
            if not isinstance(value,str) or not value or len(value.encode())>127 or any(ord(c)<32 or ord(c)==127 for c in value):raise ValueError('Invalid nickname')
            wire=value.encode().hex()
        else:raise ValueError('Unknown setting')
        result=json.loads(self.raw(f'systemSet {name} {wire}'))
        if 'error' in result:raise FileError(str(result))
        return result
    def read(self,path,offset=0,size=65536):
        integer(offset,0,2**63-1);integer(size,1,65536);data=self.raw(f'fsRead {path_arg(path)} {offset} {size}');header,sep,body=data.partition(b'\n')
        if not sep or not header.startswith(b'FILE '):raise FileError('Invalid file read')
        at,count,total=map(int,header.split()[1:])
        if at!=offset or count!=len(body) or count>size:raise FileError('Invalid file range')
        return {'offset':at,'size':count,'total':total,'base64':base64.b64encode(body).decode()}
    def write(self,path,content_base64,expected_sha256='missing'):
        import tempfile
        if not isinstance(content_base64,str) or len(content_base64)>65536:raise ValueError('Use REST binary upload for files larger than 48 KiB')
        data=base64.b64decode(content_base64,validate=True)
        with tempfile.TemporaryDirectory() as t:
            p=Path(t)/'payload';p.write_bytes(data);return self.files().upload(p,path,expected_sha256)
    def call(self,name,args):
        with self.lock:
            if name in ('get_datetime','get_auto_time'):return json.loads(self.raw('clockInfo'))
            if name=='dns_update_status':return json.loads(self.raw('dnsStatus'))
            if name=='set_auto_time':return json.loads(self.raw('clockSync '+str(args['enabled']).lower()))
            if name=='set_dns':
                import ipaddress
                if args['automatic']:
                    if 'primary' in args or 'secondary' in args:raise ValueError('Automatic DNS has no addresses')
                    wire='dnsSet true - -'
                else:
                    first=str(ipaddress.IPv4Address(args['primary']));second=str(ipaddress.IPv4Address(args.get('secondary','0.0.0.0')));wire=f'dnsSet false {first} {second}'
                return json.loads(self.raw(wire))
            if name=='set_datetime':
                fields=['year','month','day','hour','minute','second']
                if 'timestamp' in args:
                    if set(args)!={'timestamp'}:raise ValueError('Use UTC timestamp or complete local fields')
                    wire='clockSetUtc '+str(integer(args['timestamp'],946684800,4102444799))
                else:
                    if set(args)!=set(fields):raise ValueError('All six local fields are required')
                    from datetime import datetime
                    datetime(**args);wire='clockSetLocal '+' '.join(str(args[k]) for k in fields)
                return json.loads(self.raw(wire))
            if name in ('network_status','lifecycle_status'):return json.loads(self.raw('networkInfo' if name=='network_status' else 'lifecycleInfo'))
            if name=='list_installed_titles':return json.loads(self.raw('listTitles '+str(args.get('cursor',0))))
            if name in ('input_status','cancel_input','renew_input'):
                cmd={'input_status':'inputStatus','cancel_input':'inputCancel','renew_input':'inputRenew'}[name]+' '+args['action']
                if name=='renew_input':cmd+=' '+str(args['lease_ms'])
                return json.loads(self.raw(cmd))
            if name=='run_input':return self.sequence(args['steps'])
            if name=='hold_input':
                step={k:v for k,v in args.items() if k!='lease_ms'};step.update(duration_ms=args['lease_ms'],wait_ms=0)
                return json.loads(self.raw('inputLease '+job_wire([step])))
            if name=='start_hash':return self.files().json('fsHashStart '+path_arg(args['path']))
            if name in ('file_job_status','cancel_file_job'):return self.files().json(('fsTaskInfo ' if name=='file_job_status' else 'fsTaskCancel ')+args['job'])
            if name=='tail_file':return self.files().tail(args['path'],args.get('size',4096))
            if name=='status':return self.status()
            if name=='screenshot':return self.screenshot()
            if name=='tap_buttons':return self.tap(**args)
            if name=='tap_sequence':return self.sequence(**args)
            if name=='set_stick':return self.stick(**args)
            if name=='clear_input':return {'result':self.raw('inputClear').decode().strip()}
            if name=='set_system':return self.setting(**args)
            if name=='list_directory':return self.files().json(f'fsList {path_arg(args["path"])} {args.get("cursor",0)}')
            if name=='stat_file':return self.files().stat(args['path'])
            if name=='hash_file':return {'sha256':self.files().hash(args['path'])}
            if name=='read_file':return self.read(**args)
            if name=='write_file':return self.write(**args)
            if name=='make_directory':return self.files().mkdir(args['path'])
            if name=='delete_file':return self.files().delete(args['path'],args['expected_sha256'])
            if name=='move_file':return self.files().move(args['source'],args['destination'],args['expected_sha256'])
            raise ValueError('Unknown tool')

def schema(properties,required=()):return {'type':'object','properties':properties,'required':list(required),'additionalProperties':False}
STRING={'type':'string'};NUMBER={'type':'number','minimum':-1,'maximum':1};MS={'type':'integer','minimum':1,'maximum':10000}
TOOLS=[]
def tool(name,description,properties={},required=(),read=False):
    TOOLS.append({'name':name,'description':description,'inputSchema':schema(properties,required),'annotations':{'readOnlyHint':read,'destructiveHint':not read,'openWorldHint':False}})
tool('status','Console firmware, battery, charging, brightness, volume, theme, nickname and controller status.',read=True)
tool('screenshot','Capture the current screen, including HOME and homebrew, as JPEG.',read=True)
tool('tap_buttons','Press and release a button chord; the virtual controller detaches afterward.',{'buttons':{'type':'array','items':{'type':'string','enum':sorted(BUTTONS)},'minItems':1,'maxItems':8},'duration_ms':MS},['buttons'])
STEP={'button':{'type':'string','enum':sorted(BUTTONS)},'buttons':{'type':'array','items':{'type':'string','enum':sorted(BUTTONS)},'maxItems':8},'duration_ms':MS,'wait_ms':{'type':'integer','minimum':0,'maximum':10000},**{k:NUMBER for k in ['left_x','left_y','right_x','right_y']}}
for name in ['tap_sequence','run_input']:
    tool(name,'Start up to 32 combined button/stick steps, 120 seconds total. Returns action token. Renew its lease within 10s; poll or cancel by token.',{'steps':{'type':'array','items':schema(STEP),'minItems':1,'maxItems':32}},['steps'])
tool('hold_input','Hold combined buttons/sticks until cancelled or lease expires. Requires ownership token for renewal; lease_ms is 100..10000.',{**{k:v for k,v in STEP.items() if k not in ['duration_ms','wait_ms']},'lease_ms':{'type':'integer','minimum':100,'maximum':10000}},['lease_ms'])
for name in ['input_status','cancel_input']:tool(name,'Inspect or cancel the action owned by this random action token.',{'action':STRING},['action'],name=='input_status')
tool('renew_input','Renew an active action lease. Stale tokens cannot restart expired input.',{'action':STRING,'lease_ms':{'type':'integer','minimum':100,'maximum':10000}},['action','lease_ms'])
for name,description in [('network_status','Active IP, subnet, gateway, DNS and connection status; excludes credentials.'),('lifecycle_status','Power registration, sleep/wake counters and controller cleanup status.')]:tool(name,description,read=True)
tool('get_datetime','Current console date/time, UTC timestamp, timezone and automatic synchronization status.',read=True)
tool('get_auto_time','Current runtime and persistent automatic clock synchronization settings.',read=True)
tool('set_auto_time','Enable or disable automatic clock synchronization, updating runtime and saved preference.',{'enabled':{'type':'boolean'}},['enabled'])
tool('set_datetime','Set manual time and disable automatic sync. Supply either a UTC timestamp or all six fields in the console timezone. Ambiguous DST local times require UTC. Years 2000..2099.',{'timestamp':{'type':'integer','minimum':946684800,'maximum':4102444799},**{k:{'type':'integer','minimum':2000 if k=='year' else 0,'maximum':2099 if k=='year' else 255} for k in ['year','month','day','hour','minute','second']}})
tool('set_dns','Schedule a saved-profile DNS update. automatic=true restores automatic DNS; otherwise primary IPv4 is required and secondary defaults to 0.0.0.0. Returns operation ID; connection may reset. Query dns_update_status after reconnect; never replay a write.',{'automatic':{'type':'boolean'},'primary':STRING,'secondary':STRING},['automatic'])
tool('dns_update_status','Last scheduled DNS operation state and native result; excludes network credentials.',read=True)
tool('list_installed_titles','Read installed application IDs and names, in pages of 16.',{'cursor':{'type':'integer','minimum':0,'maximum':100000}},read=True)
tool('set_stick','Move an analog stick for 1..10000 ms. Above 1000 ms returns a cancellable action token. Positive Y is up.',{'side':{'type':'string','enum':['left','right']},'x':NUMBER,'y':NUMBER,'duration_ms':MS},['side','x','y'])
tool('clear_input','Release input and detach the virtual controller.')
tool('set_system','Change brightness/volume (0..1), auto_brightness (boolean), theme (light/dark), or nickname. Theme may require HOME to reload.',{'name':{'type':'string','enum':['brightness','volume','auto_brightness','theme','nickname']},'value':{'type':['string','number','boolean']}},['name','value'])
for name,description in [('list_directory','List an SD directory.'),('stat_file','Get SD path metadata.'),('hash_file','SHA-256 an SD file up to 8 GiB; large files return a cooperative verification job.')]:tool(name,description,{'path':STRING,**({'cursor':{'type':'integer','minimum':0}} if name=='list_directory' else {})},['path'],True)
tool('read_file','Read up to 64 KiB from an SD file; bytes are returned as base64.',{'path':STRING,'offset':{'type':'integer','minimum':0},'size':{'type':'integer','minimum':1,'maximum':65536}},['path'],True)
tool('tail_file','Read the last 1..65536 bytes of a log; result is base64.',{'path':STRING,'size':{'type':'integer','minimum':1,'maximum':65536}},['path'],True)
tool('start_hash','Start a cancellable SHA-256 job with bounded memory.',{'path':STRING},['path'],True)
for name in ['file_job_status','cancel_file_job']:tool(name,'Inspect or cancel a file-verification job by its random job token.',{'job':STRING},['job'],name=='file_job_status')
tool('write_file','Upload at most 48 KiB of base64 data with SHA-256 verification. Replacing an existing file requires its expected hash.',{'path':STRING,'content_base64':STRING,'expected_sha256':STRING},['path','content_base64'])
tool('make_directory','Create one SD directory.',{'path':STRING},['path'])
tool('delete_file','Delete one SD file only when its current hash matches.',{'path':STRING,'expected_sha256':STRING},['path','expected_sha256'])
tool('move_file','Move one SD file to an absent destination, checking its source hash.',{'source':STRING,'destination':STRING,'expected_sha256':STRING},['source','destination','expected_sha256'])

def validate(args,spec):
    if spec.get('type')=='object':
        if not isinstance(args,dict) or set(args)-set(spec['properties']) or any(k not in args for k in spec.get('required',[])):raise ValueError('Invalid or missing arguments')
        for key,value in args.items():validate(value,spec['properties'][key])
    elif spec.get('type')=='array':
        if not isinstance(args,list) or not spec.get('minItems',0)<=len(args)<=spec.get('maxItems',100):raise ValueError('Invalid array')
        for value in args:validate(value,spec['items'])
    else:
        kinds=spec.get('type',[]);kinds=[kinds] if isinstance(kinds,str) else kinds
        checks={'string':lambda:isinstance(args,str),'number':lambda:isinstance(args,(int,float)) and not isinstance(args,bool) and math.isfinite(args),'integer':lambda:isinstance(args,int) and not isinstance(args,bool),'boolean':lambda:isinstance(args,bool)}
        if kinds and not any(checks[k]() for k in kinds):raise ValueError('Invalid argument type')
        if 'enum' in spec and args not in spec['enum']:raise ValueError('Invalid argument value')
        if isinstance(args,(int,float)) and not isinstance(args,bool) and not spec.get('minimum',-math.inf)<=args<=spec.get('maximum',math.inf):raise ValueError('Argument outside range')

def invoke(switch,name,args):
    spec=next((t for t in TOOLS if t['name']==name),None)
    if not spec:raise ValueError('Unknown tool')
    validate(args,spec['inputSchema']);return switch.call(name,args)
