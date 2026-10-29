#!/usr/bin/env python3
"""Fixed stock/refined full managed Session matrix; no retries or default changes."""
import argparse,importlib.util,json,os,pathlib,sys
HERE=pathlib.Path(__file__).resolve().parent
REPO=HERE.parents[2]
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module);return module
identity=load('managed_refined_identity',HERE/'refined-identities.py')
runner=load('managed_owned_runner',REPO/'tools/run-owned-process.py')

def plan():
    for provider in ('inbox','packaged'):
        for trial,order in enumerate((('stock','refined'),('refined','stock')),1):
            for variant in order: yield provider,trial,variant

def passed(result):
    return (result.exit_code==0 and result.reaped and not result.timed_out and
            not result.output_truncated and not result.cleanup_error and
            result.output_tail.splitlines().count(b'REFINED_MANAGED_RESULT fresh=1 warm=1 result=PASS')==1)

def run(args):
    roots={name:getattr(args,name).resolve() for name in ('original','stock','refined')}
    ids=identity.identities(roots)
    args.output.mkdir(parents=True,exist_ok=False)
    binary_paths={n:getattr(args,n).resolve() for n in ('host','native')}
    binary_paths.update(helper=args.host.resolve().parent/'stormtty-console-setup.exe',
                        ghostty=args.host.resolve().parent/'ghostty-vt.dll')
    binaries={n:identity.sha(path) for n,path in binary_paths.items()}
    sources=['managed-native/host.cpp','managed-native/config.zsh','managed-native/native.c','0006-refined-prefix.patch']
    report={'scope':'fixed-full-managed-session-stock-refined','default_activation_allowed':False,
            'exact_internal_path_qualified':False,'planned':8,'identities':ids,
            'sources':{n:identity.sha(HERE/n) for n in sources},
            'binaries':binaries,'cases':[]}
    for provider,trial,variant in plan():
        # Validate paired runtime provenance before every fresh invocation.
        identity.check(identity.identities(roots)==ids,'runtime identity changed during matrix')
        identity.check({n:identity.sha(path) for n,path in binary_paths.items()}==binaries,'fixture binary changed during matrix')
        name=f'{provider}-{trial}-{variant}'
        cmd=[str(args.host.resolve()),str(roots[variant]),str(args.config.resolve()),str(args.native.resolve())]
        if provider=='packaged':cmd.append(str(args.provider.resolve()))
        record={'provider':provider,'trial':trial,'variant':variant,'passed':False}
        try:
            result=runner.run_owned(cmd,timeout=90,reap_timeout=10,tail_bytes=1048576,output_dir=args.output)
            (args.output/(name+'.log')).write_bytes(result.output_tail)
            record.update(passed=passed(result),runner=result.metadata())
        except OSError:
            record['launch_error']=True
        report['cases'].append(record)
        (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
        print('REFINED_MANAGED_TRIAL '+json.dumps(record,sort_keys=True),flush=True)
    identity.check(identity.identities(roots)==ids,'runtime identity changed after matrix')
    return int(any(not x['passed'] for x in report['cases']))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('original','stock','refined','provider','output','host','native','config'):
        parser.add_argument('--'+name,type=pathlib.Path,required=True)
    args=parser.parse_args()
    if os.name!='nt':parser.error('native Windows execution only')
    identity.check(identity.sha(args.config)==identity.sha(HERE/'managed-native/config.zsh'),'observer config mismatch')
    return run(args)
if __name__=='__main__':raise SystemExit(main())
