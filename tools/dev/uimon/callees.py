import incl,collections,sys
ts=incl.threads(sys.argv[1])
t=[x for x in ts if 'main-thread' in x['name'] or 'Message Thread' in x['name']][0]
nodes=incl.parse(t)
for target in sys.argv[2:]:
    by=collections.Counter()
    for nd in nodes:
        p=nd['path']
        ks=[k for k,x in enumerate(p[:-1]) if x.startswith(target)]
        if ks and len(p)-1-ks[-1] in (1,2,3):
            by[(len(p)-1-ks[-1], nd['fn'][:90])]+=nd['n']
    print('==',target)
    for d in (1,2,3):
        for (dd,k),v in sorted(by.items(),key=lambda x:-x[1]):
            if dd==d and v>=12: print('  '*d,v,k)
