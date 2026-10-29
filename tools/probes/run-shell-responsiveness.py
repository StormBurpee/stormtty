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
TRACE_HEADER = re.compile(r'TYPING_TRACE version=1 overflow=[01] records=[0-9]+ bytes=[0-9]+')
TRACE_FIELDS = ('ordinal','us','sequence','offset','size','auxiliary','next','next_command','queued','generation','row','column','row_hash','visible','alternate','success','reported','error')
TRACE_EVENT = re.compile(r'TYPING_EVENT kind=(?:read|write|enqueue|feed|command|publication|marker)' + ''.join(' '+field+r'=[0-9]+' for field in TRACE_FIELDS) + r' hex=[0-9a-f]*')
REPLAY = re.compile(r'TYPING_REPLAY code=0 input_bytes=([0-9]+) input_hash=([0-9]+) output_bytes=[0-9]+ publications=[0-9]+ retreats=[0-9]+ queued_retreats=[0-9]+ drainable_retreats=[0-9]+')

def sanitize(output, traced=False, replay=False):
    lines=[]
    for line in output.decode('utf-8',errors='replace').splitlines():
        if replay and REPLAY.fullmatch(line): lines.append(line)
        elif not replay and CONTROL.fullmatch(line): lines.append(line)
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
        ok=ok and len(headers)==1 and 'overflow=0 ' in headers[0] and lines.count('END_TYPING_TRACE')==1
        if ok: ok=len(events)==int(re.search(r' records=([0-9]+)',headers[0])[1])
    return ok

def one(command, output, *, traced=False, replay=False, invoke=None):
    lines=[]; passed=False
    invoke=invoke or runner.run_owned
    try:
        result=invoke(command, timeout=120 if not replay else 45, reap_timeout=10, tail_bytes=1024*1024, output_dir=output.parent)
        lines=sanitize(result.output_tail,traced,replay)
        passed=(result.exit_code==0 and result.reaped and not result.timed_out and not result.output_truncated and not result.cleanup_error and valid(lines,traced,replay))
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
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    failures=[];signatures={}
    for provider,extra in [('inbox',[]),('packaged',[str(a.provider.resolve())])]:
        for case,traced in [('control',False),('trace',True)]:
            exe='managed_shell_typing_trace.exe' if traced else 'managed_shell_state_probe.exe'
            args=[str((a.build/exe).resolve()),str(a.runtime.resolve()),*extra]
            output=a.output/(provider+'-'+case+'.log')
            passed,_=one(args,output,traced=traced)
            if not passed: failures.append(provider+'-'+case)
            if traced:
                # Run replay even if probe/lifecycle failed: retain independent
                # parser evidence. A missing/incomplete trace must fail replay.
                ok,lines=one([str((a.build/'qualification_typing_evidence_tests.exe').resolve()),'--replay',str(output.resolve())],a.output/(provider+'-replay.log'),replay=True)
                if not ok: failures.append(provider+'-replay')
                if ok:
                    match=next(REPLAY.fullmatch(line) for line in lines if REPLAY.fullmatch(line))
                    signatures[provider]=(int(match[1]),int(match[2]))
    comparable=len(signatures)==2 and signatures['inbox']==signatures['packaged']
    summary={'schema':1,'fixture':'authored managed shell warm quoted typing','retries':0,
             'instrumentation_perturbs_timing':True,'physical_display_verified':False,
             'completed_input_identical':comparable,'completed_input_signatures':signatures,'failed_cases':failures}
    if not comparable: failures.append('completed-input-comparison-inconclusive')
    (a.output/'comparison.json').write_text(json.dumps(summary,indent=2,sort_keys=True)+'\n',encoding='utf-8')
    print('RESPONSIVENESS_COMPARISON comparable='+str(int(comparable)),flush=True)
    return int(bool(failures))

if __name__=='__main__': raise SystemExit(main())
