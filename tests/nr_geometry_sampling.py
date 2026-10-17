"""CPU coverage regression for the production fitter's fixed 32-hypothesis budget."""
from pathlib import Path
import itertools,json,re

header=(Path(__file__).resolve().parents[1]/'OptiScaler/motion/OpticalFlow_Hlsl.h').read_text(encoding='utf-8-sig')
match=re.search(r'static const uint patterns\[32\]\s*=\s*\{([^}]+)\}',header)
assert match,'Production hypothesis table missing'
packed=[int(x) for x in re.findall(r'(\d+)u',match[1])]
assert len(packed)==32
def decode(value):
    result=[]
    for q in range(4):
        local=(value>>(q*2))&3
        result.append(((q>>1)*2+(local>>1))*4+(q&1)*2+(local&1))
    return tuple(result)
new=[decode(x) for x in packed]
old=[]
for h in range(32):
    value=0
    for q in range(4):
        seed=(h*747796405+q*2891336453+277803737)&0xffffffff
        value|=((seed>>16)&3)<<(q*2)
    old.append(decode(value))
assert len(set(old))==15
assert len(set(new))==32
assert new[:15]==list(dict.fromkeys(old)),'Original distinct hypotheses/tie order must survive'
assert set(old)<=set(new)
result=[]
for n in [6,8,10]:
    supports=[set(t) for t in itertools.combinations(range(16),n) if len({i%4//2+(i//4//2)*2 for i in t})==4]
    before=sum(not any(set(r)<=s for r in old) for s in supports)
    after=sum(not any(set(r)<=s for r in new) for s in supports)
    assert after<before,(n,before,after)
    assert all(not any(set(r)<=s for r in old) or any(set(r)<=s for r in new) for s in supports)
    result.append({'usable':n,'arrangements':len(supports),'missedBefore':before,'missedAfter':after})
print(json.dumps({'distinctBefore':15,'distinctAfter':32,'oldCoveragePreserved':True,'coverage':result,'result':'PASS'},indent=2))
