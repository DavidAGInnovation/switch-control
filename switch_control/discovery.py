"""IPv4 DNS-SD discovery without scanning the subnet or requiring extra packages."""
import secrets,socket,struct,time
SERVICE='_switch-control._tcp.local'

def name_bytes(name):
    return b''.join(bytes([len(p.encode())])+p.encode() for p in name.rstrip('.').split('.'))+b'\0'

def read_name(data,offset):
    at=offset;end=None;parts=[];seen=set()
    for _ in range(128):
        if at>=len(data) or at in seen:raise ValueError('Invalid DNS name')
        seen.add(at);n=data[at];at+=1
        if n&0xc0==0xc0:
            if at>=len(data):raise ValueError('Truncated DNS pointer')
            if end is None:end=at+1
            at=((n&63)<<8)|data[at];continue
        if n&0xc0 or n>63 or at+n>len(data):raise ValueError('Invalid DNS label')
        if not n:return '.'.join(parts),end if end is not None else at
        parts.append(data[at:at+n].decode('utf-8'));at+=n
    raise ValueError('DNS pointer depth exceeded')

def parse_records(data):
    if len(data)<12:raise ValueError('Truncated DNS packet')
    ident,flags,q,a,ns,ar=struct.unpack_from('!6H',data)
    if not flags&0x8000 or q+a+ns+ar>128:raise ValueError('Not a bounded DNS response')
    at=12
    for _ in range(q):_,at=read_name(data,at);at+=4
    records=[]
    for _ in range(a+ns+ar):
        name,at=read_name(data,at)
        if at+10>len(data):raise ValueError('Truncated DNS record')
        kind,cls,ttl,n=struct.unpack_from('!HHIH',data,at);at+=10;end=at+n
        if end>len(data):raise ValueError('Truncated DNS data')
        value=None
        if kind==1 and n==4:value=socket.inet_ntoa(data[at:end])
        elif kind==12:value,_=read_name(data,at)
        elif kind==33 and n>=7:value={'port':struct.unpack_from('!H',data,at+4)[0],'host':read_name(data,at+6)[0]}
        elif kind==16:
            value={};p=at
            while p<end:
                size=data[p];p+=1
                if p+size>end:raise ValueError('Truncated TXT record')
                text=data[p:p+size].decode('utf-8');p+=size;k,_,v=text.partition('=');value[k]=v
        records.append((name.lower(),kind,ttl,value));at=end
    return ident,records

def discover(timeout=2.0,*,destination=('224.0.0.251',5353)):
    if not 0<timeout<=10:raise ValueError('Discovery timeout must be within 0..10 seconds')
    multicast=destination==('224.0.0.251',5353)
    ident=0 if multicast else secrets.randbelow(65536)
    query=struct.pack('!6H',ident,0,1,0,0,0)+name_bytes(SERVICE)+struct.pack('!HH',12,1 if multicast else 0x8001)
    found={};records={}
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.IPPROTO_IP,socket.IP_MULTICAST_TTL,255)
        if multicast:
            # Share the mDNS port with the OS resolver and receive multicast
            # replies. Ephemeral-port QU queries are filtered on some LANs.
            sock.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
            if hasattr(socket,'SO_REUSEPORT'):sock.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEPORT,1)
            sock.bind(('',5353))
            sock.setsockopt(socket.IPPROTO_IP,socket.IP_ADD_MEMBERSHIP,socket.inet_aton(destination[0])+socket.inet_aton('0.0.0.0'))
        else:sock.bind(('',0))
        sock.sendto(query,destination);deadline=time.monotonic()+timeout
        while (left:=deadline-time.monotonic())>0:
            sock.settimeout(left)
            try:data,peer=sock.recvfrom(9000)
            except socket.timeout:break
            try:response,rows=parse_records(data)
            except (ValueError,UnicodeError,struct.error):continue
            if response not in (0,ident):continue
            for name,kind,ttl,value in rows:
                if ttl and value is not None:records[name,kind]=value
            for (name,kind),instance in list(records.items()):
                if name!=SERVICE or kind!=12:continue
                srv=records.get((instance.lower(),33));txt=records.get((instance.lower(),16),{})
                if not srv or txt.get('auth')!='tls-psk':continue
                ip=records.get((srv['host'].lower(),1),peer[0])
                found[ip,srv['port']]={'host':ip,'port':srv['port'],'hostname':srv['host'],'name':instance,'properties':txt}
    return sorted(found.values(),key=lambda x:(x['hostname'],x['host']))
