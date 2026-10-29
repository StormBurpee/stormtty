"""Fixed owned-console smoke matrix; path qualification is ALWAYS inconclusive."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('refined_native_owned',HERE.parents[2]/'run-owned-process.py')
owned=importlib.util.module_from_spec(spec);sys.modules[spec.name]=owned;spec.loader.exec_module(owned)
SCENARIOS=('raw-vmin2','select','canonical','mouse-on','surrogate','longjmp')
EXPECTED={name:b'ab' for name in SCENARIOS}
EXPECTED.update(canonical=b'ab\n',longjmp=b'ab\n',surrogate='a😀b'.encode(),
                **{'mouse-on':b'a\x1b[<64;2;2Mb'})
ORDERS=(('stock','refined'),('refined','stock'))
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def parse(path):
    return [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()]
def assess(scenario,reader,injector,returncode):
    final=next((row for row in reversed(reader) if 'final_hex' in row),{})
    done=next((row for row in reversed(injector) if 'reader_exit' in row),{})
    geometry=next((row for row in injector if 'width_before' in row),{})
    ok=(returncode==0 and done.get('reader_exit')==0 and done.get('timeout') is False
        and final.get('final_hex')==EXPECTED[scenario].hex()
        and geometry.get('width_before',0)==geometry.get('width_after',0)+1
        and done.get('remaining_native_records')==0)
    return {'scenario_verdict':'pass' if ok else 'fail',
            'exact_path_verdict':'inconclusive',
            'reason':'No copied_chars, mouse-path, or prefix-retirement hook; native queue emptiness is insufficient.',
            'observed_hex':final.get('final_hex'), 'expected_hex':EXPECTED[scenario].hex(),
            'signals':final.get('signals'),
            'longjmp_observed':any(row.get('longjmp') is True for row in reader)}
def complete(result):
    return (result.reaped and not result.timed_out and
            not result.output_truncated and not result.cleanup_error)

def main():
    ap=argparse.ArgumentParser()
    for name in ('injector','reader','original','stock','refined','output'): ap.add_argument('--'+name,type=Path,required=True)
    a=ap.parse_args()
    if os.name!='nt': raise SystemExit('Windows native execution only')
    roots={n:getattr(a,n).resolve() for n in ('original','stock','refined')}
    spec=importlib.util.spec_from_file_location('refined_identities',HERE.parent/'refined-identities.py')
    module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    identities=module.identities(roots) # fail closed before any fixture or child
    a.output=a.output.absolute()
    a.output.mkdir(exist_ok=False)
    if not str(a.output).isascii(): raise ValueError('ANSI diagnostic fixture requires ASCII private paths')
    fixture=a.output/'private-fixtures';fixture.mkdir()
    for variant in ('stock','refined'):
        dst=fixture/variant; dst.mkdir()
        shutil.copyfile(a.reader,dst/'reader.exe')
        shutil.copyfile(roots[variant]/'usr/bin/msys-2.0.dll',dst/'msys-2.0.dll')
        assert sha(dst/'reader.exe')==sha(a.reader)
        assert sha(dst/'msys-2.0.dll')==identities[variant]['dll_sha256']
    result={'qualification':'inconclusive-no-runtime-hooks','identities':identities,
            'fixture_sha256':{'reader':sha(a.reader),'injector':sha(a.injector)},'trials':[]}
    for trial,order in enumerate(ORDERS,1):
        for scenario in SCENARIOS:
            for variant in order:
                run=a.output/f'{trial}-{scenario}-{variant}'; run.mkdir()
                reader_log=run/'reader.jsonl'; injector_log=run/'injector.jsonl'
                cmd=[str(a.injector.resolve()),str(fixture/variant/'reader.exe'),scenario,
                     str(reader_log),str(run/'ready.txt'),str(injector_log)]
                env=os.environ.copy();env['PATH']=str(fixture/variant)+os.pathsep+str(Path(env['SystemRoot'])/'System32')
                # Injector owns a job and its child; watchdog kills only its private reader.
                record={'trial':trial,'variant':variant,'scenario':scenario,'returncode':125}
                finished=False
                try:
                    child_result=owned.run_owned(cmd,env=env,timeout=20,reap_timeout=5,
                                           tail_bytes=65536,output_dir=run)
                    (run/'launcher.log').write_bytes(child_result.output_tail)
                    record.update(returncode=child_result.exit_code,runner=child_result.metadata())
                    finished=complete(child_result)
                except OSError:
                    record['launch_error']=True
                    (run/'launcher.log').write_text('Owned injector launch failed\n')
                returncode=record['returncode'] if finished else 125
                try: record.update(assess(scenario,parse(reader_log),parse(injector_log),returncode))
                except (OSError,ValueError) as exc:
                    record.update(scenario_verdict='fail',exact_path_verdict='inconclusive',parse_error=str(exc))
                result['trials'].append(record)
                (a.output/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    # Runs are not retried. Every independent case runs even after failure.
    return int(any(t['scenario_verdict']!='pass' for t in result['trials']))
if __name__=='__main__': raise SystemExit(main())
