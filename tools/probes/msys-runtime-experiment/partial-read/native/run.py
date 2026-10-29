#!/usr/bin/env python3
"""Fixed twelve trials, never retries; byte outcomes and internal coverage separate."""
import argparse,hashlib,importlib.util,json,os,pathlib,shutil,sys
HERE=pathlib.Path(__file__).resolve().parent

def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);sys.modules[name]=m;spec.loader.exec_module(m);return m
owned=module('passive_owned',HERE.parents[3]/'run-owned-process.py')
parser=module('passive_parser',HERE.parents[1]/'passive-path/parser.py')
identity=module('passive_identity',HERE/'identities.py')
resize=module('partial_read_resize',HERE/'resize-predicate.py')
external=module('passive_environment',HERE.parents[1]/'refined-native/run.py')
CASES=('partial-zero','positive-prefix','zero-copy')
ORDERS=(('stock','copied'),('copied','stock'))
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def lines(path):
 with path.open('rb') as stream:blob=stream.read(65537)
 if len(blob)>65536:raise ValueError('oversize fixture log')
 rows=[json.loads(line) for line in blob.decode('utf-8').splitlines()]
 if any(not isinstance(row,dict) for row in rows):raise ValueError('non-object fixture row')
 return rows
def assess(case,reader,controller,runner,trace,variant,coverage=None):
 end=next((row for row in reversed(reader) if 'synthetic_hex' in row),{})
 done=next((row for row in reversed(controller) if 'cleanup_ok' in row),{})
 queue=next((row for row in reversed(controller) if 'remaining_native_records' in row),{})
 bounded=(runner.get('reaped') is True and runner.get('timed_out') is False and
          runner.get('output_truncated') is False and runner.get('cleanup_error')=='' and runner.get('returncode')==0)
 byte_ok=(bounded and end.get('result')==0 and end.get('synthetic_hex')=={'partial-zero':'6162','positive-prefix':'616263','zero-copy':'62'}[case] and
          done.get('cleanup_ok') is True and done.get('timeout') is False and done.get('scenario_ok') is True and
          done.get('reader_exit')==0 and queue.get('remaining_native_records')==0)
 coverage=parser.analyze(trace,case,variant) if coverage is None else coverage
 # An incomplete owned outcome cannot qualify even a well-formed supplied trace.
 if (not end or not queue or not bounded or done.get('cleanup_ok') is not True or done.get('timeout') is not False or
     done.get('reader_exit')!=0 or done.get('scenario_ok') is not True):
  coverage.update(verdict='INCONCLUSIVE',reason='owned-outcome-incomplete')
 return dict(byte_verdict='PASS' if byte_ok else 'FAIL',coverage=coverage,native_promotion_allowed=False)
def component_verdict(result):
 # Legacy mouse/master coverage is retained, not relabelled as resize proof.
 if result.get('byte_verdict')!='PASS':return 'FAIL'
 observed=result.get('resize_observation',{})
 return ('PASS' if observed.get('verdict')=='RESIZE_OBSERVED' and
         observed.get('expected_return_match') is True else 'INCONCLUSIVE')
def collect_assessment(case,run,runner,variant):
 # Decode the bounded trace BEFORE fixture logs. An alarm/crash may truncate a
 # log without invalidating already committed numeric observations in the map.
 result=dict(byte_verdict='FAIL',component_verdict='FAIL',native_promotion_allowed=False)
 try:
  with (run/'trace.bin').open('rb') as stream:trace=stream.read(parser.MAX_BYTES+1)
 except OSError as error:
  result.update(coverage=parser.analyze(b'',case,variant),fixture_error=type(error).__name__)
  return result
 coverage=parser.analyze(trace,case,variant)
 result['coverage']=coverage
 result['resize_observation']=resize.analyze(trace,case,variant)
 if len(trace)<=parser.MAX_BYTES:result['trace_sha256']=hashlib.sha256(trace).hexdigest()
 try:
  reader=lines(run/'reader.jsonl');controller=lines(run/'controller.jsonl')
 except (OSError,ValueError) as error:
  coverage.update(verdict='INCONCLUSIVE',reason='fixture-log-incomplete')
  result['fixture_error']=type(error).__name__
  return result
 result.update(assess(case,reader,controller,runner,trace,variant,coverage=coverage))
 result['component_verdict']=component_verdict(result)
 return result

