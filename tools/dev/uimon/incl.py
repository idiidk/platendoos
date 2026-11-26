import re,sys
IDLE=('mach_msg2_trap','mach_msg_trap','__psynch_cvwait','__workq_kernreturn','kevent','semaphore_wait_trap','__select','__psynch_mutexwait','__ulock_wait','__ulock_wait2','read','poll','__semwait_signal','semaphore_timedwait_trap','kevent64','kevent_id','nanosleep','usleep','swtch_pri','_pthread_cond_wait','__sigwait','__recvfrom','__accept','__sigsuspend_nocancel')
def threads(path):
    cur=None;out=[]
    for l in open(path,errors='replace'):
        l=l.rstrip('\n')
        m=re.match(r'^    (\d+) (Thread_\S+)(.*)$',l)
        if m: cur={'n':int(m.group(1)),'name':(m.group(2)+m.group(3)).strip(),'rows':[]};out.append(cur);continue
        if l.startswith('Total number in stack') or l.startswith('Sort by top'): cur=None
        if cur is not None and l.strip(): cur['rows'].append(l)
    return out
def parse(t):
    nodes=[];stack=[]
    for l in t['rows']:
        m=re.match(r'^([ +!:|]*)(\d+) (.*)$',l)
        if not m: continue
        d=len(m.group(1)); n=int(m.group(2)); f=m.group(3)
        fn=re.split(r'  \(in ',f)[0].strip(); lib=re.search(r'\(in ([^)]*)\)',f); lib=lib.group(1) if lib else ''
        while stack and stack[-1][0]>=d: stack.pop()
        node={'d':d,'n':n,'fn':fn,'lib':lib,'kids':0,'path':[x[2] for x in stack]+[fn]}
        if stack: stack[-1][3]['kids']+=n
        stack.append((d,n,fn,node)); nodes.append(node)
    return nodes
def analyse(path,match='main-thread'):
    ts=threads(path)
    t=[x for x in ts if (match=='main' and ('main-thread' in x['name'] or 'Message Thread' in x['name'])) or match in x['name']][0]
    nodes=parse(t)
    busy=0;blocked={};incl={};libs={};idle=0
    for nd in nodes:
        selfn=nd['n']-nd['kids']
        if selfn>0:
            iswait=nd['fn'].split(' ')[0] in IDLE or nd['fn'] in IDLE
            inside=any('CFRUNLOOP_IS_CALLING_OUT' in p for p in nd['path'])
            if iswait and not inside: idle+=selfn
            else:
                busy+=selfn
                if iswait: blocked[nd['fn']]=blocked.get(nd['fn'],0)+selfn
        if nd['fn'] not in nd['path'][:-1]:
            incl[nd['fn']]=incl.get(nd['fn'],0)+nd['n']; libs[nd['fn']]=nd['lib']
    analyse.blocked=blocked
    return t['n'],busy,idle,incl,libs
if __name__=='__main__':
    path=sys.argv[1]; match=sys.argv[2] if len(sys.argv)>2 else 'main'; top=int(sys.argv[3]) if len(sys.argv)>3 else 40
    tot,busy,idle,incl,libs=analyse(path,match)
    print(f'{match}: total samples {tot}, busy(non-wait leaf) {busy} ({busy*1.7:.0f} ms at 1.7ms/tick; {busy} ms at nominal 1ms), idle {idle}')
    print('  of which blocked in a wait call inside a callout:',sum(analyse.blocked.values()),dict(sorted(analyse.blocked.items(),key=lambda x:-x[1])[:3]))
    skip=('start','juce::JUCEApplicationBase::main','-[NSApplication run]','-[NSApplication(NSEventRouting)','_NSApplicationMain')
    k=0
    for fn,n in sorted(incl.items(),key=lambda x:-x[1]):
        if libs[fn] not in ('rekordbox','librbmods.dylib') and '--all' not in sys.argv: continue
        if fn.startswith(skip) or 'nextEventMatching' in fn: continue
        print(f'{n:6d} {100*n/max(busy,1):5.1f}%  {fn[:150]}  [{libs[fn]}]'); k+=1
        if k>=top: break
