"""Bounded numeric passive trace decoder. No native qualification is inferred.

Malformed, incomplete and uncovered traces all have an INCONCLUSIVE verdict;
format validity is separate from internal-path coverage. Never output payloads.
"""
import argparse
import json
from pathlib import Path
import struct

HEADER = struct.Struct('<8sIIIIQQIIQQ')
RECORD = struct.Struct('<IHHQQIHHQQQQ8Q')
MAX_CAPACITY = 16384
MAX_BYTES = HEADER.size + MAX_CAPACITY * RECORD.size


class FormatError(ValueError):
    pass


def decode(blob):
    if len(blob) < HEADER.size or len(blob) > MAX_BYTES:
        raise FormatError('length')
    (magic, version, hs, rs, capacity, reserved, overflow, initialized,
     finalized, generation, flags) = HEADER.unpack_from(blob)
    if magic != b'STPTH001' or version != 1 or hs != 64 or rs != 128:
        raise FormatError('schema')
    if not 1 <= capacity <= MAX_CAPACITY or reserved > capacity:
        raise FormatError('capacity')
    if len(blob) != hs + capacity * rs:
        raise FormatError('length')
    if initialized not in (0, 1) or finalized not in (0, 1) or not generation or flags:
        raise FormatError('header')
    records = []
    incomplete = not initialized or not finalized or bool(overflow)
    for slot in range(capacity):
        offset = hs + slot * rs
        raw = blob[offset:offset+rs]
        if slot >= reserved:
            if any(raw): raise FormatError('unused-slot')
            continue
        values = RECORD.unpack(raw)
        committed = values[0]
        if committed not in (0, 1): raise FormatError('publication')
        if not committed:
            incomplete = True
            continue  # Fields may be partially written: never interpret them.
        (_, rv, event, sequence, process, thread, role, rf,
         read, peek, op, parent, *payload) = values
        if rv != 1 or not 1 <= event <= 20 or sequence != slot:
            raise FormatError('record-schema')
        if process != generation or not thread or role not in (1, 2, 3, 4) or rf & ~3:
            raise FormatError('identity')
        if rf != 1 or event == 20: incomplete = True
        records.append(dict(event=event, sequence=sequence, process=process,
                            thread=thread, role=role, flags=rf, read=read,
                            peek=peek, op=op, parent=parent, payload=tuple(payload)))
    return dict(records=records, incomplete=bool(incomplete), overflow=overflow,
                generation=generation, reserved=reserved)


