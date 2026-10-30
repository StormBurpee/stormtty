"""Proposed 15-case stage gate. No launch without separately hash-bound authorization."""
import argparse,hashlib,importlib.util,json,os,re,stat,subprocess,sys
from pathlib import Path
HERE=Path(__file__).resolve().parent

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);sys.modules[name]=m;spec.loader.exec_module(m);return m
compare=module('component_compare',HERE/'compare.py')
manifest=module('component_manifest',HERE/'source-manifest.py')
linked=module('component_linked',HERE.parent/'linked-gate.py')
runner=module('component_runner',HERE.parent/'run.py')

def read_review(path,expected_sha):
    if not re.fullmatch('[0-9a-f]{64}',expected_sha):raise ValueError('explicit review SHA256 required')
    if any(p.is_symlink() or getattr(p.lstat(),'st_file_attributes',0)&0x400 for p in (path,*path.parents)):raise ValueError('review alias')
    data=compare.read(path)
    if len(data)>1024*1024 or hashlib.sha256(data).hexdigest()!=expected_sha:raise ValueError('review identity mismatch')
    value=compare.read_json(data)
    if value.get('schema')!=1 or value.get('scope')!='proposed-15-case-component-stage':raise ValueError('review scope')
    # A checked-in proposal is never authorization. Root's separate decision must
    # explicitly replace this field and publish that record's fixed digest data.
    if value.get('execution_authorized') is not True:raise ValueError('component execution not authorized')
    if value.get('runtime_promotion_allowed') is not False or value.get('native_qualified') is not False:raise ValueError('review overclaims qualification')
    if value.get('assumptions')!={
        'os_tls_and_accessor_async_semantics':'explicit-assumption-not-general-proof',
        'reader_module_literal':'source-inferred-not-exported-byte-evidence',
        'raw_pe_hash_binding':'native-cold-extractor-plus-verified-collection',
        'native_cleanup_and_liveness':'unexecuted-before-component-stage'}:raise ValueError('review assumptions missing')
    return value

def validate_identity(identity,review,current_sources):
    if compare.prior_projection(identity)!=compare.expected_identities()['identities']:raise ValueError('immutable prior identity mismatch')
    if identity['variants']['copied']['source']!=current_sources:raise ValueError('current source snapshot mismatch')
    if review.get('reviewed_source_inventory')!=current_sources:raise ValueError('reviewed executable/source dependency drift')
    if review.get('case_ids')!=list(runner.matrix.STAGE_CASES):
        raise ValueError('stage case scope mismatch')
    if review.get('trial_count')!=len(list(runner.matrix.trials())):raise ValueError('fixed schedule mismatch')
    if review.get('pending_cases')!=list(runner.matrix.PENDING_CASES):raise ValueError('pending matrix scope mismatch')
    for variant in ('copied','combined'):
        value=review['variants'][variant]
        linked.review_contract(value,variant)
        if value['dll_sha256']!=identity['variants'][variant]['dll'] or value['reader_sha256']!=identity['variants'][variant]['reader']:raise ValueError('review binary drift')
        if value['controller_sha256']!=identity['controller']['controller_sha256']:raise ValueError('review controller drift')
        if value['cold_linkage_sha256']!=identity['variants'][variant]['cold_linkage_sha256']:raise ValueError('review linkage evidence drift')
        if value['fixture_source_sha256']!=current_sources['fixture_source_sha256']:raise ValueError('review fixture drift')

def main():
    p=argparse.ArgumentParser()
    for name in ('build-root','cold-collection','review','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--review-sha256',required=True);a=p.parse_args()
    review=read_review(a.review,a.review_sha256)
    if os.name!='nt':raise ValueError('native Windows stage only')
    root=a.build_root.resolve()
    if str(root).replace('\\','/').lower()!='c:/stormtty-build-review':raise ValueError('canonical build root required')
    for path,relative in ((a.output,'component-stage'),(a.cold_collection,'cold-stage-text')):
        for candidate in (path,*path.parents):
            try:info=candidate.lstat()
            except FileNotFoundError:continue
            if stat.S_ISLNK(info.st_mode) or getattr(info,'st_file_attributes',0)&0x400:raise ValueError('stage path alias')
        if path.resolve()!=root/relative:raise ValueError('canonical stage path required')
    identity=compare.identity(compare.collection(a.cold_collection),1)
    current=manifest.identities();validate_identity(identity,review,current)
    for variant in ('copied','combined'):
        manifest.check(root/(variant+'-build/provenance/source-before.json'))
    manifest.check(root/'controller/source-before.json')
    # Bind the actual workflow as well as its recursively inventoried template.
    template=HERE/'component-stage.workflow.yml.template'
    active=HERE.parents[4]/'.github/workflows/msys-component-stage-review.yml'
    if active.read_bytes()!=template.read_bytes():raise ValueError('active workflow/template mismatch')
    if a.output.exists():raise ValueError('new stage output required')
    trust=root/'component-stage-trust';trust.mkdir(exist_ok=False)
    for variant in ('copied','combined'):
        target=trust/(variant+'.json');target.write_text(json.dumps(review['variants'][variant],sort_keys=True)+'\n')
        runner.fixture_review(review['variants'][variant],root/'controller/controller.exe',runner.source_identities())
        linked.inspect(root/(variant+'-build/experimental-msys-2.0.dll'),root/(variant+'-build/reader.exe'),root/(variant+'-build/provenance'),target,variant)
    # Both readers must be exactly the pinned common reader before choosing one.
    if runner.digest(root/'copied-build/reader.exe')!=runner.digest(root/'combined-build/reader.exe'):raise ValueError('variant readers differ')
    manifest.check(root/'controller/source-before.json')
    command=[sys.executable,str(HERE.parent/'run.py'),'--controller',str(root/'controller/controller.exe'),
             '--reader',str(root/'copied-build/reader.exe'),'--output',str(a.output)]
    for variant in ('copied','combined'):
        command += ['--'+variant,str(root/(variant+'-build/experimental-msys-2.0.dll')),
                    '--'+variant+'-provenance',str(root/(variant+'-build/provenance')),
                    '--'+variant+'-review',str(trust/(variant+'.json'))]
    # run.py owns bounded job creation/reaping for every one of the fixed trials.
    result=subprocess.run(command,check=False)
    manifest.check(root/'controller/source-before.json')
    return result.returncode # Preserve2=full-matrix-inconclusive; never turn it green.
if __name__=='__main__':raise SystemExit(main())
