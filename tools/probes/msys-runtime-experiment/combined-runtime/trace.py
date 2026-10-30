"""Strict v2 observation decoder, NOT the future native matrix adjudicator.

Returns observations only. Format validity or paired events never implies byte,
path, production or native qualification. Incomplete/missing coverage stays so.
"""
import argparse
import json
from pathlib import Path
import struct

HEADER = struct.Struct('<8sIIIIQQIIQQ')
RECORD = struct.Struct('<IHHQQIHHQQQQ8Q')
CAPACITY = 16384
MAX_BYTES = HEADER.size+CAPACITY*RECORD.size


class TraceError(ValueError):
    pass


def decode(blob):
    if not HEADER.size <= len(blob) <= MAX_BYTES:
        raise TraceError('length')
    magic, version, hs, rs, cap, used, overflow, initialized, finalized, generation, flags = HEADER.unpack_from(blob)
    if (magic, version, hs, rs) != (b'STCMB002', 2, 64, 128):
        raise TraceError('schema')
    if not 1 <= cap <= CAPACITY or used > cap or len(blob) != hs+cap*rs:
        raise TraceError('capacity/length')
    if initialized not in (0, 1) or finalized not in (0, 1) or flags or not generation:
        raise TraceError('header')
    incomplete = not initialized or not finalized or bool(overflow)
    rows = []
    for slot in range(cap):
        raw = blob[hs+slot*rs:hs+(slot+1)*rs]
        if slot >= used:
            if any(raw):
                raise TraceError('unused slot')
            continue
        committed, rv, event, sequence, process, thread, role, rf, read, peek, op, parent, *data = RECORD.unpack(raw)
        if committed not in (0, 1):
            raise TraceError('publication')
        if not committed:
            incomplete = True
            continue
        if rv != 2 or not 1 <= event <= 28 or sequence != slot:
            raise TraceError('record schema')
        if process != generation or not thread or role not in (1, 2, 3, 4) or rf != 1:
            raise TraceError('record identity')
        incomplete |= event == 20
        rows.append(dict(event=event, sequence=sequence, thread=thread, role=role,
                         read=read, peek=peek, op=op, parent=parent, data=data))
    return dict(records=rows, incomplete=bool(incomplete), generation=generation)


