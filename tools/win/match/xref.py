# Who calls / references a Windows address (or a mac symbol's matched Windows function).
#   python3 -I xref.py 0x<winaddr>         callers and data refs of a Windows address
#   python3 -I xref.py --mac <symbol|0x>   callers of a mac function (with their Windows matches)
import os
import sys, pickle, subprocess
R=os.environ.get('RBWIN', os.path.expanduser('~/rbwin')) + '/'
mt=pickle.load(open(R+'m.pkl','rb')); match=mt['match']; rmatch={w:m for m,w in match.items()}
def dem(n): return subprocess.run(['c++filt'],input=n[1:] if n.startswith('__Z') else n,capture_output=True,text=True).stdout.strip()
if sys.argv[1]=='--mac':
    mac=pickle.load(open(R+'mac.pkl','rb')); M=mac['funcs']; x=sys.argv[2]
    a=int(x,16) if x.startswith('0x') else next(k for k,f in M.items() if x in f['names'])
    for k,f in M.items():
        n=sum(1 for c in f['calls'] if (c[1] if isinstance(c,tuple) else c)==a)
        if n: print('%x x%d %s %s'%(k,n,dem(f['names'][0])[:110],('[=win %x]'%match[k]) if k in match else ''))
else:
    win=pickle.load(open(R+'win.pkl','rb')); t=int(sys.argv[1],16)
    for k,f in win['funcs'].items():
        n=sum(1 for c in f['calls'] if (c[1] if isinstance(c,tuple) else c)==t)
        r=f['refs'].count(t)
        if n or r:
            root=f['root']; m=rmatch.get(root)
            print('%x (fn %x) calls x%d refs x%d %s'%(k,root,n,r,('= '+dem(pickle.load(open(R+'mac.pkl','rb'))['funcs'][m]['names'][0])[:100]) if m else ''))
