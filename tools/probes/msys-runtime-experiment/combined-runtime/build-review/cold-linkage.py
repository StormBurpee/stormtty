"""Small cold PE linkage excerpts; no loader, execution or approval path."""
import hashlib,struct
MAX_BYTES=8192

def excerpt(pe,rva,size):
    if not 0<size<=256:raise ValueError('excerpt bound')
    data=pe.at(rva,size)
    return dict(rva=rva,size=size,hex=data.hex(),sha256=hashlib.sha256(data).hexdigest())
def name_excerpt(pe,rva,expected=None):
    name=pe.string(rva)
    if expected is not None and name!=expected:raise ValueError('name mismatch')
    return dict(name=name,**excerpt(pe,rva,len(name.encode('ascii'))+1))
def import_binding(pe,slot,expected):
    rva,size=pe.imports_directory
    for i in range(min(size//20,128)):
        at=rva+i*20;raw=pe.at(at,20)
        original,stamp,chain,name,first=struct.unpack('<IIIII',raw)
        if not any((original,stamp,chain,name,first)):break
        if slot<first or (slot-first)%8:continue
        index=(slot-first)//8
        if index>=8192:continue
        table=original or first
        # Prove selected slot precedes the bounded terminator, not just lies nearby.
        for j in range(index+1):
            item=struct.unpack('<Q',pe.at(table+j*8,8))[0]
            if not item:break
        else:
            if item>>63:raise ValueError('ordinal accessor import')
            dll=name_excerpt(pe,name);symbol=name_excerpt(pe,item+2)
            if (dll['name'].lower(),symbol['name'])!=expected:raise ValueError('wrong import identity')
            iat=excerpt(pe,slot,8)
            if iat['hex']!=excerpt(pe,table+index*8,8)['hex']:raise ValueError('bound or rewritten IAT')
            return dict(descriptor=excerpt(pe,at,20),lookup=excerpt(pe,table+index*8,8),iat=iat,dll=dll,hint=excerpt(pe,item,2),symbol=symbol)
    raise ValueError('unresolved import slot')
def export_binding(pe,name):
    rva,size=pe.export;fields=struct.unpack('<IIHHIIIIIII',pe.at(rva,40));nf,nn,functions,names,ordinals=fields[6:]
    if not 1<=nn<=nf<=16384:raise ValueError('export bound')
    found=[]
    for i in range(nn):
        nrva=struct.unpack('<I',pe.at(names+4*i,4))[0]
        if pe.string(nrva)!=name:continue
        ordinal=struct.unpack('<H',pe.at(ordinals+2*i,2))[0]
        if ordinal>=nf:raise ValueError('export ordinal')
        target=struct.unpack('<I',pe.at(functions+4*ordinal,4))[0]
        if target!=pe.code_export(name):raise ValueError('export binding mismatch')
        found.append(dict(directory=excerpt(pe,rva,40),name_pointer=excerpt(pe,names+4*i,4),name=name_excerpt(pe,nrva,name),ordinal=excerpt(pe,ordinals+2*i,2),function=excerpt(pe,functions+4*ordinal,4),code_rva=target))
    if len(found)!=1:raise ValueError('export name ambiguity')
    return found[0]
def reader_names(pe,names):
    result={}
    # Export-name literals only, not a section dump. Each must be unique.
    for name in names:
        needle=name.encode('ascii')+b'\0';matches=[]
        for section,_,va,length,raw in pe.sections:
            if section.rstrip(b'\0')!=b'.rdata':continue
            data=pe.take(raw,length);start=0
            while True:
                at=data.find(needle,start)
                if at<0:break
                matches.append(va+at);start=at+1
        if len(matches)!=1:raise ValueError('reader name absent or ambiguous: '+name)
        result[name]=name_excerpt(pe,matches[0],name)
    return result

def reader_name_pointers(pe,names):
    result={name:[] for name in names}
    for section,_,va,length,raw in pe.sections:
        if section.rstrip(b'\0')!=b'.rdata':continue
        data=pe.take(raw,length)
        for offset in range((-va)%8,length-7,8):
            pointer=struct.unpack_from('<Q',data,offset)[0]
            for name,record in names.items():
                if pointer!=pe.base+record['rva']:continue
                result[name].append(excerpt(pe,va+offset,8))
                if len(result[name])>4:raise ValueError('reader name pointer bound')
    return result

def evidence(pe,rp,exports,calls,emitter,errno,accessors,emitter_name):
    bindings={}
    for name,target in calls:
        if name==emitter_name:
            if target!=emitter:raise ValueError('emitter mismatch')
        elif name=='__errno':
            if target!=errno:raise ValueError('errno mismatch')
        elif name in accessors:
            slot=pe.os_thunk(target,name)
            record=dict(thunk=excerpt(pe,target,6),binding=import_binding(pe,slot,('kernel32.dll',name)))
            if name in bindings and bindings[name]!=record:raise ValueError('ambiguous accessor')
            bindings[name]=record
        else:raise ValueError('unknown handler callee '+name)
    if set(bindings)!=set(accessors):raise ValueError('missing accessor')
    result=dict(schema=1,scope='bounded-cold-linkage-excerpts',fixtures_executed=False,approved_for_fixture_execution=False,
                runtime_base=pe.base,reader_base=rp.base,accessors=bindings,
                runtime_exports={name:export_binding(pe,name) for name in (*exports,'__errno')},reader_names=reader_names(rp,exports))
    result['reader_name_pointers']=reader_name_pointers(rp,result['reader_names'])
    def byte_count(value):
        if isinstance(value,dict):return (len(value['hex'])//2 if 'hex' in value else 0)+sum(byte_count(v) for k,v in value.items() if k!='hex')
        if isinstance(value,list):return sum(byte_count(v) for v in value)
        return 0
    result['selected_bytes']=byte_count(result)
    if result['selected_bytes']>MAX_BYTES:raise ValueError('total excerpt bound')
    return result
