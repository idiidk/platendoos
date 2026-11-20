import sys,pickle,re,glob,os,collections
mac=pickle.load(open('mac.pkl','rb')); r=pickle.load(open(sys.argv[1],'rb'))
M=mac['funcs']; byname={n:a for a,f in M.items() for n in f['names']}
dn={n for ns in mac['datanames'].values() for n in ns}
repo=os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
hooks={}
sys.path.insert(0,repo+'/tools/build'); import hooksdef
for l in hooksdef.lines(repo):
    m=re.match(r'HOOK\((\w+), "([^"]+)", (\d+), (\d+), "([^"]+)", (-?\d+)\)',l)
    if m: hooks[m.group(1)]=(m.group(2),m.group(5),int(m.group(6)))
rows=[]
for f in sorted(glob.glob(repo+'/mods/*.mm')+glob.glob(repo+'/core/*.mm')):
    src=open(f).read()
    syms=set(re.findall(r'"(__Z[^"]+|_[a-z_][^"]*)"',src))
    hs=set(re.findall(r'rbm_hook_(\w+)',src))&set(hooks)
    need=set(s for s in syms if s in byname or s in dn)
    for h in hs: need|={hooks[h][0],hooks[h][1]}
    ok=sum(1 for s in need if s in byname and byname[s] in r['match'])
    data=sum(1 for s in need if s in dn and s not in byname)
    rows.append((os.path.basename(f),len(need),ok,data,len(hs)))
print('%-24s %5s %5s %5s %5s'%('file','need','found','data','hooks'))
for x in rows:
    if x[1]: print('%-24s %5d %5d %5d %5d'%x)
