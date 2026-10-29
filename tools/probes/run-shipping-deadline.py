#!/usr/bin/env python3
"""One production-ABI baseline/candidate run per provider; preserve every result."""
import argparse,importlib.util,json,pathlib,re,sys
spec=importlib.util.spec_from_file_location('owned',pathlib.Path(__file__).resolve().parents[1]/'run-owned-process.py')
owned=importlib.util.module_from_spec(spec);sys.modules[spec.name]=owned;spec.loader.exec_module(owned)
FIELDS=('requested','available','waits','timers','notified','spurious','failures','overshoot_total_us','overshoot_max_us')
DEADLINE=re.compile('SHIPPING_DEADLINE'+''.join(' '+k+r'=([0-9]+)' for k in FIELDS))
TYPING=re.compile(r'MANAGED_TYPING_RESULT code=0 characters=25 frames=[0-9]+ retreats=[0-9]+ echo_total_us=[0-9]+ echo_max_us=[0-9]+')
STATE=re.compile(r'MANAGED_STATE_RESULT code=0 phase=11 cold_samples=1 warm_samples=1 checks=[0-9]+ cold_ms=[0-9]+ warm_ms=[0-9]+')
def validate(lines,candidate):
    ds=[DEADLINE.fullmatch(l) for l in lines if DEADLINE.fullmatch(l)]
    if len(ds)!=1 or sum(bool(TYPING.fullmatch(l)) for l in lines)!=1 or sum(bool(STATE.fullmatch(l)) for l in lines)!=1:return False
    d=dict(zip(FIELDS,map(int,ds[0].groups())))
    if candidate:return d['requested']==1 and d['available']==1 and d['waits']>0 and d['timers']>0 and d['spurious']==d['failures']==0
    return all(v==0 for v in d.values())
def main():
    p=argparse.ArgumentParser()
    for key in ('baseline','candidate','runtime','provider','output'):p.add_argument('--'+key,type=pathlib.Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);results={}
    for provider,extra in [('inbox',[]),('packaged',[str(a.provider.resolve())])]:
        for mode in ('baseline','candidate'):
            name=provider+'-'+mode
            try:
                r=owned.run_owned([str((getattr(a,mode)/'managed_shell_state_probe.exe').resolve()),str(a.runtime.resolve()),*extra],timeout=120,reap_timeout=10,tail_bytes=1024*1024,output_dir=a.output)
                lines=[l for l in r.output_tail.decode('utf-8',errors='replace').splitlines() if DEADLINE.fullmatch(l) or TYPING.fullmatch(l) or STATE.fullmatch(l)]
                ok=r.exit_code==0 and r.reaped and not r.timed_out and not r.output_truncated and not r.cleanup_error and validate(lines,mode=='candidate')
                metadata={'exit_code':r.exit_code,'reaped':r.reaped,'timed_out':r.timed_out,'truncated':r.output_truncated,'cleanup_failed':bool(r.cleanup_error),'valid':ok}
            except OSError:
                lines=[];ok=False;metadata={'launch_failed':True,'valid':False}
            (a.output/(name+'.log')).write_text('\n'.join(lines)+'\n'+json.dumps(metadata,sort_keys=True)+'\n')
            results[name]=metadata;print(name+' valid='+str(int(ok)),flush=True)
    (a.output/'comparison.json').write_text(json.dumps({'production_abi':True,'publication_policy_ms':8,'physical_display_verified':False,'retries':0,'results':results},indent=2)+'\n')
    return int(not all(r['valid'] for r in results.values()))
if __name__=='__main__':raise SystemExit(main())
