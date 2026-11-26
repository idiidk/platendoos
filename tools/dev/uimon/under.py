# under.py <sample> <function substring> [filter words...]: inclusive and self ticks of everything below the outermost
# frame of that function on the main thread
import re,collections,sys
lines=open(sys.argv[1],errors='replace').read().split('\n')
start=next(i for i,l in enumerate(lines) if sys.argv[2] in l)
m=re.match(r'^([ +!:|]*)(\d+) (.*)$',lines[start]); d0=len(m.group(1))
rows=[]
for l in lines[start:]:
    m=re.match(r'^([ +!:|]*)(\d+) (.*)$',l)
    if not m: break
    d=len(m.group(1))
    if rows and d<=d0: break
    rows.append((d,int(m.group(2)),re.split(r'  \(in ',m.group(3))[0].strip()))
incl=collections.Counter(); selfc=collections.Counter(); stack=[]
for i,(d,n,f) in enumerate(rows):
    while stack and stack[-1][0]>=d: stack.pop()
    if f not in [s[1] for s in stack]: incl[f]+=n
    stack.append((d,f))
    j=i+1; ch=[]
    while j<len(rows) and rows[j][0]>d: ch.append(rows[j]); j+=1
    kids=sum(c[1] for c in ch if c[0]==min(x[0] for x in ch)) if ch else 0
    if n-kids>0: selfc[f]+=n-kids
tot=rows[0][1]; print('total under it:',tot,'ticks')
words=sys.argv[3:]
print('-- inclusive')
k=0
for f,n in incl.most_common(600):
    if not words or any(w in f for w in words):
        print(f'{n:5d} {100*n/tot:5.1f}%  {f[:135]}'); k+=1
        if k>=44: break
print('-- self')
for f,n in selfc.most_common(12): print(f'{n:5d} {100*n/tot:5.1f}%  {f[:120]}')
