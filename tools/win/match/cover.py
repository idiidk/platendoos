import sys,pickle
mac=pickle.load(open(sys.argv[1],'rb')); m=pickle.load(open(sys.argv[2],'rb'))
byname={}
for a,f in mac['funcs'].items():
    for n in f['names']: byname[n]=a
dn={}
for a,ns in mac['datanames'].items():
    for n in ns: dn[n]=a
need=[l.strip() for l in open(sys.argv[3]) if l.strip()]
ok=miss=data=absent=0
miss_l=[]
for n in need:
    if n in byname:
        if byname[n] in m['match']: ok+=1
        else: miss+=1; miss_l.append(n)
    elif n in dn: data+=1
    else: absent+=1; print('ABSENT-x86', n)
print('matched',ok,'unmatched',miss,'data',data,'absent',absent)
open(sys.argv[4],'w').write('\n'.join(miss_l))
