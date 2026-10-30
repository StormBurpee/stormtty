#!/usr/bin/env python3
"""Independent proposal gates/text tests. Never launch native or candidate code."""
import copy,hashlib,importlib.util,json,tempfile,types,unittest
from pathlib import Path
from unittest.mock import patch
HERE=Path(__file__).resolve().parent

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
stage=module('independent_component_stage',HERE/'component-stage.py')
text=module('independent_component_text',HERE/'component-text.py')
fixtures=module('independent_compare_fixtures',HERE/'compare-tests.py')
ASSUMPTIONS={'os_tls_and_accessor_async_semantics':'explicit-assumption-not-general-proof',
 'reader_module_literal':'source-inferred-not-exported-byte-evidence',
 'raw_pe_hash_binding':'native-cold-extractor-plus-verified-collection',
 'native_cleanup_and_liveness':'unexecuted-before-component-stage'}

def review(authorized=False):
    return dict(schema=1,scope='proposed-15-case-component-stage',execution_authorized=authorized,
                runtime_promotion_allowed=False,native_qualified=False,assumptions=dict(ASSUMPTIONS))

def identity_review():
    identity=stage.compare.identity(fixtures.fixture(1),1)
    sources=identity['variants']['copied']['source'];r=review(True)
    r.update(reviewed_source_inventory=copy.deepcopy(sources),case_ids=list(stage.runner.matrix.STAGE_CASES),
             trial_count=len(stage.runner.matrix.trials()),pending_cases=list(stage.runner.matrix.PENDING_CASES),variants={})
    for variant in ('copied','combined'):
        r['variants'][variant]=dict(schema=2,variant=variant,approved_for_fixture_execution=True,
          runtime_handler_call_graph_reviewed=True,reader_handler_call_graph_reviewed=True,
          reader_callback_binding_reviewed=True,rendezvous_unreachable_from_handlers_reviewed=True,
          handler_export=stage.linked.HANDLER,dll_sha256=identity['variants'][variant]['dll'],
          reader_sha256=identity['variants'][variant]['reader'],controller_sha256=identity['controller']['controller_sha256'],
          cold_linkage_sha256=identity['variants'][variant]['cold_linkage_sha256'],fixture_source_sha256=sources['fixture_source_sha256'])
    return identity,r,sources

