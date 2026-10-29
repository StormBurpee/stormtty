"""Strict local resize/read observation; never mouse/master or byte qualification."""
import importlib.util
from pathlib import Path
P=Path(__file__).resolve().parents[2]/'passive-path/parser.py'
spec=importlib.util.spec_from_file_location('partial_resize_decoder',P)
parser=importlib.util.module_from_spec(spec);spec.loader.exec_module(parser)


def observe(trace,case):
    """Numeric observations independent of variant identity or expected outcome."""
    if trace['incomplete'] or case not in ('partial-zero','positive-prefix','zero-copy'):return []
    rows=trace['records']; expected=0 if case=='zero-copy' else 1
    requested=64 if case=='positive-prefix' else 2
    signals=[r for r in rows if r['event']==12]
    handlers=[r for r in rows if r['event'] in (18,19)]
    if len(signals)!=1 or sum(r['event']==13 for r in rows)!=1 or [r['event'] for r in handlers]!=[18,19]:return []
    # Stable roles and operation families forbid ambiguous reused identities.
    roles={};ops={};families={7:7,8:7,9:7,10:10,11:10,12:12,13:12,18:12,19:12}
    for r in rows:
        roles.setdefault(r['thread'],set()).add(r['role'])
        if r['op']:
            family=families.get(r['event'],r['event'])
            if ops.setdefault(r['op'],family)!=family:return []
    if any(v not in ({1},{2},{3},{4},{1,3}) for v in roles.values()):return []
    observed=[]
    for begin in rows:
        if begin['event']!=1 or begin['role']!=1 or not begin['read']:continue
        if begin['payload'][:6]!=(requested,0,2,0,0,0):continue
        rr=[r for r in rows if r['read']==begin['read']]
        ends=[r for r in rr if r['event']==15]
        if len(ends)!=1 or sum(r['event']==1 for r in rr)!=1:continue
        end=ends[0]
        if any(r['thread']!=begin['thread'] or r['role'] not in (1,3) or not begin['sequence']<=r['sequence']<=end['sequence'] for r in rr):continue
        if any(r['role']!=(3 if r['event'] in (18,19) else 1) for r in rr):continue
        copies=[r for r in rr if r['event']==2]
        if expected:
            if len(copies)!=1 or copies[0]['role']!=1 or copies[0]['payload'][:3]!=(0,1,1) or copies[0]['payload'][5]!=requested-1:continue
        elif copies or any(r['event']==3 for r in rr):continue
        if end['payload'][1]!=expected:continue
        dispatches=[r for r in rr if r['event']==5 and r['payload'][0]==4]
        if len(dispatches)!=1:continue
        d=dispatches[0];event,index,total,stat,mouse,ra,copied,_=d['payload']
        if not d['peek'] or (index,stat,mouse,copied)!=(0,0,0,expected) or total<1:continue
        pp=[r for r in rr if r['peek']==d['peek']]
        if sum(r['event']==5 for r in pp)!=1:continue
        def one(event):
            matches=[r for r in pp if r['event']==event]
            return matches[0] if len(matches)==1 else None
        peek,geo,ret,sig,send=[one(e) for e in (4,6,14,12,13)]
        if any(r is None for r in (peek,geo,ret,sig,send)):continue
        if any(r['role']!=1 for r in (peek,d,geo,ret,sig,send)):continue
        if peek['payload'][0]!=1 or peek['payload'][2:5]!=(total,requested-expected,1):continue
        if geo['payload']!=(4,0,110,30,109,30,1,1):continue
        if any(r['event']==6 and r is not geo and r['payload'][6] for r in rows):continue
        if ret['payload'][0]!=3 or ret['payload'][4]!=expected:continue # input_winch
        if sig is not signals[0] or not sig['op'] or send['op']!=sig['op'] or sig['payload'][:3]!=(28,4,0) or send['payload'][:3]!=(28,4,0):continue
        h1,h2=handlers
        if any(h['op']!=sig['op'] or h['role']!=3 or h['thread']!=begin['thread'] or h['read']!=begin['read'] or h['peek']!=d['peek'] or h['payload'][0]!=28 for h in handlers):continue
        if h1['payload'][:2]!=h2['payload'][:2] or h1['payload'][1]!=1:continue
        chain=[begin,peek,d,geo,sig,h1,h2,send,ret,end]
        if any(a['sequence']>=b['sequence'] for a,b in zip(chain,chain[1:])):continue
        if expected:
            more=[r for r in rr if r['event']==3 and r['payload'][:3]==(1,requested,2) and copies[0]['sequence']<r['sequence']<peek['sequence']]
            if len(more)!=1 or not begin['sequence']<copies[0]['sequence']<peek['sequence']:continue
        # Require successful matched input-mutex release/reacquire around signal.
        locks={};good=True
        for r in rr:
            if r['event'] in (10,11):locks.setdefault(r['op'],[]).append(r)
        for op,pair in locks.items():
            if not op or [r['event'] for r in pair]!=[10,11]:good=False;break
            a,b=pair
            if a['peek']!=b['peek'] or a['payload'][:2]!=b['payload'][:2] or b['payload'][0] not in (1,2) or b['payload'][1] not in (1,2) or b['payload'][2]!=(0 if b['payload'][1]==1 else 1):good=False;break
        if not good:continue
        releases=[p for p in locks.values() if p[0]['peek']==d['peek'] and p[1]['payload'][:2]==(1,2) and geo['sequence']<p[0]['sequence']<p[1]['sequence']<sig['sequence']]
        acquires=[p for p in locks.values() if p[0]['peek']==d['peek'] and p[1]['payload'][:2]==(1,1) and send['sequence']<p[0]['sequence']<p[1]['sequence']<ret['sequence']]
        if len(releases)!=1 or len(acquires)!=1:continue
        result,_,error=end['payload'][:3]
        outcome='positive-count' if expected and result==expected else ('eintr' if result==(1<<64)-1 and error==4 else None)
        if outcome is not None:observed.append(dict(read=begin['read'],copied=expected,outcome=outcome,return_count=expected if outcome=='positive-count' else -1,errno=error))
    return observed


def analyze(blob,case,variant):
    result=dict(verdict='INCONCLUSIVE',format='VALID',case=case,variant=variant,expected_return_match=False,native_qualification=False,byte_correctness_proven=False,mouse_master_path_proven=False)
    try:trace=parser.decode(blob)
    except parser.FormatError as error:return dict(result,format='INVALID',reason=str(error))
    if case not in ('partial-zero','positive-prefix','zero-copy') or variant not in ('stock','copied'):return dict(result,reason='unsupported-case-or-variant')
    observations=observe(trace,case)
    if len(observations)!=1:return dict(result,reason='incomplete' if trace['incomplete'] else 'uncovered-or-ambiguous')
    observation=observations[0]
    expected='positive-count' if variant=='copied' and case!='zero-copy' else 'eintr'
    return dict(result,verdict='RESIZE_OBSERVED',reason='complete-local-chain',observation=observation,expected_return_match=observation['outcome']==expected)