def _coverage(trace, case, variant):
    """Conservative v1 predicates; PATH_OBSERVED is not byte/native qualification.

    Numeric payload lanes follow the emitter contract in PASSIVE-PATH-PLAN and
    the paired synthetic tests. No absence-of-events claim is made on overflow.
    """
    rows = trace['records']
    if trace['incomplete'] or case not in ('partial-zero', 'positive-prefix') or variant not in ('stock', 'refined'):
        return False
    # A later unreturned read permits committed observations, not a complete
    # path verdict. Finalized mapping only proves writers stopped.
    read_boundaries = {}
    for r in rows:
        if r['event'] in (1, 15): read_boundaries.setdefault(r['read'], []).append(r['event'])
    if any(not read or events != [1, 15] for read, events in read_boundaries.items()):
        return False
    # Operation IDs cannot silently alias unrelated operation families.
    families = {7:7, 8:7, 9:7, 10:10, 11:10, 12:12, 13:12, 18:12, 19:12}
    operations = {}
    for r in rows:
        if r['op']:
            family = families.get(r['event'], r['event'])
            previous = operations.setdefault(r['op'], family)
            if previous != family: return False
    # Role identity must be stable, except a handler may interrupt a reader.
    thread_roles = {}
    for r in rows:
        thread_roles.setdefault(r['thread'], set()).add(r['role'])
    if any(roles not in ({1}, {2}, {3}, {4}, {1,3}) for roles in thread_roles.values()):
        return False
    # Observe a real distinct master and successful queue read/write operations.
    masters = [r for r in rows if r['event']==16 and r['role']==2]
    if not all(any(r['payload'][0]==direction and r['payload'][2]==1 and
                       0 < r['payload'][3] <= r['payload'][1] for r in masters)
               for direction in (1,2)):
        return False
    if any(r['payload'][2]!=1 or r['payload'][3]>r['payload'][1] for r in masters):
        return False
    # Every emitted lock operation must have one complete successful pair.
    locks = {}
    for r in rows:
        if r['event'] in (10,11): locks.setdefault(r['op'], []).append(r)
    for op, pair in locks.items():
        if not op or len(pair)!=2 or [r['event'] for r in pair] != [10,11]: return False
        a,b=pair; pa,pb=a['payload'],b['payload']
        if a['thread']!=b['thread'] or a['read']!=b['read'] or a['peek']!=b['peek']: return False
        if pa[:2]!=pb[:2] or pb[0] not in (1,2) or pb[1] not in (1,2): return False
        if pb[2] != (0 if pb[1]==1 else 1): return False
    for begin in rows:
        if begin['event']!=1 or begin['role']!=1 or not begin['read']: continue
        read = begin['read']; thread = begin['thread']; requested = 2 if case=='partial-zero' else 64
        if begin['payload'][:6] != (requested,0,2,0,0,0): continue
        rr=[r for r in rows if r['read']==read]
        if sum(r['event']==1 for r in rr)!=1: continue
        if any(r['role']==1 and r['thread']!=thread for r in rr): continue
        if any(r['role']==2 for r in rr): continue
        ends=[r for r in rr if r['event']==15]
        if len(ends)!=1: continue
        end=ends[0]; expected=2
        if end['payload'][:3]!=(expected,expected,0): continue
        if any(r['sequence']<begin['sequence'] or r['sequence']>end['sequence'] for r in rr): continue
        copies=[r for r in rr if r['event']==2]
        if not copies or copies[0]['payload'][:3]!=(0,1,1): continue
        copied=0; good=True
        for r in copies:
            prev,delta,after,_,_,remaining,_,_=r['payload']
            if prev!=copied or after!=prev+delta or after>requested or remaining!=requested-after:
                good=False; break
            copied=after
        if not good or copied!=expected: continue
        more=[r for r in rr if r['event']==3 and r['payload'][:3]==(1,requested,2)
              and r['sequence']>copies[0]['sequence']]
        if not more: continue
        for dispatch in rr:
            if dispatch['event']!=5 or not dispatch['peek']: continue
            event,index,total,stat,mouse,ra,dc,_=dispatch['payload']
            expected_index=0 if case=='partial-zero' else 1
            if (event,index,stat,mouse,dc)!=(2,expected_index,expected_index,0,1): continue
            if total<=index or not (more[0]['sequence']<dispatch['sequence']<end['sequence']): continue
            pp=[r for r in rr if r['peek']==dispatch['peek']]
            peeks=[r for r in pp if r['event']==4]
            geometry=[r for r in pp if r['event']==6]
            returns=[r for r in pp if r['event']==14]
            sb=[r for r in pp if r['event']==12]
            se=[r for r in pp if r['event']==13]
            if any(len(x)!=1 for x in (peeks,geometry,returns,sb,se)): continue
            peek,geo,ret,sig,send=peeks[0],geometry[0],returns[0],sb[0],se[0]
            if peek['payload'][0]!=1 or peek['payload'][2:5]!=(total,requested-1,1): continue
            g=geo['payload']
            if g[:2]!=(event,index) or g[6:]!=(1,1) or g[2:4]==g[4:6]: continue
            if not all(0<x<=32767 for x in g[2:6]): continue
            if copies[0]['sequence'] >= peek['sequence'] or copies[-1]['sequence'] <= ret['sequence']: continue
            if any(c['sequence'] > copies[0]['sequence'] and c['sequence'] < ret['sequence'] for c in copies): continue
            if not (peek['sequence']<dispatch['sequence']<geo['sequence']<sig['sequence']<send['sequence']<ret['sequence']): continue
            if not sig['op'] or send['op']!=sig['op'] or sig['payload'][:3]!=send['payload'][:3]: continue
            if sig['payload'][0]==0 or sig['payload'][1:3]!=(event,index): continue
            # A sole request globally excludes ambiguous signal attribution.
            if sum(r['event']==12 for r in rows)!=1: continue
            handlers=[r for r in rows if r['event'] in (18,19)]
            if len(handlers)!=2 or [r['event'] for r in handlers]!=[18,19]: continue
            h1,h2=handlers
            if any(h['op']!=sig['op'] or h['role']!=3 or h['payload'][0]!=sig['payload'][0]
                   or h['read']!=read or h['peek']!=dispatch['peek'] for h in handlers): continue
            if h1['thread']!=thread or h2['thread']!=thread or h1['payload'][:2]!=h2['payload'][:2] or not h1['payload'][1]: continue
            if not(sig['sequence']<h1['sequence']<h2['sequence']<send['sequence']): continue
            releases=[pair for pair in locks.values() if pair[1]['read']==read and
                      pair[1]['peek']==dispatch['peek'] and pair[1]['payload'][:2]==(1,2) and
                      geo['sequence']<pair[0]['sequence']<pair[1]['sequence']<sig['sequence']]
            acquires=[pair for pair in locks.values() if pair[1]['read']==read and
                      pair[1]['peek']==dispatch['peek'] and pair[1]['payload'][:2]==(1,1) and
                      send['sequence']<pair[0]['sequence']<pair[1]['sequence']<ret['sequence']]
            if len(releases)!=1 or len(acquires)!=1: continue
            # No alternate geometry discovery is allowed in the read interval.
            if any(r['event']==6 and r is not geo and r['payload'][6] or
                   r['event']==17 and r['payload'][0]==4 for r in rows): continue
            retirement=[r for r in pp if r['event'] in (7,8,9)]
            if variant=='refined':
                if ret['payload'][0]!=stat or ret['payload'][4]!=1: continue
                if case=='partial-zero':
                    fresh=[r for r in rr if r['event']==4 and r['peek']!=dispatch['peek'] and
                           ret['sequence']<r['sequence']<end['sequence'] and r['payload'][0]==1]
                    if not fresh or retirement: continue
                else:
                    # copied1 + ready b reaches VMIN2: first read returns2.
                    # Tail c must be observed in a later distinct read, not an
                    # impossible third-byte copy in the first read.
                    tails=[r for r in rows if r['event']==1 and r['role']==1 and
                           r['thread']==thread and r['read'] not in (0,read) and
                           r['sequence']>end['sequence'] and r['payload'][:5]==(64,0,1,0,0)]
                    tail_ok=False
                    for tail in tails:
                        tr=[r for r in rows if r['read']==tail['read']]
                        tp=[r for r in tr if r['event']==4]
                        tc=[r for r in tr if r['event']==2]
                        te=[r for r in tr if r['event']==15]
                        if sum(r['event']==1 for r in tr)!=1 or any(r['role']!=1 or r['thread']!=thread for r in tr): continue
                        if len(tp)!=1 or len(tc)!=1 or len(te)!=1: continue
                        fresh,copy,last=tp[0],tc[0],te[0]
                        if fresh['peek'] in (0,dispatch['peek']) or fresh['payload'][0]!=1 or fresh['payload'][3:5]!=(64,1): continue
                        if copy['payload'][:3]!=(0,1,1) or copy['payload'][5]!=63 or last['payload'][:3]!=(1,1,0): continue
                        if not(tail['sequence']<fresh['sequence']<copy['sequence']<last['sequence']): continue
                        if any(r['sequence']<tail['sequence'] or r['sequence']>last['sequence'] for r in tr): continue
                        tail_ok=True
                    if not tail_ok: continue
                    if len(retirement)!=3 or [r['event'] for r in retirement]!=[7,8,9]: continue
                    rb,api,re=retirement
                    if not rb['op'] or any(r['op']!=rb['op'] for r in retirement): continue
                    # Reason0 means unspecified; exact successful operation placement
                    # before release supplies the evidence, not a guessed label.
                    if rb['payload'][:2]!=(index,index) or rb['payload'][4] not in (0,2): continue
                    if api['payload'][:3]!=(index,1,index) or api['payload'][5]!=0: continue
                    if re['payload'][0:2]!=(index,0): continue
                    before,after=re['payload'][2:4]
                    if before!=rb['payload'][3] or after!=before-min(before,index): continue
                    if not(geo['sequence']<rb['sequence']<api['sequence']<re['sequence']<releases[0][0]['sequence']): continue
            else:
                # v1 only qualifies stock's zero-prefix partial-copy trace.
                # Positive-prefix stock retirement differs and needs its own tests.
                if case!='partial-zero' or ret['payload'][0]!=1 or ret['payload'][4]!=1: continue
                if len(retirement)!=3 or [r['event'] for r in retirement]!=[7,8,9]: continue
                rb,api,re=retirement
                if not rb['op'] or any(r['op']!=rb['op'] for r in retirement): continue
                if rb['payload'][:2]!=(total,total) or rb['payload'][4] not in (0,1): continue
                if api['payload'][:3]!=(total,1,total) or api['payload'][5]!=0: continue
                if re['payload'][:2]!=(total,0): continue
                before,after=re['payload'][2:4]
                if before!=rb['payload'][3] or after!=before-min(before,total): continue
                if not(acquires[0][1]['sequence']<rb['sequence']<api['sequence']<re['sequence']<ret['sequence']): continue
            return True
    return False


