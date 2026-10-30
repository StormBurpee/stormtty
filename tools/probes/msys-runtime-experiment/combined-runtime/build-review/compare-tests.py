#!/usr/bin/env python3
"""Synthetic evidence-schema tests, never native qualification fixtures."""
import copy,hashlib,importlib.util,json,tempfile,unittest
from pathlib import Path
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('cold_compare',HERE/'compare.py');c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
SHA='1'*64

def fixture(repeat):
    files={};snapshot={'schema':1,'fixture_source_sha256':{'reader.c':SHA},'build_source_sha256':{'build.sh':SHA}}
    def js(name,value):files[name]=(json.dumps(value)+'\n').encode()
    js('evidence/independent-build.json',{'repeat':repeat,'run_id':'1','run_attempt':'1','commit':'a'*40,'clean_root':True})
    for variant in ('copied','combined'):
        prefix=variant+'-build/provenance/'
        metadata={'tool-binaries-before.txt':''.join(SHA+'  /tool'+str(i)+'\n' for i in range(5)),
          'source-epoch.txt':'867eb7748e6e5a0e8430ce9f46af27b4b1e8f204\n1790890720\n2026-10-02T06:38:40+09:00\n',
          'build-time-environment.txt':'SOURCE_DATE_EPOCH=1790890720\n','makepkg.conf':'synthetic config\n',
          'compiler-packages.txt':'synthetic packages\n','compiler-mode.txt':'synthetic mode\n'}
        metadata['tool-binaries-after.txt']=metadata['tool-binaries-before.txt']
        metadata['cold-linkage.json']=json.dumps(dict(schema=1,scope='bounded-cold-linkage-excerpts',fixtures_executed=False,
            approved_for_fixture_execution=False,dll_sha256=SHA,reader_sha256=SHA))
        for name,value in metadata.items():files[prefix+name]=value.encode()
        js(prefix+'source-before.json',snapshot)
        js(prefix+'build-review.json',dict(schema=2,scope='cold-build-evidence-only',variant=variant,fixtures_executed=False,
          approved_for_fixture_execution=False,review_required=True,native_qualified=False,dll_sha256=SHA,reader_sha256=SHA,
          fixture_source_sha256=snapshot['fixture_source_sha256'],build_source_sha256=snapshot['build_source_sha256'],
          provenance_sha256={name:hashlib.sha256(value.encode()).hexdigest() for name,value in metadata.items()}))
    js('controller/controller-build-review.json',dict(schema=2,scope='controller-build-only',fixtures_executed=False,review_required=True,controller_sha256=SHA,object_sha256=SHA,source_sha256=SHA))
    js('controller/source-before.json',snapshot)
    for phase in ('before','after'):js('controller/tool-binaries-'+phase+'.json',[{'path':'C:/tool.exe','sha256':SHA}])
    js('controller/build-config.json',{'flags':['synthetic']})
    return files

