# Windows-only __FUNCTION__ strings -> {win root: qualified name}, and mac qualified names -> addrs.
import sys, pickle, re, subprocess, collections
FNRE=re.compile(r'^[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_~][A-Za-z0-9_]*)+$')
def load(macp, winp):
    mac=pickle.load(open(macp,'rb')); win=pickle.load(open(winp,'rb'))
    M=mac['funcs']
    names=[(a,n) for a,f in M.items() for n in f['names']]
    dem=subprocess.run(['c++filt'],input='\n'.join(n[1:] if n.startswith('__Z') else n for _,n in names),capture_output=True,text=True).stdout.split('\n')
    byq=collections.defaultdict(set)
    for (a,n),d in zip(names,dem):
        q=d.split('(')[0].replace('non-virtual thunk to ','')
        q=re.sub(r'\[abi:[^\]]*\]','',q)
        byq[q].add(a)
    roots=collections.defaultdict(set)
    for a,f in win['funcs'].items():
        for s in f['strs']:
            if FNRE.match(s): roots[f['root']].add(s)
    return byq, {w:next(iter(ss)) for w,ss in roots.items() if len(ss)==1 and next(iter(ss)) in byq}
if __name__=='__main__':
    byq,wn=load(sys.argv[1],sys.argv[2])
    r=pickle.load(open(sys.argv[3],'rb'))
    rm={w:m for m,w in r['match'].items()}
    c=collections.Counter()
    for w,q in wn.items():
        if w in rm: c[(r['how'].get(rm[w]), rm[w] in byq[q])]+=1
        else: c['unmatched']+=1
    print('truth', len(wn))
    for k,v in sorted(c.items(),key=str): print(' ',k,v)