class Stage(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='component-source-tests-',dir='/workspace/shared');self.root=Path(self.temp.name)
    def tearDown(self):self.temp.cleanup()
    def saved(self,value):
        p=self.root/'review.json';data=json.dumps(value).encode();p.write_bytes(data);return p,hashlib.sha256(data).hexdigest()
    def test_proposal_default_is_not_authorization(self):
        p,h=self.saved(review())
        with self.assertRaisesRegex(ValueError,'not authorized'):stage.read_review(p,h)
        p,h=self.saved(review(True));self.assertTrue(stage.read_review(p,h)['execution_authorized'])
    def test_review_sha_scope_assumptions_and_overclaim_fail_closed(self):
        p,h=self.saved(review(True))
        for bad in ('','x'*64,'0'*64):
            with self.assertRaises(ValueError):stage.read_review(p,bad)
        for key,value in (('schema',2),('scope','other'),('assumptions',{}),('native_qualified',True),('runtime_promotion_allowed',True),('execution_authorized',1)):
            r=review(True);r[key]=value;p,h=self.saved(r)
            with self.assertRaises(ValueError):stage.read_review(p,h)
    def test_review_file_and_parent_alias_rejected(self):
        p,h=self.saved(review(True));alias=self.root/'alias.json';alias.symlink_to(p)
        with self.assertRaises(ValueError):stage.read_review(alias,h)
        parent=self.root/'alias';parent.symlink_to(self.root,target_is_directory=True)
        with self.assertRaises(ValueError):stage.read_review(parent/'review.json',h)
    def test_identity_source_prior_and_schedule_drift(self):
        identity,r,sources=identity_review();expected={'identities':stage.compare.prior_projection(identity)}
        with patch.object(stage.compare,'expected_identities',return_value=expected):
            stage.validate_identity(identity,r,sources)
            for kind in ('prior','sources','review_sources','case','pending','count','binary','reader','controller','linkage','fixture','review_flag'):
                with self.subTest(kind=kind):
                    i=copy.deepcopy(identity);v=copy.deepcopy(r);s=copy.deepcopy(sources)
                    if kind=='prior':i['controller']['controller_sha256']='2'*64
                    elif kind=='sources':s['build_source_sha256']={'other':'2'*64}
                    elif kind=='review_sources':v['reviewed_source_inventory']={}
                    elif kind=='case':v['case_ids']=v['case_ids'][:-1]
                    elif kind=='pending':v['pending_cases']=[]
                    elif kind=='count':v['trial_count']=1
                    else:
                        field={'binary':'dll_sha256','reader':'reader_sha256','controller':'controller_sha256','linkage':'cold_linkage_sha256','fixture':'fixture_source_sha256','review_flag':'reader_callback_binding_reviewed'}[kind]
                        v['variants']['combined'][field]=False if kind=='review_flag' else {}
                    with self.assertRaises(ValueError):stage.validate_identity(i,v,s)
    def test_no_launch_until_both_linked_gates_and_common_reader(self):
        for fail in ('copied','combined','readers','none'):
            with self.subTest(fail=fail),tempfile.TemporaryDirectory(dir=self.root) as d:
                root=Path(d);events=[]
                class BuildRoot:
                    def resolve(self):return self
                    def __str__(self):return 'C:/stormtty-build-review'
                    def __truediv__(self,value):return root/value
                args=types.SimpleNamespace(build_root=BuildRoot(),cold_collection=root/'cold-stage-text',review=root/'review',review_sha256='1'*64,output=root/'component-stage')
                record={'variants':{'copied':{},'combined':{}}}
                def inspect(*args):
                    variant=args[-1];events.append(variant)
                    if fail==variant:raise ValueError('synthetic linked rejection')
                def launch(command,**kwargs):
                    self.assertEqual(events,['copied','combined']);self.assertEqual(kwargs,{'check':False})
                    self.assertIn(str(root/'controller/controller.exe'),command)
                    self.assertIn(str(root/'copied-build/reader.exe'),command)
                    self.assertIn(str(root/'component-stage-trust/combined.json'),command)
                    self.assertEqual(command[1],str(HERE.parent/'run.py'))
                    return types.SimpleNamespace(returncode=2)
                digest=lambda p: 'different' if fail=='readers' and 'combined-build' in str(p) else 'same'
                with patch.object(stage.argparse.ArgumentParser,'parse_args',return_value=args),patch.object(stage,'read_review',return_value=record),patch.object(stage,'os',types.SimpleNamespace(name='nt')),patch.object(stage.compare,'collection',return_value={}),patch.object(stage.compare,'identity',return_value={}),patch.object(stage.manifest,'identities',return_value={}),patch.object(stage,'validate_identity'),patch.object(stage.manifest,'check'),patch.object(stage.Path,'read_bytes',return_value=b'same template'),patch.object(stage.runner,'fixture_review'),patch.object(stage.runner,'source_identities',return_value={}),patch.object(stage.linked,'inspect',side_effect=inspect),patch.object(stage.runner,'digest',side_effect=digest),patch.object(stage.subprocess,'run',side_effect=launch) as launched:
                    if fail=='none':self.assertEqual(stage.main(),2);launched.assert_called_once()
                    else:
                        with self.assertRaisesRegex(ValueError,'variant readers differ' if fail=='readers' else 'synthetic linked rejection'):stage.main()
                        self.assertEqual(events,['copied'] if fail=='copied' else ['copied','combined'])
                        launched.assert_not_called()
    def test_text_collector_excludes_binaries_and_decodes_trace(self):
        source=self.root/'source';source.mkdir();(source/'results.json').write_text('{}')
        trial=source/'1-raw-1-copied';trial.mkdir();(trial/'fixture.json').write_text('{}');(trial/'trace.bin').write_bytes(b'synthetic trace')
        (trial/'reader.exe').write_bytes(b'MZnot included');(source/'candidate.dll').write_bytes(b'MZnot included')
        with patch.object(text.trace,'decode',return_value={'synthetic':True}):result=text.collect(source,self.root/'out')
        paths={e['path'] for e in result['files']}
        self.assertEqual(paths,{'results.json','1-raw-1-copied/fixture.json','1-raw-1-copied/trace.json'})
        self.assertFalse(result['native_qualified']);self.assertFalse(result['runtime_promotion_allowed'])
        self.assertFalse((self.root/'out/1-raw-1-copied/trace.bin').exists())
        decoded=json.loads((self.root/'out/1-raw-1-copied/trace.json').read_text())
        self.assertEqual(decoded['raw_trace_sha256'],hashlib.sha256(b'synthetic trace').hexdigest())
    def test_text_collector_alias_and_bad_text_fail_closed(self):
        for kind in ('alias','dangling','parent','binary','nul','size','total','destination'):
            with self.subTest(kind=kind),tempfile.TemporaryDirectory(dir=self.root) as d:
                root=Path(d);source=root/'source';source.mkdir();dest=root/'out'
                file=source/'results.json';file.write_text('{}')
                if kind in ('alias','dangling'):
                    file.unlink();file.symlink_to(root/('missing' if kind=='dangling' else 'target'));(root/'target').write_text('{}')
                elif kind=='parent':
                    real=source;source=root/'alias';source.symlink_to(real,target_is_directory=True)
                elif kind=='binary':file.write_bytes(b'MZcandidate')
                elif kind=='nul':file.write_bytes(b'a\0b')
                elif kind=='size':file.write_bytes(b'x'*(text.LIMITS['results.json']+1))
                elif kind=='destination':
                    parent=root/'redirect';parent.symlink_to(source,target_is_directory=True);dest=parent/'out'
                with patch.object(text,'TOTAL',1 if kind=='total' else text.TOTAL):
                    with self.assertRaises(ValueError):text.collect(source,dest)
                self.assertFalse(dest.exists())
    def test_text_collector_invalid_trace_is_inconclusive(self):
        source=self.root/'source';trial=source/'1-raw-1-copied';trial.mkdir(parents=True);(trial/'trace.bin').write_bytes(b'not a trace')
        text.collect(source,self.root/'out');v=json.loads((self.root/'out/1-raw-1-copied/trace.json').read_text())
        self.assertEqual(v['verdict'],'INCONCLUSIVE')

if __name__=='__main__':unittest.main()