def write(root,files):
    entries=[]
    for name,data in files.items():
        target=root/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
        entries.append({'path':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
    (root/'collection.json').write_text(json.dumps(dict(schema=2,scope='cold-build-text-only',collection_complete=True,both_build_reports_present=True,fixtures_executed=False,rejected=[],files=entries)))

def change(files,name,key,value):
    obj=json.loads(files[name]);obj[key]=value
    if name.endswith('/build-review.json') and key in ('dll_sha256','reader_sha256'):
        linked=name.replace('build-review.json','cold-linkage.json')
        body=json.loads(files[linked]);body[key]=value;files[linked]=json.dumps(body).encode()
        obj['provenance_sha256']['cold-linkage.json']=hashlib.sha256(files[linked]).hexdigest()
    files[name]=json.dumps(obj).encode()

def baseline(files=None):
    value=c.identity(fixture(1) if files is None else files,1)
    return {'origin':value['run'],'identities':c.prior_projection(value)}

class Compare(unittest.TestCase):
    def compare(self,b,after=None):
        with tempfile.TemporaryDirectory(dir=HERE) as directory:
            aroot=Path(directory)/'a';broot=Path(directory)/'b';write(aroot,fixture(1));write(broot,b)
            if after:after(broot)
            return c.compare(aroot,broot,baseline())
    def test_matching_synthetic_evidence_not_approval(self):
        result=self.compare(fixture(2));self.assertEqual(result['verdict'],'EXACT_MATCH');self.assertFalse(result['approved_for_fixture_execution']);self.assertFalse(result['native_qualified'])
    def test_binary_mismatch(self):
        for path,key in [('copied-build/provenance/build-review.json','dll_sha256'),('combined-build/provenance/build-review.json','reader_sha256'),('controller/controller-build-review.json','controller_sha256')]:
            b=fixture(2);change(b,path,key,'2'*64);self.assertEqual(self.compare(b)['verdict'],'MISMATCH')
    def test_missing_and_malformed_evidence(self):
        b=fixture(2)
        for name in list(b):
            altered=dict(b);del altered[name];self.assertEqual(self.compare(altered)['verdict'],'INCONCLUSIVE',name)
        altered=dict(b);altered['controller/controller-build-review.json']=b'{"schema":2,"schema":2}'
        self.assertEqual(self.compare(altered)['verdict'],'INCONCLUSIVE')
    def test_hash_corruption_and_alias(self):
        self.assertEqual(self.compare(fixture(2),lambda root:(root/'controller/controller-build-review.json').write_text('{}'))['verdict'],'INCONCLUSIVE')
        def alias(root):
            p=root/'controller/controller-build-review.json';p.unlink();p.symlink_to(root/'controller/source-before.json')
        self.assertEqual(self.compare(fixture(2),alias)['verdict'],'INCONCLUSIVE')
    def test_independence_and_source_mutations(self):
        for key,value in [('repeat',1),('repeat',True),('clean_root',False),('commit','b'*40),('run_id','2')]:
            b=fixture(2);change(b,'evidence/independent-build.json',key,value);self.assertEqual(self.compare(b)['verdict'],'INCONCLUSIVE')
        b=fixture(2);change(b,'controller/source-before.json','fixture_source_sha256',{'reader.c':'2'*64});self.assertEqual(self.compare(b)['verdict'],'INCONCLUSIVE')
    def test_controller_tool_or_configuration_difference(self):
        b=fixture(2)
        for phase in ('before','after'):b['controller/tool-binaries-'+phase+'.json']=json.dumps([{'path':'C:/tool.exe','sha256':'2'*64}]).encode()
        self.assertEqual(self.compare(b)['verdict'],'MISMATCH')
        b=fixture(2);change(b,'controller/build-config.json','flags',['changed']);self.assertEqual(self.compare(b)['verdict'],'MISMATCH')
    def test_standard_sha256sum_binary_marker_and_malformed_markers(self):
        def marked(repeat,marker):
            files=fixture(repeat)
            for variant in ('copied','combined'):
                prefix=variant+'-build/provenance/'
                report=json.loads(files[prefix+'build-review.json'])
                for phase in ('before','after'):
                    name='tool-binaries-'+phase+'.txt'
                    value=''.join(SHA+' '+marker+'/tool'+str(i)+'\n' for i in range(5)).encode()
                    files[prefix+name]=value
                    report['provenance_sha256'][name]=hashlib.sha256(value).hexdigest()
                files[prefix+'build-review.json']=json.dumps(report).encode()
            return files
        with tempfile.TemporaryDirectory(dir=HERE) as directory:
            a=Path(directory)/'a';b=Path(directory)/'b'
            write(a,marked(1,'*'));write(b,marked(2,'*'))
            self.assertEqual(c.compare(a,b,baseline(marked(1,'*')))['verdict'],'EXACT_MATCH')
        # Valid marker spelling is not normalized: exact original metadata must match.
        self.assertEqual(self.compare(marked(2,'*'))['verdict'],'MISMATCH')
        for marker in ('?', ''):
            self.assertEqual(self.compare(marked(2,marker))['verdict'],'INCONCLUSIVE')

    def test_same_changed_binaries_in_both_repeats_fail_prior_pin(self):
        with tempfile.TemporaryDirectory(dir=HERE) as directory:
            a=Path(directory)/'a';b=Path(directory)/'b'
            for repeat,root in ((1,a),(2,b)):
                files=fixture(repeat)
                change(files,'copied-build/provenance/build-review.json','dll_sha256','2'*64)
                write(root,files)
            result=c.compare(a,b,baseline())
            self.assertTrue(result['repeat_match']);self.assertFalse(result['prior_match'])
            self.assertEqual(result['verdict'],'MISMATCH');self.assertFalse(result['exact_match'])
    def test_new_build_source_inventory_allowed_but_fixture_drift_rejected(self):
        for mapping,want in (('build_source_sha256','EXACT_MATCH'),('fixture_source_sha256','MISMATCH')):
            with tempfile.TemporaryDirectory(dir=HERE) as directory:
                a=Path(directory)/'a';b=Path(directory)/'b'
                for repeat,root in ((1,a),(2,b)):
                    files=fixture(repeat)
                    for prefix in ('copied-build/provenance/','combined-build/provenance/','controller/'):
                        change(files,prefix+'source-before.json',mapping,{'changed':'2'*64})
                        if prefix!='controller/':change(files,prefix+'build-review.json',mapping,{'changed':'2'*64})
                    write(root,files)
                self.assertEqual(c.compare(a,b,baseline())['verdict'],want)
    def test_missing_or_changed_pinned_expected_file_fails_closed(self):
        original=c.EXPECTED_PATH
        try:
            with tempfile.TemporaryDirectory(dir=HERE) as directory:
                c.EXPECTED_PATH=Path(directory)/'expected.json'
                with self.assertRaises(OSError):c.expected_identities()
                c.EXPECTED_PATH.write_text('{}')
                with self.assertRaisesRegex(ValueError,'hash mismatch'):c.expected_identities()
        finally:c.EXPECTED_PATH=original
        expected=c.expected_identities()
        self.assertEqual(expected['origin']['run_id'],'40994103183')
        self.assertFalse(expected['approved_for_fixture_execution'])

    def test_cold_linkage_scope_and_identity_required(self):
        for key,value in (('schema',2),('scope','other'),('fixtures_executed',True),
                          ('approved_for_fixture_execution',True),('dll_sha256','2'*64),('reader_sha256','2'*64)):
            files=fixture(2);prefix='copied-build/provenance/'
            change(files,prefix+'cold-linkage.json',key,value)
            report=json.loads(files[prefix+'build-review.json'])
            report['provenance_sha256']['cold-linkage.json']=hashlib.sha256(files[prefix+'cold-linkage.json']).hexdigest()
            files[prefix+'build-review.json']=json.dumps(report).encode()
            self.assertEqual(self.compare(files)['verdict'],'INCONCLUSIVE')
    def test_identical_new_controller_config_still_fails_prior(self):
        with tempfile.TemporaryDirectory(dir=HERE) as directory:
            a=Path(directory)/'a';b=Path(directory)/'b'
            for repeat,root in ((1,a),(2,b)):
                files=fixture(repeat);change(files,'controller/build-config.json','flags',['changed'])
                write(root,files)
            result=c.compare(a,b,baseline())
            self.assertTrue(result['repeat_match']);self.assertFalse(result['prior_match'])
            self.assertEqual(result['verdict'],'MISMATCH')

    def test_crlf_and_scope(self):
        b=fixture(2)
        for name in list(b):
            if name.endswith('.json'):b[name]=b[name].replace(b'\n',b'\r\n')
        self.assertEqual(self.compare(b)['verdict'],'EXACT_MATCH')
        change(b,'copied-build/provenance/build-review.json','fixtures_executed',True)
        self.assertEqual(self.compare(b)['verdict'],'INCONCLUSIVE')
if __name__=='__main__':unittest.main()
