import incl,collections,sys
ts=incl.threads(sys.argv[1])
t=[x for x in ts if 'main-thread' in x['name'] or 'Message Thread' in x['name']][0]
nodes=incl.parse(t)
def callers(prefix,stop,depth=3):
    by=collections.Counter()
    for nd in nodes:
        if nd['fn'].startswith(prefix):
            p=nd['path'][:-1]
            js=[k for k,x in enumerate(p) if x.startswith(stop)] if stop else [len(p)]
            if not js: continue
            j=js[-1]
            by[' < '.join(x[:50] for x in p[max(j-depth,0):j][::-1])]+=nd['n']
    return by
for a in sys.argv[2:]:
    pre,_,stop=a.partition('@')
    print('==',pre,'(callers above',stop or 'it',')')
    for k,v in callers(pre,stop).most_common(7): print('  ',v,k)