def interrupted_partial_reads(trace):
    """Committed numeric facts only, never dispatch attribution/qualification.

    An unrelated later unreturned read cannot erase an earlier complete fact.
    Each counted read itself must have a unique same-thread complete chain.
    """
    empty = dict(count=0, max_copied=0)
    if trace['incomplete']: return empty
    relevant = [r for r in trace['records'] if r['event'] in (1, 2, 15)]
    groups = {}
    for r in relevant: groups.setdefault(r['read'], []).append(r)
    observed = []
    for read, group in groups.items():
        if not read or any(r['role'] != 1 for r in group): continue
        starts = [r for r in group if r['event'] == 1]
        ends = [r for r in group if r['event'] == 15]
        if len(starts) != 1 or len(ends) != 1: continue
        begin, end = starts[0], ends[0]
        if group[0] is not begin or group[-1] is not end: continue
        if any(r['thread'] != begin['thread'] for r in group): continue
        requested = begin['payload'][0]
        if not requested or begin['payload'][5] != 0: continue
        copied = 0
        valid = True
        for r in group[1:-1]:
            previous, delta, after, _, _, remaining, _, _ = r['payload']
            if (r['event'] != 2 or previous != copied or after != previous + delta or
                    after > requested or remaining != requested - after):
                valid = False
                break
            copied = after
        if not valid or end['payload'][1] != copied: continue
        if end['payload'][0] == (1 << 64) - 1 and end['payload'][2] == 4 and copied > 0:
            observed.append(copied)
    return dict(count=len(observed), max_copied=max(observed, default=0))