def observe(trace):
    rows = trace['records']
    result = dict(schema=2, scope='observations-only', native_qualified=False,
                  matrix_adjudicator_ready=False, coverage_verdict='INCONCLUSIVE', complete=False, reasons=[],
                  wait_results=[], ingresses=[], restart_decisions=[], returns=[],
                  gates=[], handlers=[])
    if trace['incomplete']:
        result['reasons'].append('incomplete/overflow/context error')
    reads, waits, gates, signals, handlers, arms = {}, {}, {}, {}, {}, {}
    seen_gate_visits = []
    operation_stacks, operation_ids, active_reads = {}, set(), {}
    active_handlers, consumed_tokens = {}, set()
    operation_ends = {7: 9, 10: 11, 12: 13}
    end_begins = {end: begin for begin, end in operation_ends.items()}
    for r in rows:
        e, d, read = r['event'], r['data'], r['read']
        key = (r['thread'], read)
        stack = operation_stacks.setdefault(r['thread'], [])
        if e in operation_ends:
            expected_parent = stack[-1]['op'] if stack else 0
            if not r['op'] or r['op'] in operation_ids or r['parent'] != expected_parent:
                result['reasons'].append('operation begin identity/parent')
            if stack and (stack[-1]['read'], stack[-1]['peek'], stack[-1]['role']) != (read, r['peek'], r['role']):
                result['reasons'].append('nested operation ownership')
            operation_ids.add(r['op'])
            stack.append(r)
        elif e in end_begins:
            begin = stack.pop() if stack else None
            if begin is None or (begin['event'], begin['op'], begin['read'], begin['peek'], begin['role']) != (end_begins[e], r['op'], read, r['peek'], r['role']) or r['parent']:
                result['reasons'].append('operation end identity/nesting')
            elif e == 11 and begin['data'][:2] != d[:2]:
                result['reasons'].append('lock operation kind')
        elif e not in (18, 19):
            if r['op'] != (stack[-1]['op'] if stack else 0) or r['parent']:
                result['reasons'].append('operation membership')
            if stack and (stack[-1]['read'], stack[-1]['peek'], stack[-1]['role']) != (read, r['peek'], r['role']):
                result['reasons'].append('operation record ownership')
        if e == 8 and (not stack or stack[-1]['event'] != 7):
            result['reasons'].append('retirement API outside retirement')
        if e in (18, 19):
            # Neither nested handlers nor an external signal inside an internal
            # signal operation have an unambiguous observer ownership contract.
            if d[2] == 2 and signals:
                result['reasons'].append('external/internal signal overlap')
            if e == 18:
                if r['thread'] in active_handlers:
                    result['reasons'].append('overlapping handlers')
                active_handlers[r['thread']] = r
            else:
                entered = active_handlers.pop(r['thread'], None)
                if entered is None or (entered['read'], entered['peek'], entered['op'], entered['data'][:4]) != (read, r['peek'], r['op'], d[:4]):
                    result['reasons'].append('handler ownership pair')
        if e == 1:
            if not read or read in reads or r['role'] != 1:
                result['reasons'].append('read begin identity')
            if active_reads:
                result['reasons'].append('overlapping reads')
            active_reads[read] = r['thread']
            reads[read] = dict(begin=r, end=None)
        elif e == 15:
            if read not in reads or reads[read]['end'] is not None or not 1 <= d[3] <= 9:
                result['reasons'].append('read end/reason')
            else:
                reads[read]['end'] = r
                if r['thread'] != reads[read]['begin']['thread'] or r['role'] != 1:
                    result['reasons'].append('read end identity')
            active_reads.pop(read, None)
            if stack or key in waits or any(g[0] == read for g in gates) or r['thread'] in active_handlers:
                result['reasons'].append('read returned inside operation')
            result['returns'].append(dict(read=read, result=d[0], copied=d[1], errno=d[2], reason=d[3]))
        elif e == 21:
            if not read or key in waits or r['role'] != 1:
                result['reasons'].append('wait begin')
            waits[key] = r
        elif e == 22:
            begin = waits.pop(key, None)
            if begin is None or r['role'] != 1 or d[1] != begin['data'][1]:
                result['reasons'].append('wait pair')
            result['wait_results'].append(dict(read=read, result=d[0], copied=d[1]))
        elif e == 23:
            if d[0] not in (1, 2, 3) or not read or r['role'] != 1:
                result['reasons'].append('ingress identity')
            result['ingresses'].append(dict(read=read, kind=d[0], result=d[1], copied=d[2]))
        elif e == 24:
            if d[2] not in (0, 1) or not read or r['role'] != 1:
                result['reasons'].append('restart identity')
            result['restart_decisions'].append(dict(read=read, result=d[0], signal=d[1], restart=d[2]))
        elif e == 25:
            gate_key = (read, r['peek'], d[0])
            if gate_key in gates or d[2] != 1500 or r['role'] != 1 or not read or not r['peek']:
                result['reasons'].append('gate begin')
            gates[gate_key] = r
            seen_gate_visits.append(d[0])
        elif e == 26:
            begin = gates.pop((read, r['peek'], d[0]), None)
            if begin is None or begin['thread'] != r['thread'] or begin['data'][1] != d[1] or r['role'] != 1:
                result['reasons'].append('gate pair')
            if d[2:] != [0, 1, 0, 0, 0, 0]:
                result['reasons'].append('gate failed/timed out')
            result['gates'].append(dict(read=read, peek=r['peek'], visit=d[0], index=d[1], result=d[2]))
        elif e == 12:
            if not r['op'] or r['op'] in signals:
                result['reasons'].append('signal begin')
            signals[r['op']] = r
        elif e == 13:
            begin = signals.pop(r['op'], None)
            if begin is None or begin['data'][:3] != d[:3] or begin['thread'] != r['thread']:
                result['reasons'].append('signal pair')
        elif e == 28:
            if not d[0] or not d[1] or arms or read or r['role'] != 1:
                result['reasons'].append('external arm')
            arms[(d[0], d[1])] = r
        elif e in (18, 19):
            handler_key = (read, r['peek'], r['thread'], r['op'], *d[:4])
            source = d[2]
            if source == 1:
                origin = signals.get(r['op'])
                if origin is None or (origin['read'], origin['peek'], origin['thread'], origin['data'][0]) != (read, r['peek'], r['thread'], d[0]) or d[3]:
                    result['reasons'].append('internal handler correlation')
            elif source == 2:
                arm = arms.get((d[0], d[3]))
                if arm is None or arm['thread'] != r['thread'] or r['op']:
                    result['reasons'].append('external handler correlation')
                if e == 18:
                    token_key = (d[0], d[3])
                    if token_key in consumed_tokens:
                        result['reasons'].append('external token reused')
                    consumed_tokens.add(token_key)
            else:
                result['reasons'].append('unmatched handler')
            if r['role'] != 3 or not read or not d[1]:
                result['reasons'].append('handler identity')
            if e == 18:
                if handler_key in handlers:
                    result['reasons'].append('nested handler')
                handlers[handler_key] = r
            elif handlers.pop(handler_key, None) is None:
                result['reasons'].append('handler return')
            result['handlers'].append(dict(read=read, entering=e == 18, source=source, token=d[3]))
    if waits or gates or signals or handlers or active_handlers or any(operation_stacks.values()) or any(x['end'] is None for x in reads.values()):
        result['reasons'].append('unpaired operation/read')
    if seen_gate_visits != list(range(1, len(seen_gate_visits)+1)) or len(seen_gate_visits) > 8:
        result['reasons'].append('gate visit schedule')
    # Every reader/handler row that claims a read must lie in its exact lifetime.
    for r in rows:
        if r['read'] and r['role'] in (1, 3):
            interval = reads.get(r['read'])
            if interval is None or interval['end'] is None or not interval['begin']['sequence'] <= r['sequence'] <= interval['end']['sequence']:
                result['reasons'].append('record outside read')
                break
            if interval['begin']['thread'] != r['thread']:
                result['reasons'].append('read thread ownership')
    result['reasons'] = sorted(set(result['reasons']))
    result['complete'] = not result['reasons']
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('trace', type=Path); args = p.parse_args()
    try:
        with args.trace.open('rb') as stream:
            report = observe(decode(stream.read(MAX_BYTES+1)))
    except (OSError, TraceError) as error:
        report = dict(schema=2, complete=False, native_qualified=False,
                      matrix_adjudicator_ready=False, coverage_verdict='INCONCLUSIVE', reasons=[str(error)])
    print(json.dumps(report, indent=2))