def main():
 ap=argparse.ArgumentParser()
 for name in ('controller','reader','original','stock','copied','output'):ap.add_argument('--'+name,type=pathlib.Path,required=True)
 a=ap.parse_args()
 plan=json.loads((HERE/'trial-plan.json').read_text())
 if plan['planned']!=12 or plan['cases']!=list(CASES) or plan['orders']!=[list(order) for order in ORDERS] or plan.get('production_qualified') is not False:raise ValueError('fixed trial plan drift')
 for filename,digest in plan['source_sha256'].items():
  if filename not in ('reader.c','controller.c','recorder.hpp','stock.patch','copied.patch') or sha(HERE/filename)!=digest:raise ValueError('fixed source identity drift')
 if len(plan['source_sha256'])!=5:raise ValueError('fixed source identity missing')
 if plan['expected_hex']!={'partial-zero':'6162','positive-prefix':'616263','zero-copy':'62'} or plan.get('stock_positive_failure_not_waived') is not True:raise ValueError('fixed outcome contract drift')
 if plan['timeouts']!=dict(reader_alarm_seconds=8,controller_ready_ms=4000,queue_hint_ms=1500,child_wait_ms=10000,reap_ms=2000,runner_seconds=25,runner_reap_seconds=5):raise ValueError('fixed bounds drift')
 if os.name!='nt':raise SystemExit('Windows native only')
 roots={name:getattr(a,name).resolve() for name in ('original','stock','copied')}
 identities=identity.identities(roots)
 a.output=a.output.absolute()
 if not str(a.output).isascii():raise ValueError('private ANSI paths must be ASCII')
 a.output.mkdir(exist_ok=False);private=a.output/'private-fixtures';private.mkdir()
 for variant in ('stock','copied'):
  dst=private/variant;dst.mkdir();shutil.copyfile(a.reader,dst/'reader.exe')
  shutil.copyfile(roots[variant]/'usr/bin/msys-2.0.dll',dst/'msys-2.0.dll')
  if sha(dst/'reader.exe')!=sha(a.reader) or sha(dst/'msys-2.0.dll')!=identities[variant]['dll_sha256']:raise ValueError('private fixture copy mismatch')
 result=dict(planned=12,trials=[],identities=identities,fixture_sha256={n:sha(getattr(a,n)) for n in ('reader','controller')},production_qualified=False)
 result['trial_plan']=plan
 result['trial_plan_sha256']=sha(HERE/'trial-plan.json')
 report=a.output/'results.json'
 def save():report.write_text(json.dumps(result,indent=2)+'\n')
 save()
 for trial,order in enumerate(ORDERS,1):
  for case in CASES:
   for variant in order:
    run=a.output/f'{trial}-{case}-{variant}';run.mkdir()
    record=dict(trial=trial,case=case,variant=variant,byte_verdict='FAIL',coverage=dict(verdict='INCONCLUSIVE',reason='not-executed'))
    try:
     command=[str(a.controller.resolve()),str(private/variant/'reader.exe'),case,str(run/'reader.jsonl'),str(run/'ready.txt'),str(run/'controller.jsonl'),str(run/'trace.bin')]
     outcome=owned.run_owned(command,env=external.fixture_environment(os.environ.copy(),private/variant),timeout=25,reap_timeout=5,tail_bytes=65536,output_dir=run)
     record['runner']=outcome.metadata();(run/'launcher.log').write_bytes(outcome.output_tail)
     record.update(collect_assessment(case,run,record['runner'],variant))
    except (OSError,ValueError) as error:
     record['fixture_error']=type(error).__name__
    result['trials'].append(record);save()
 if any(row['byte_verdict']!='PASS' for row in result['trials']):return 1
 return 0 if all(row.get('component_verdict')=='PASS' for row in result['trials']) else 2
if __name__=='__main__':raise SystemExit(main())
