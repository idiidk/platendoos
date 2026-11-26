import meas,time,re,random,sys
def latency(n=12,cls='CfxOnOffButton',label='lat'):
    meas.send(['sum pre']); time.sleep(0.5)
    ts=[]
    for i in range(n):
        meas.send([f'repaint {cls}']); time.sleep(0.35+random.random()*0.15)
    time.sleep(0.5); meas.send([f'sum {label}']); time.sleep(0.6)
    reps=[float(re.match(r'([\d.]+) REPAINT',l).group(1)) for l in open(meas.D+'/stalls.txt') if ' REPAINT ' in l][-n:]
    reps=[]
    for l in open(meas.D+'/stalls.txt'):
        m=re.search(r'REPAINT \S+ media=([\d.]+)',l)
        if m: reps.append(float(m.group(1)))
    reps=reps[-n:]
    fr=[tuple(map(float,l.split()[:3])) for l in open(meas.D+'/frames.txt')]
    out=[]
    for r in reps:
        nxt=[f for f in fr if f[0]>r]
        if nxt:
            f=nxt[0]; out.append(((f[0]-r)*1000,(f[2]-r)*1000 if f[2] else float('nan')))
    return out
if __name__=='__main__':
    o=latency(int(sys.argv[1]) if len(sys.argv)>1 else 12)
    print('repaint -> next frame start ms / presented ms:',[(round(a,1),round(b,1)) for a,b in o])
    a=sorted(x[0] for x in o); b=sorted(x[1] for x in o if x[1]==x[1])
    print('frame start: min %.1f med %.1f max %.1f'%(a[0],a[len(a)//2],a[-1]))
    if b: print('presented: min %.1f med %.1f max %.1f'%(b[0],b[len(b)//2],b[-1]))
