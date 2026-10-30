#!/usr/bin/env python3
"""Reviewed native stage only. Fixed opposing orders; never retry failed trials.

No build or download. Must have fresh independent binary/controller review.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
HERE=Path(__file__).resolve().parent

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);sys.modules[name]=m;spec.loader.exec_module(m);return m
matrix=module('combined_matrix_run',HERE/'matrix.py')
adjudicate=module('combined_adjudicate_run',HERE/'adjudicate.py')
linked=module('combined_linked_run',HERE/'linked-gate.py')
owned=module('combined_owned_run',HERE.parents[2]/'run-owned-process.py')
SOURCE_NAMES=('linked-gate.py','../passive-path/linked-preflight.py','matrix.py','adjudicate.py','run.py','collect.py','trace.py','observer.hpp','identities.json',
    'controller-protocol.h','reader-protocol.h','native/cases.h','native/reader.c','native/controller.c')
TIMEOUT_SECONDS=25
REAP_SECONDS=5


def digest(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()


def source_identities():
    result={name:digest(HERE/name) for name in SOURCE_NAMES}
    result['owned-runner.py']=digest(HERE.parents[2]/'run-owned-process.py')
    return result


def fixture_review(review,controller,sources):
    if review.get('controller_job_ownership_reviewed') is not True or review.get('controller_input_schedule_reviewed') is not True:
        raise ValueError('independent native controller review missing')
    if review.get('controller_sha256')!=digest(controller) or review.get('fixture_source_sha256')!=sources:
        raise ValueError('controller/source review drift')


def environment(inherited,fixture):
    roots={v for k,v in inherited.items() if k.casefold()=='systemroot'}
    if len(roots)!=1 or not next(iter(roots)):raise ValueError('missing/ambiguous system root')
    # Do not inherit shell/terminal runtime overrides into the private fixtures.
    return {'SystemRoot':next(iter(roots)),'PATH':str(fixture)+os.pathsep+str(Path(next(iter(roots)))/'System32')}


def execute_schedule(execute,save):
    """No repeats. Infrastructure/coverage gap stops the remaining fixed plan.

    A witnessed behavioral failure is retained and the planned opposing variant
    still runs, to compare mechanisms. No retry or adaptive ordering occurs.
    """
    rows=[]
    for number,(order,case,variant) in enumerate(matrix.trials(),1):
        row=execute(number,order,case,variant)
        row.update(order=order,case=case,variant=variant)
        rows.append(row);save(rows)
        if row.get('component_verdict')=='INCONCLUSIVE':break
    return rows


def main():
    parser=argparse.ArgumentParser()
    for name in ('controller','reader','copied','combined','copied-provenance','combined-provenance','copied-review','combined-review','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    if os.name!='nt':raise SystemExit('native Windows execution only; no build or download')
    sources=source_identities();bindings={};reviews={}
    # Validate BOTH variants before creating a child or even staging a DLL.
    for variant in ('copied','combined'):
        review_path=getattr(args,variant+'_review');review=adjudicate.bounded_json(review_path,1024*1024)
        fixture_review(review,args.controller,sources)
        bindings[variant]=linked.inspect(getattr(args,variant),args.reader,getattr(args,variant+'_provenance'),review_path,variant)
        reviews[variant]=review
    output=args.output.absolute()
    if not str(output).isascii():raise ValueError('private ANSI fixture paths must be ASCII')
    output.mkdir(exist_ok=False)
    private=output/'private-fixtures';private.mkdir()
    approved_controller_hashes={review['controller_sha256'] for review in reviews.values()}
    if len(approved_controller_hashes)!=1:raise ValueError('variant controller reviews disagree')
    controller=private/'controller.exe';shutil.copyfile(args.controller,controller)
    approved_controller_hash=next(iter(approved_controller_hashes))
    if digest(controller)!=approved_controller_hash:raise ValueError('staged controller identity drift')
    snapshots={controller:approved_controller_hash}
    staged={}
    for variant in ('copied','combined'):
        directory=private/variant;directory.mkdir()
        shutil.copyfile(args.reader,directory/'reader.exe');shutil.copyfile(getattr(args,variant),directory/'msys-2.0.dll')
        expected={directory/'reader.exe':bindings[variant]['reader_sha256'],directory/'msys-2.0.dll':bindings[variant]['dll_sha256']}
        if any(digest(p)!=h for p,h in expected.items()):raise ValueError('staged identity drift')
        snapshots.update(expected);staged[variant]=directory
    def unchanged():
        if source_identities()!=sources or any(digest(p)!=h for p,h in snapshots.items()):
            raise ValueError('fixture mutation before/after trial')
    report=dict(schema=2,scope='isolated-console-records-stage',bindings=bindings,sources=sources,trials=[],
                native_qualified=False,runtime_promotion_allowed=False)
    def save(rows):
        report['trials']=rows;report['summary']=matrix.aggregate(rows)
        (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    save([])
    def execute(number,order,case,variant):
        run=output/f'{order}-{case}-{variant}';run.mkdir()
        row=dict(component_verdict='INCONCLUSIVE',byte_verdict='INCONCLUSIVE',mechanism_verdict='INCONCLUSIVE')
        try:
            unchanged()
            command=[str(controller),str(staged[variant]/'reader.exe'),case,str(run/'trace.bin'),str(run/'fixture.json'),str(number)]
            outcome=owned.run_owned(command,timeout=TIMEOUT_SECONDS,reap_timeout=REAP_SECONDS,tail_bytes=65536,
                                    output_dir=run,env=environment(os.environ,staged[variant]))
            row['runner']=outcome.metadata();(run/'launcher.log').write_bytes(outcome.output_tail)
            unchanged()
            fixture=adjudicate.bounded_json(run/'fixture.json')
            with (run/'trace.bin').open('rb') as stream:blob=stream.read(adjudicate.trace.MAX_BYTES+1)
            decoded=adjudicate.trace.decode(blob)
            row.update(adjudicate.assess(case,variant,number,fixture,row['runner'],decoded))
            row['trace_sha256']=hashlib.sha256(blob).hexdigest();row['fixture_sha256']=digest(run/'fixture.json')
        except (OSError,ValueError,KeyError,TypeError) as error:
            row['component_verdict']='INCONCLUSIVE';row['error']=str(error)[:240]
        (run/'assessment.json').write_text(json.dumps(row,indent=2)+'\n')
        return row
    rows=execute_schedule(execute,save)
    return 1 if report['summary']['stage_verdict']=='FAIL' else 2 # Full matrix intentionally pending.

if __name__=='__main__':raise SystemExit(main())
