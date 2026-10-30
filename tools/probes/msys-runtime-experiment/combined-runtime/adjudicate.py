"""Stage-1 observations: byte correctness, path coverage and mechanism separate.

No assertion here promotes a runtime. Missing later matrix cases remain explicit.
"""
import importlib.util
import json
from pathlib import Path
import sys
HERE = Path(__file__).resolve().parent

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec);sys.modules[name] = value;spec.loader.exec_module(value)
    return value

trace = module('combined_trace_stage', HERE/'trace.py')
matrix = module('combined_matrix_stage', HERE/'matrix.py')


def bounded_json(path, limit=65536):
    with Path(path).open('rb') as stream:
        raw = stream.read(limit+1)
    if len(raw) > limit:
        raise ValueError('oversize result')
    # Reject duplicated fields, non-finite JSON, trailing second objects. CRLF
    # whitespace is accepted without altering binary trace or byte evidence.
    def pairs(items):
        result = {}
        for k,v in items:
            if k in result: raise ValueError('duplicate field')
            result[k] = v
        return result
    value = json.loads(raw.decode('utf-8'), object_pairs_hook=pairs,
                       parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite')))
    if not isinstance(value, dict):raise ValueError('result is not object')
    return value


def path_observations(case, variant, decoded):
    structural = trace.observe(decoded)
    reasons = list(structural['reasons'])
    mechanism = []
    rows = decoded['records']
    read_rows = [r for r in rows if r['role'] == 1]
    begins = [r for r in read_rows if r['event'] == 1]
    ends = [r for r in read_rows if r['event'] == 15]
    if not begins or not ends:reasons.append('read path missing')
    requested = 1 if case == 'raw-1' else 64
    canonical = int(case == 'canonical')
    if any(r['data'][0] != requested or r['data'][1] != canonical for r in begins):
        reasons.append('read configuration differs')
    if not canonical and any(r['data'][2:4] != [1,0] for r in begins):
        reasons.append('raw VMIN/VTIME differs')
    if any(r['data'][3] != 6 and not (case.startswith(('changed-', 'prefix-', 'storm-')) and r['data'][3] == 9 and r['data'][0] == (1 << 64)-1 and r['data'][2] == 4) for r in ends):
        reasons.append('unexpected read return path')
    gates = [r for r in read_rows if r['event'] == 25]
    gate_count = 3 if case.startswith('storm-') else 1 if case.startswith(('changed-', 'prefix-')) else 0
    if len(gates) != gate_count:reasons.append('gate coverage differs')
    if not gate_count and any(r['event']==12 or (r['event']==6 and r['data'][6]) for r in read_rows):
        reasons.append('unexpected changed geometry or signal in stable control')
    if case == 'select-raw' and not any(r['event'] == 4 and r['data'][4] == 2 and not r['read'] for r in read_rows):
        reasons.append('select len-zero peek missing')
    event_type = 2 if 'mouse' in case else 16 if 'focus' in case else 4 if case == 'stable-resize' else None
    if event_type is not None:
        index = 1 if case.startswith('prefix-') else 0
        dispatch = [r for r in read_rows if r['event'] == 5 and r['data'][0] == event_type and r['data'][1] == index]
        if not dispatch:reasons.append('selected dispatch missing')
        reporting = case.endswith('-on')
        for r in dispatch:
            mode = r['data'][4] if event_type == 2 else r['data'][7] if event_type == 16 else 0
            if event_type in (2,16) and mode != int(reporting):reasons.append('actual report mode differs')
        geometry = [r for r in read_rows if r['event'] == 6 and r['data'][0] == event_type and r['data'][1] == index]
        if event_type in (2,4) and (not geometry or any(r['data'][7] != 1 for r in geometry)):
            reasons.append('geometry query coverage missing')
        changed = [r for r in geometry if r['data'][6] == 1]
        if len(changed) != gate_count:reasons.append('changed geometry coverage differs')
        all_dispatch=[r for r in read_rows if r['event']==5 and r['data'][0]==event_type]
        if all_dispatch:
            final=all_dispatch[-1]
            retirement=[r for r in read_rows if r['event']==9 and r['read']==final['read'] and r['peek']==final['peek'] and r['sequence']>final['sequence']]
            retired_apis=[r for r in read_rows if r['event']==8 and r['read']==final['read'] and r['peek']==final['peek'] and r['sequence']>final['sequence']]
            if not retirement or not retired_apis or any(r['data'][1]!=1 or not 0<r['data'][2]<=r['data'][0] or r['data'][3:5]!=[0,0] for r in retired_apis):
                reasons.append('normal retirement coverage missing/failed')
        if gate_count:
            used_gates=set();used_signals=set()
            # Pair each selected gate and changed query by actual read/peek,
            # then demand its internal handler, original input unlock and end.
            # Even a bounded malicious trace cannot force quadratic scans.
            # Extra changes already invalidate coverage; inspect at most the
            # observer's eight possible scheduling gates.
            for change in changed[:8]:
                same = [r for r in read_rows if r['read'] == change['read'] and r['peek'] == change['peek']]
                preceding = [r for r in same if r['event'] == 26 and r['sequence'] < change['sequence']]
                signals = [r for r in same if r['event'] == 12 and r['sequence'] > change['sequence']]
                if not preceding or preceding[-1]['data'][1] != index or not signals:
                    reasons.append('changed query lacks gate/signal');continue
                gate_end=preceding[-1]
                gate_begin=next((r for r in same if r['event']==25 and r['data'][0]==gate_end['data'][0] and r['sequence']<gate_end['sequence']),None)
                selected=[r for r in same if r['event']==5 and r['data'][:2]==[event_type,index] and gate_begin and r['sequence']<gate_begin['sequence']]
                if gate_end['sequence'] in used_gates or gate_begin is None or not selected:
                    reasons.append('gate/query chain reused or unordered')
                used_gates.add(gate_end['sequence'])
                signal = signals[0]
                if signal['op'] in used_signals:reasons.append('signal/query chain reused')
                used_signals.add(signal['op'])
                handlers = [r for r in rows if r['event'] == 18 and r['op'] == signal['op'] and r['data'][2] == 1]
                unlocks = [r for r in same if r['event'] == 10 and r['data'][:2] == [1,2]
                           and change['sequence'] < r['sequence'] < signal['sequence']]
                if not handlers or len(unlocks)!=1:reasons.append('changed query lacks handler/unlock');continue
                unlocked=next((r for r in same if r['event']==11 and r['op']==unlocks[0]['op'] and unlocks[0]['sequence']<r['sequence']<signal['sequence']),None)
                if unlocked is None or unlocked['data'][:5]!=[1,2,1,0,0]:
                    reasons.append('input unlock failed or missing')
                if variant == 'combined':
                    retired = [r for r in same if r['event'] == 9 and change['sequence'] < r['sequence'] < unlocks[0]['sequence']]
                    if index == 0 and retired:mechanism.append('zero prefix retired before unlock')
                    apis=[r for r in same if r['event']==8 and change['sequence']<r['sequence']<unlocks[0]['sequence']]
                    if index == 0 and apis:mechanism.append('zero prefix retirement API before unlock')
                    if index == 1 and (len(retired) != 1 or retired[0]['data'][0:2] != [1,1] or len(apis)!=1 or apis[0]['data'][:5]!=[1,1,1,0,0]):
                        mechanism.append('positive prefix not retired exactly once')
                    finish = next((r for r in same if r['event'] == 13 and r['op'] == signal['op']), None)
                    if finish is None:reasons.append('signal end missing');continue
                    if any(r['event'] == 5 and r['sequence'] > finish['sequence'] for r in same):
                        mechanism.append('continued abandoned peek')
                    fresh = [r for r in read_rows if r['event'] == 4 and r['sequence'] > finish['sequence'] and r['peek'] != change['peek']]
                    if not fresh:reasons.append('fresh peek missing after signal')
                    fresh_ids={r['peek'] for r in fresh}
                    revisits=[r for r in read_rows if r['event']==5 and r['peek'] in fresh_ids and r['data'][:2]==[2,0] and r['sequence']>finish['sequence']]
                    if not revisits:reasons.append('fresh head mouse redispatch missing')
            if len(used_gates)!=gate_count or len(used_signals)!=gate_count:
                reasons.append('gate/query/signal exposure not bijective')
            if case.startswith('storm-'):
                if not changed or not any(r['data'][6] == 0 and r['sequence'] > changed[-1]['sequence'] for r in geometry):
                    reasons.append('finite storm lacks stable query')
    return dict(structurally_complete=structural['complete'], coverage_reasons=sorted(set(reasons)),
                mechanism_reasons=sorted(set(mechanism)), trace_observations=structural)


def exact_json(actual,expected):
    if type(actual) is not type(expected):return False
    if isinstance(expected,dict):return actual.keys()==expected.keys() and all(exact_json(actual[k],v) for k,v in expected.items())
    if isinstance(expected,list):return len(actual)==len(expected) and all(exact_json(a,b) for a,b in zip(actual,expected))
    return actual==expected


def input_inventory(case, fixture):
    def key(char):return dict(type=1,unicode=ord(char),down=True,repeat=1,virtual_key=0,scan_code=0,controls=0)
    geometry = fixture
    if case in ('raw-1','raw-64','canonical','select-raw'):
        expected = [key('a'),key('b')] + ([key('\r')] if case == 'canonical' else [])
    else:
        if 'mouse' in case:
            if not all(type(geometry.get(k)) is int for k in ('initial_left','initial_top')):return False
            record = dict(type=2,x=geometry['initial_left']+1,y=geometry['initial_top']+1,flags=4,buttons=120<<16,controls=0)
        elif 'focus' in case:record = dict(type=16,focused=True)
        else:
            if not all(type(geometry.get(k)) is int for k in ('buffer_width','buffer_height')):return False
            record = dict(type=4,width=geometry['buffer_width'],height=geometry['buffer_height'])
        expected = ([key('a')] if case.startswith('prefix-') else []) + [record,key('b')]
    batches = fixture.get('input_batches')
    return exact_json(batches,[dict(requested=len(expected), accepted=len(expected), records=expected)])


def assess(case, variant, generation, fixture, runner, decoded):
    if case not in matrix.STAGE_CASES or variant not in ORDINARY_VARIANTS:
        raise ValueError('unsupported case/variant')
    path = path_observations(case, variant, decoded)
    reasons = path['coverage_reasons']
    integer_fields=('schema','generation','queue_before','queue_remaining','child_exit','cleanup_errors','fixture_error',
                    'initial_input_mode','initial_output_mode','initial_left','initial_top','initial_width','initial_height','buffer_width','buffer_height')
    if any(type(fixture.get(k)) is not int for k in integer_fields) or type(runner.get('returncode')) is not int:
        reasons.append('numeric fixture/runner field type')
    if fixture.get('schema') != 2 or fixture.get('stage') != 'console-records' or fixture.get('case') != case or fixture.get('generation') != generation or decoded['generation'] != generation:
        reasons.append('fixture/trace identity differs')
    bounded = (runner.get('reaped') is True and runner.get('timed_out') is False and
               runner.get('output_truncated') is False and runner.get('cleanup_error') == '' and runner.get('returncode') in (0,1))
    cleaned = (fixture.get('reaped') is True and fixture.get('assigned') is True and fixture.get('resumed') is True and
               fixture.get('timed_out') is False and fixture.get('cleanup_errors') == 0 and
               fixture.get('all_writers_stopped') is True and fixture.get('trace_written') is True and fixture.get('queue_remaining_valid') is True)
    if not bounded or not cleaned:reasons.append('owned lifecycle incomplete')
    if fixture.get('fixture_error') != 0 or fixture.get('queue_before') != 0 or not input_inventory(case, fixture):
        reasons.append('input exposure invalid')
    reader = fixture.get('reader')
    if not isinstance(reader,dict):
        reasons.append('reader is not object');reader={}
    reader_integers=('configured','finished','result','input_mode','output_mode','canonical','vmin','vtime','mouse','focus',
                     'select_called','select_result','select_error','termios_restored')
    if any(type(reader.get(k)) is not int for k in reader_integers):reasons.append('numeric reader field type')
    modes=[fixture.get('initial_input_mode'),fixture.get('initial_output_mode'),reader.get('input_mode'),reader.get('output_mode')]
    if any(type(v) is not int or not 0<=v<=0xffffffff for v in modes) or fixture.get('initial_input_mode')!=0x98:
        reasons.append('native mode observations missing/different')
    canonical = int(case == 'canonical')
    if any(reader.get(k) != v for k,v in dict(configured=1,finished=1,canonical=canonical,vmin=1,vtime=0,
        mouse=int('mouse-on' in case),focus=int(case == 'stable-focus-on'),termios_restored=1).items()):
        reasons.append('reader configuration/lifecycle incomplete')
    if case == 'select-raw':
        if any(reader.get(k) != v for k,v in dict(select_called=1,select_result=1,select_error=0).items()):reasons.append('select readiness missing')
    elif reader.get('select_called') != 0:reasons.append('unexpected select')
    reads = reader.get('reads', [])
    if not isinstance(reads,list) or not 1 <= len(reads) <= 16:
        reasons.append('read observations missing');reads=[]
    requested = 1 if case == 'raw-1' else 64
    valid_reads = all(isinstance(r,dict) and all(type(r.get(k)) is int for k in ('count','requested','errno')) and r.get('requested') == requested and
        ((0 < r['count'] <= requested and r.get('errno') == 0) or
         (r['count'] == -1 and r.get('errno') == 4 and case.startswith(('changed-', 'prefix-', 'storm-')))) for r in reads)
    if not valid_reads:reasons.append('invalid read result')
    output = reader.get('output_hex')
    if not isinstance(output,str) or len(output)>256 or len(output)%2 or any(c not in '0123456789abcdef' for c in output):
        reasons.append('invalid output bytes');output=None
    if output is not None and valid_reads and sum(max(0,r['count']) for r in reads) != len(output)//2:
        reasons.append('read/output count mismatch')
    # Cross-check read evidence with runtime return sites, not solely two logs.
    returns = [r for r in decoded['records'] if r['event']==15 and r['role']==1]
    if valid_reads and [(r['data'][0] if r['data'][0] < (1<<63) else r['data'][0]-(1<<64),r['data'][2]) for r in returns] != [(r['count'],r['errno']) for r in reads]:
        reasons.append('reader/runtime return mismatch')
    expected_gates = 3 if case.startswith('storm-') else 1 if case.startswith(('changed-','prefix-')) else 0
    gates = fixture.get('gates')
    if not isinstance(gates,list) or len(gates)!=expected_gates:reasons.append('controller gate count')
    else:
        for i,g in enumerate(gates):
            if not isinstance(g,dict) or any(type(v) is not int for v in g.values()) or any(g.get(k)!=v for k,v in dict(visit=i+1,wait_result=0,wait_error=0,geometry_error=0,release_error=0,arrived=1,queried=1,applied=1,released=1).items()):
                reasons.append('controller gate failed');continue
            if not all(type(g.get(k)) is int for k in ('width_before','width_after','height_before','height_after')) or g['width_after'] != g['width_before'] + (1 if i==1 else -1) or g['height_after'] != g['height_before']:
                reasons.append('controller geometry differs')
    if expected_gates and isinstance(gates,list):
        selected_index=int(case.startswith('prefix-'))
        changed=[r for r in decoded['records'] if r['role']==1 and r['event']==6 and r['data'][:2]==[2,selected_index] and r['data'][6]==1]
        width,height=fixture.get('initial_width'),fixture.get('initial_height')
        if type(width) is not int or type(height) is not int or not 1<width<=32767 or not 1<height<=32767 or len(changed)!=len(gates):
            reasons.append('controller/trace geometry inventory differs')
        else:
            for g,r in zip(gates,changed):
                if not isinstance(g,dict):continue
                if g.get('width_before')!=width or g.get('height_before')!=height or r['data'][2:6]!=[g.get('width_before'),g.get('height_before'),g.get('width_after'),g.get('height_after')]:
                    reasons.append('controller/trace geometry chain differs')
                width,height=g.get('width_after'),g.get('height_after')
    byte_verdict = 'INCONCLUSIVE' if output is None else 'PASS' if output == matrix.EXPECTED[case].hex() and fixture.get('queue_remaining') == 0 and reader.get('result') == 0 and fixture.get('child_exit') == 0 else 'FAIL'
    if byte_verdict=='PASS' and (fixture.get('scenario_ok') is not True or runner.get('returncode')!=0):
        reasons.append('controller success evidence inconsistent')
    if byte_verdict=='FAIL' and fixture.get('scenario_ok') is True:
        reasons.append('controller failure evidence inconsistent')
    mechanism_verdict = 'FAIL' if path['mechanism_reasons'] else 'INCONCLUSIVE' if reasons else 'PASS'
    verdict = 'INCONCLUSIVE' if reasons else 'FAIL' if byte_verdict=='FAIL' or mechanism_verdict=='FAIL' else 'PASS'
    return dict(case=case,variant=variant,byte_verdict=byte_verdict,mechanism_verdict=mechanism_verdict,
        component_verdict=verdict,coverage_reasons=sorted(set(reasons)),mechanism_reasons=path['mechanism_reasons'],
        output_hex=output,expected_hex=matrix.EXPECTED[case].hex(),native_qualified=False,runtime_promotion_allowed=False)

ORDINARY_VARIANTS = ('copied','combined')
