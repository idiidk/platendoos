import subprocess,time,sys,os,re,json
D='/tmp/rbm-uimon'
def pid(): return subprocess.check_output(['pgrep','-x','rekordbox']).split()[0].decode()
def send(lines):
    p=D+'/cmd.tmp'; open(p,'w').write('\n'.join(lines)+'\n'); os.rename(p,D+'/cmd.txt')
def stalls(t0,t1):
    r=[]
    for l in open(D+'/stalls.txt'):
        m=re.match(r'(\d+\.\d+) STALL ([\d.]+) ms',l)
        if m and t0<=float(m.group(1))<=t1: r.append((float(m.group(1))-t0,float(m.group(2))))
    return r
def logs(t0,t1):
    r=[];seen=set()
    for l in open(D+'/stalls.txt'):
        if l in seen: continue
        seen.add(l)
        m=re.match(r'(\d+\.\d+) (CLICK|KEY|MENU|MARK|no |menu|BUSY|REPAINT|SUM|CLOSEWIN).*',l)
        if m and t0<=float(m.group(1))<=t1: r.append(l.strip())
    return r
def run(label,lines,dur=3.0,sample=False):
    """send cmd lines (sleep n supported), wait dur, return stalls"""
    sf=None
    if sample:
        sf=f'{D}/samples/{label}.sample.txt'; os.makedirs(D+'/samples',exist_ok=True)
        sp=subprocess.Popen(['sample',pid(),str(int(dur+0.5)),'1','-file',sf],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); time.sleep(1.0)
    t0=time.time(); send(lines); time.sleep(dur)
    t1=time.time()
    if sample: sp.wait()
    st=stalls(t0,t1)
    open(D+'/runs.jsonl','a').write(json.dumps({'label':label,'t0':t0,'stalls':st,'sample':sf,'dur':dur,'cmds':lines})+'\n')
    return t0,st,logs(t0,t1)
if __name__=='__main__':
    label=sys.argv[1]; dur=float(sys.argv[2]); lines=sys.argv[3].split(';')
    t0,s,lg=run(label,lines,dur,len(sys.argv)>4 and sys.argv[4]=='s')
    print(label,'stalls(offset s, ms):',s); print('\n'.join(lg))