def analyze(blob, case='partial-zero', variant='refined'):
    try:
        trace = decode(blob)
    except FormatError as error:
        return dict(verdict='INCONCLUSIVE', format='INVALID', reason=str(error),
                    recorded_observations=dict(interrupted_partial_reads=dict(count=0, max_copied=0)))
    observed = _coverage(trace, case, variant)
    return dict(verdict='PATH_OBSERVED' if observed else 'INCONCLUSIVE', format='VALID',
                reason='predicate-complete' if observed else ('incomplete' if trace['incomplete'] else 'uncovered'),
                records=len(trace['records']), case=case, variant=variant,
                native_qualification=False, byte_correctness_proven=False,
                recorded_observations=dict(interrupted_partial_reads=interrupted_partial_reads(trace)))


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('trace', type=Path)
    args.add_argument('--case', choices=('partial-zero', 'positive-prefix', 'resize'), default='partial-zero')
    args.add_argument('--variant', choices=('stock', 'refined'), default='refined')
    ns = args.parse_args()
    try:
        with ns.trace.open('rb') as stream: blob = stream.read(MAX_BYTES+1)
        result = analyze(blob, ns.case, ns.variant)
    except OSError:
        result = dict(verdict='INCONCLUSIVE', format='INVALID', reason='file-read')
    print(json.dumps(result, sort_keys=True))
    return 0 if result['verdict'] == 'PATH_OBSERVED' else 2


if __name__ == '__main__': raise SystemExit(main())
