# For each Offset recipe: does its expected value appear in the mac x86 function, and in the Windows counterpart?
import os
import json, pickle, sys, re, collections
R=os.environ.get('RBWIN', os.path.expanduser('~/rbwin')) + '/'
offs=json.load(open(R+'offsets.json'))
mac=pickle.load(open(R+'mac.pkl','rb')); win=pickle.load(open(R+'win.pkl','rb')); mt=pickle.load(open(R+sys.argv[1],'rb'))
M=mac['funcs']; byname={n:a for a,f in M.items() for n in f['names']}
dn={n:a for a,ns in mac['datanames'].items() for n in ns}
import capstone
md=capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
import pefile
pe=pefile.PE(R+'x/rekordbox.exe',fast_load=True); base=pe.OPTIONAL_HEADER.ImageBase
macd=open(R+'mac/rekordbox_x86','rb').read()
def imms(data,foff,va,size):
    s=collections.Counter()
    for (a,sz,mn,op) in md.disasm_lite(data[foff:foff+size],va):
        for v in re.findall(r'[+-] (0x[0-9a-f]+)\]|, (0x[0-9a-f]+)$',op):
            for x in v:
                if x: s[int(x,16)]+=1
    return s
def winimms(w):
    s=collections.Counter()
    for c,f in win['funcs'].items():
        if f['root']==w: s+=imms(pe.__data__,pe.get_offset_from_rva(c-base),c,f['size'])
    return s
c=collections.Counter()
for o in offs:
    k=o['kind']
    if k.startswith('vt:'): o['status']='vtable-slot'; c['vt']+=1; continue
    a=byname.get(o['fn'])
    if a is None: o['status']='fn-not-func' if o['fn'] in dn else 'fn-missing'; c[o['status']]+=1; continue
    mi=imms(macd,a-0x100000000,a,M[a]['size'])
    w=mt['match'].get(a)
    inmac=o['expected'] in mi
    if not w: o['status']='win-unmatched'; c['win-unmatched']+=1; continue
    wi=winimms(w)
    o['win']=hex(w)
    o['status']=('same' if o['expected'] in wi else 'differs') + ('' if inmac else '?macx86-absent')
    c[o['status']]+=1
print(c)
json.dump(offs,open(R+'offsets_status.json','w'),indent=0)
for o in offs:
    if o['status'].startswith('differs'): print(o['file'],o['name'],hex(o['expected']),o['kind'])
