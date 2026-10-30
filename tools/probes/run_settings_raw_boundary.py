#!/usr/bin/env python3
"""One authored Settings-close event per stock provider. Never retries or promotes."""
import argparse,hashlib,importlib.util,json,os,pathlib,re,sys
HERE=pathlib.Path(__file__).resolve().parent

EXPECTED=(b'\x1b[200~PASTE_QUAL_0123456789_[x]_end\x1b[201~\r').hex()
def assess(metadata,output,provider=None):
    result={'boundary_verdict':'INCONCLUSIVE','resize_reproduction':'INCONCLUSIVE','native_qualified':False,'runtime_promotion_allowed':False}
    def reject(reason):result['reason']=reason;return result
    if metadata['timed_out'] or not metadata['reaped'] or metadata['output_truncated'] or metadata['cleanup_error']:
        return reject('owned runner incomplete')
    lines=output.splitlines()
    if lines.count('Native smoke shell state cleanup: actors_closed=1 removed=1')!=1 or len([s for s in lines if re.fullmatch(r'Native CI teardown: tick_ms=\d+ thread=\d+ stage=wWinMain locals destroyed; returning to CRT',s)])!=1:
        return reject('owned actor/root cleanup evidence missing')
    posts=[s for s in lines if s.startswith('SETTINGS_RAW_POST')];rows=[s for s in lines if s.startswith('SETTINGS_RAW_RESULT')];finals=[s for s in lines if s.startswith('SETTINGS_RAW_FINAL')]
    if len(posts)!=1 or len(rows)!=1 or len(finals)!=1:return reject('single posted event/result/final evidence missing')
    post=re.fullmatch(r'SETTINGS_RAW_POST separate_dispatch=1 closed_scene_presented=1 bracketed=1 resize_requested=(\d+) resize_applied=(\d+) resize_since_raw_ready=([01]) resize_before=(\d+) client_width_before=(\d+) client_width_after=(\d+) grid_columns_before=(\d+) grid_columns_after=(\d+) reader_termios=raw-noecho win32_input_mode=unobserved provider=(.+)',posts[0])
    if not post:return reject('malformed post evidence')
    requested,applied,observed,before,width_before,width_after,cols_before,cols_after=map(int,post.groups()[:8])
    if requested!=applied or applied<=before or width_after!=width_before+80 or cols_after<=cols_before or not observed:
        return reject('single applied real resize evidence missing')
    actual=post[9]
    if provider not in (None,'inbox','packaged'):return reject('unknown provider')
    if provider=='inbox' and actual!='Windows ConPTY (system)':return reject('provider mismatch')
    if provider=='packaged' and actual!='Windows ConPTY (packaged experimental 1.25.260930003; OpenConsole identity verified)':return reject('provider mismatch')
    row=re.fullmatch(r'SETTINGS_RAW_RESULT verdict=(PASS|MISMATCH|INCONCLUSIVE) writer_complete=([01]) child_valid=([01]) child_exit=(-?\d+) expected_hex=([0-9a-f]*) writer_hex=([0-9a-f]*) child_hex=([0-9a-f]*)',rows[0])
    if not row:return reject('incomplete bounded byte result')
    verdict,complete,valid,child_exit,expected,writer,child=row.groups()
    if len(writer)>524288 or len(child)>2048 or len(writer)%2 or len(child)%2 or (valid=='1' and not child) or expected!=EXPECTED:return reject('byte evidence bounds/identity')
    if verdict=='PASS':
        if metadata['returncode']!=0 or complete!='1' or valid!='1' or child_exit!='0' or writer!=EXPECTED or child!=EXPECTED:return reject('PASS byte/exit evidence inconsistent')
        if finals[0]!='SETTINGS_RAW_FINAL verdict=PASS detail=; authored Settings-close boundary only, not ZLE or general qualification.':return reject('PASS final evidence inconsistent')
    else:
        if metadata['returncode']!=2 or not re.fullmatch(r'SETTINGS_RAW_FINAL verdict=INCONCLUSIVE_OR_MISMATCH detail=.+; authored Settings-close boundary only, not ZLE or general qualification\.',finals[0]):return reject('nonpass final/exit mismatch')
        if verdict=='MISMATCH' and (complete!='1' or valid!='1' or child_exit!='0' or (writer==EXPECTED and child==EXPECTED)):return reject('MISMATCH evidence inconsistent')
    result['boundary_verdict']=verdict
    result['resize_reproduction']='OBSERVED' if post[3]=='1' else 'INCONCLUSIVE'
    result['reason']='one authored boundary only; direct Win32 input mode and ZLE unobserved'
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('build-dir','shell-runtime','console-runtime'):parser.add_argument('--'+name,type=pathlib.Path,required=True)
    args=parser.parse_args()
    if sys.platform!='win32':parser.error('native Windows only; no cross-build runtime claim')
    build=args.build_dir.resolve(strict=True);runtime=args.shell_runtime.resolve(strict=True);provider=args.console_runtime.resolve(strict=True)
    app=build/'stormtty_settings_raw_probe.exe'
    if not app.is_file():raise ValueError('missing separate diagnostic executable')
    artifacts=build/'settings-raw-artifacts';artifacts.mkdir(exist_ok=False)
    spec=importlib.util.spec_from_file_location('settings_owned_runner',HERE.parent/'run-owned-process.py')
    runner=importlib.util.module_from_spec(spec);sys.modules[spec.name]=runner;spec.loader.exec_module(runner)
    def digest(path):
        with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
    identities={'commit':os.environ.get('GITHUB_SHA','unrecorded'),'diagnostic_sha256':digest(app),
        'stock_zsh_sha256':digest(runtime/'usr/bin/zsh.exe'),'stock_msys_sha256':digest(runtime/'usr/bin/msys-2.0.dll')}
    records=[]
    for name,extra in [('inbox',[]),('packaged',['--console-runtime',str(provider)])]:
        command=[str(app),'--smoke-settings-raw','--shell-runtime',str(runtime),*extra]
        try:
            owned=runner.run_owned(command,timeout=60,reap_timeout=10,tail_bytes=65536,output_dir=artifacts)
            metadata=owned.metadata();raw=owned.output_tail;output=raw.decode('utf-8',errors='replace')
            verdict=assess(metadata,output,name)
        except (OSError,ValueError) as error:
            metadata={'launch_error':str(error)};raw=('SETTINGS_RAW_LAUNCH_ERROR '+str(error)+'\n').encode();verdict={'boundary_verdict':'INCONCLUSIVE','resize_reproduction':'INCONCLUSIVE','native_qualified':False,'runtime_promotion_allowed':False}
        path=artifacts/('settings-raw-'+name+'.log');path.write_bytes(raw)
        records.append({'provider':name,'runner':metadata,**verdict,'log_bytes':len(raw),'log_sha256':hashlib.sha256(raw).hexdigest()})
        (artifacts/'owned-processes.json').write_text(json.dumps({'schema':1,'scope':'two-stock-provider-authored-settings-events','identities':identities,'records':records,'native_qualified':False,'runtime_promotion_allowed':False},indent=2)+'\n',encoding='utf-8')
        print(json.dumps(records[-1],sort_keys=True),flush=True)
    # A no-resize PASS is still not a reproduction of the known resize failure.
    if any(r['boundary_verdict']=='MISMATCH' for r in records):return 1
    if any(r['boundary_verdict']!='PASS' or r['resize_reproduction']!='OBSERVED' for r in records):return 2
    return 0
if __name__=='__main__':raise SystemExit(main())
