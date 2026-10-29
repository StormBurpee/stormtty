#!/usr/bin/env python3
"""One bounded authored control/trace/replay per provider; no retries or user state."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('responsiveness_owned_runner', HERE.parent / 'run-owned-process.py')
runner = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runner
spec.loader.exec_module(runner)
NUMBER = r'[0-9]+(?:\.[0-9]+)?'
CONTROL = re.compile(r'MANAGED_(?:STATE_CONTRACT index=[0-9]+ passed=[01]|STATE_RESULT code=[01] phase=[0-9]+ cold_samples=1 warm_samples=1 checks=[0-9]+ cold_ms=[0-9]+ warm_ms=[0-9]+|STATE_RESULT code=1 phase=[0-9]+ checks=[0-9]+ cold_ms=[0-9]+ warm_ms=[0-9]+|TYPING_RESULT code=[01] characters=[0-9]+ frames=[0-9]+ retreats=[0-9]+ echo_total_us=[0-9]+ echo_max_us=[0-9]+)')
TRACE_HEADER = re.compile(r'TYPING_TRACE version=2 overflow=[01] phase=[0-2] records=[0-9]+ bytes=[0-9]+')
TRACE_FIELDS = ('ordinal','us','sequence','offset','size','auxiliary','next','next_command','queued','generation','row','column','row_hash','visible','alternate','success','reported','error')
TRACE_EVENT = re.compile(r'TYPING_EVENT kind=(?:read|write|enqueue|feed|command|publication|marker|prelude_feed|wake)' + ''.join(' '+field+r'=[0-9]+' for field in TRACE_FIELDS) + r' hex=[0-9a-f]*')
REPLAY = re.compile(r'TYPING_REPLAY code=0 input_bytes=([0-9]+) input_hash=([0-9]+) output_bytes=[0-9]+ publications=[0-9]+ retreats=[0-9]+ queued_retreats=[0-9]+ drainable_retreats=[0-9]+')

def sanitize(output, traced=False, replay=False):
    lines=[]
    for line in output.decode('utf-8',errors='replace').splitlines():
        if replay and REPLAY.fullmatch(line): lines.append(line)
        elif not replay and (CONTROL.fullmatch(line) or re.fullmatch(r"TYPING_WAIT_MODE high_resolution=[01]",line) or re.fullmatch(r"TYPING_PUBLICATION_BUDGET ms=(?:8|16)",line)): lines.append(line)
        elif traced and (TRACE_HEADER.fullmatch(line) or TRACE_EVENT.fullmatch(line) or line=='END_TYPING_TRACE' or line=='TYPING_TRACE_RESULT inconclusive=1 reason=capacity'): lines.append(line)
    return lines

def valid(lines, traced=False, replay=False):
    if replay: return len(lines)==1 and REPLAY.fullmatch(lines[0]) is not None
    states=[line for line in lines if line.startswith('MANAGED_STATE_RESULT ')]
    typings=[line for line in lines if line.startswith('MANAGED_TYPING_RESULT ')]
    ok=(len(states)==1 and states[0].startswith('MANAGED_STATE_RESULT code=0 phase=11 ') and
        len(typings)==1 and typings[0].startswith('MANAGED_TYPING_RESULT code=0 characters=25 '))
    if traced:
        headers=[line for line in lines if TRACE_HEADER.fullmatch(line)]
        events=[line for line in lines if TRACE_EVENT.fullmatch(line)]
        ok=ok and len(headers)==1 and 'overflow=0 phase=2 ' in headers[0] and lines.count('END_TYPING_TRACE')==1
        if ok:
            count=int(re.search(r' records=([0-9]+)',headers[0])[1])
            byte_count=int(re.search(r' bytes=([0-9]+)',headers[0])[1])
            payloads=[line.rsplit(' hex=',1)[1] for line in events]
            ok=(len(events)==count<=2048 and byte_count<=1024*1024 and
                all(len(value)%2==0 for value in payloads) and sum(len(value)//2 for value in payloads)==byte_count)
            markers=[line for line in events if line.startswith('TYPING_EVENT kind=marker ')]
            ok=ok and len(markers)==2 and ' size=1 ' in markers[0] and ' size=2 ' in markers[1]
            # The C++ replay remains the authoritative order/frame/scope gate.
            # Never declare this structural sanitizer alone an exact replay.
    return ok

def one(command, output, *, traced=False, replay=False, invoke=None, deadline_mode=None, publication_budget=None):
    lines=[]; passed=False
    invoke=invoke or runner.run_owned
    try:
        result=invoke(command, timeout=120 if not replay else 45, reap_timeout=10, tail_bytes=1024*1024, output_dir=output.parent)
        lines=sanitize(result.output_tail,traced,replay)
        passed=(result.exit_code==0 and result.reaped and not result.timed_out and not result.output_truncated and not result.cleanup_error and valid(lines,traced,replay))
        if deadline_mode is not None:
            modes=[line for line in lines if line.startswith('TYPING_WAIT_MODE ')]
            wakes=[line for line in lines if line.startswith('TYPING_EVENT kind=wake ')]
            passed=passed and modes==['TYPING_WAIT_MODE high_resolution='+str(deadline_mode)] and bool(wakes) and (any(' alternate=1 ' in line for line in wakes) if deadline_mode else all(' alternate=0 ' in line for line in wakes))
        if publication_budget is not None:
            passed=passed and [line for line in lines if line.startswith('TYPING_PUBLICATION_BUDGET ')]==['TYPING_PUBLICATION_BUDGET ms='+str(publication_budget)]
        lines.append('RESPONSIVENESS_RUNNER exit_code=%d timed_out=%d reaped=%d truncated=%d cleanup_error=%d elapsed_seconds=%.3f' % (result.exit_code,result.timed_out,result.reaped,result.output_truncated,bool(result.cleanup_error),result.elapsed_seconds))
    except OSError:
        lines.append('RESPONSIVENESS_LAUNCH_ERROR code=1')
    lines.append('RESPONSIVENESS_EVIDENCE_VALID code='+str(0 if passed else 1))
    output.write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print(output.name+' valid='+str(int(passed)),flush=True)
    return passed,lines

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('build','runtime','provider','output'): p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--deadline-ab',action='store_true')
    p.add_argument('--cadence-ab',action='store_true')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    if a.cadence_ab and not a.deadline_ab:p.error('--cadence-ab requires --deadline-ab')
    failures=[];signatures={}
    for provider,extra in [('inbox',[]),('packaged',[str(a.provider.resolve())])]:
        cases=[('control',False),('trace',True)]+([('highres',True)] if a.deadline_ab else [])+([('highres16',True)] if a.cadence_ab else [])
        for case,traced in cases:
            exe='managed_shell_typing_trace.exe' if traced else 'managed_shell_state_probe.exe'
            args=[str((a.build/exe).resolve()),str(a.runtime.resolve()),*extra]
            if case.startswith('highres'):args.append('--high-resolution-deadlines')
            if case=='highres16':args.append('--publication-budget-16')
            output=a.output/(provider+'-'+case+'.log')
            mode=int(case.startswith('highres')) if a.deadline_ab and traced else None
            passed,_=one(args,output,traced=traced,deadline_mode=mode,publication_budget=(16 if case=='highres16' else 8) if a.cadence_ab and traced else None)
            if not passed: failures.append(provider+'-'+case)
            if traced:
                # Run replay even if probe/lifecycle failed: retain independent
                # parser evidence. A missing/incomplete trace must fail replay.
                ok,lines=one([str((a.build/'qualification_typing_evidence_tests.exe').resolve()),'--replay',str(output.resolve())],a.output/(provider+('-'+case+'-replay.log' if case.startswith('highres') else '-replay.log')),replay=True)
                if not ok: failures.append(provider+('-'+case+'-replay' if case.startswith('highres') else '-replay'))
                if ok:
                    match=next(REPLAY.fullmatch(line) for line in lines if REPLAY.fullmatch(line))
                    signatures[provider+('-'+case if case.startswith('highres') else '')]=(int(match[1]),int(match[2]))
    comparable=len(signatures)==(6 if a.cadence_ab else 4 if a.deadline_ab else 2) and len(set(signatures.values()))==1
    summary={'schema':1,'fixture':'authored managed shell warm quoted typing','retries':0,
             'deadline_ab':a.deadline_ab,'cadence_ab':a.cadence_ab,'budgets_ms':[8,16] if a.cadence_ab else [8],'instrumentation_perturbs_timing':True,'physical_display_verified':False,
             'completed_input_identical':comparable,'completed_input_signatures':signatures,'failed_cases':failures}
    if not comparable: failures.append('completed-input-comparison-inconclusive')
    (a.output/'comparison.json').write_text(json.dumps(summary,indent=2,sort_keys=True)+'\n',encoding='utf-8')
    print('RESPONSIVENESS_COMPARISON comparable='+str(int(comparable)),flush=True)
    return int(bool(failures))

if __name__=='__main__': raise SystemExit(main())
