#!/usr/bin/env python3
"""Portable synthetic evidence tests, never native execution or qualification."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
HERE=Path(__file__).resolve().parent

def load(name,file):
    spec=importlib.util.spec_from_file_location(name,HERE/file);m=importlib.util.module_from_spec(spec);sys.modules[name]=m;spec.loader.exec_module(m);return m
ad=load('stage_ad','adjudicate.py');runner=load('stage_run','run.py');matrix=load('stage_matrix','matrix.py');collect=load('stage_collect','collect.py')
base=load('stage_wire','tests.py')


def evidence():
    fixture=dict(schema=2,stage='console-records',case='raw-64',generation=99,
        input_batches=[dict(requested=2,accepted=2,records=[dict(type=1,unicode=x,down=True,repeat=1,virtual_key=0,scan_code=0,controls=0) for x in (97,98)])],
        initial_left=0,initial_top=0,initial_width=80,initial_height=24,buffer_width=120,buffer_height=900,initial_input_mode=0x98,initial_output_mode=3,queue_before=0,queue_remaining=0,queue_remaining_valid=True,child_exit=0,timed_out=False,reaped=True,
        assigned=True,resumed=True,cleanup_errors=0,fixture_error=0,all_writers_stopped=True,trace_written=True,gates=[],scenario_ok=True,
        reader=dict(input_mode=0x98,output_mode=3,configured=1,finished=1,result=0,canonical=0,vmin=1,vtime=0,mouse=0,focus=0,termios_restored=1,
                    select_called=0,select_result=0,select_error=0,reads=[dict(count=2,requested=64,errno=0)],output_hex='6162'))
    run=dict(reaped=True,timed_out=False,output_truncated=False,cleanup_error='',returncode=0)
    events=[(1,[64,0,1,0,0]),(2,[0,2,2,0,0,62]),(15,[2,2,0,6])]
    return fixture,run,events


def assess(fixture,run,events):
    return ad.assess('raw-64','combined',99,fixture,run,ad.trace.decode(base.wire(events)))


def mouse_events(prefix=False):
    index=int(prefix);peek={'peek':2}
    rows=[(1,[64,0,1,0,0]),(4,[1,0,2+index,64,1,0,0],peek),
          (5,[2,index,2+index,0,0,0,0,0],peek),
          (25,[1,index,1500,0],peek),(26,[1,index,0,1,0,0,0],peek),
          (6,[2,index,80,24,79,24,1,1],peek)]
    if prefix:
        rows += [(7,[1,1,1,1],dict(peek=2,op=3)),(8,[1,1,1,0,0,0],dict(peek=2,op=3)),
                 (9,[1,1,1,0],dict(peek=2,op=3))]
    rows += [(10,[1,2],dict(peek=2,op=4)),(11,[1,2,1,0,0],dict(peek=2,op=4)),
        (12,[28,2,index],dict(peek=2,op=5)),(18,[28,1,1,0],dict(peek=2,op=5,role=3)),
        (19,[28,1,1,0],dict(peek=2,op=5,role=3)),(13,[28,2,index],dict(peek=2,op=5)),
        (14,[0,index,0,0,0],peek),(4,[1,0,2,64,1,0,0],dict(peek=6)),
        (5,[2,0,2,0,0,0,0,0],dict(peek=6)),(6,[2,0,79,24,79,24,0,1],dict(peek=6)),
        (7,[2,2,2,2],dict(peek=6,op=7)),(8,[2,1,2,0,0,0],dict(peek=6,op=7)),
        (9,[2,2,2,0],dict(peek=6,op=7)),(2,[0,1+index,1+index,0,0,63-index],dict(peek=6)),
        (15,[1+index,1+index,0,6],dict(peek=6))]
    return rows


class Stage(unittest.TestCase):
    def test_positive_control_is_not_full_qualification(self):
        result=assess(*evidence());self.assertEqual(result['component_verdict'],'PASS');self.assertFalse(result['native_qualified'])
        rows=[dict(order=o,case=c,variant=v,component_verdict='PASS',byte_verdict='PASS',mechanism_verdict='PASS') for o,c,v in matrix.trials()]
        summary=matrix.aggregate(rows);self.assertEqual(summary['stage_verdict'],'PASS');self.assertEqual(summary['full_matrix_verdict'],'INCONCLUSIVE');self.assertTrue(summary['pending_cases'])

    def test_changed_mouse_and_prefix_mechanism_controls(self):
        for prefix in (False,True):
            case='prefix-mouse-off' if prefix else 'changed-mouse-off'
            events=mouse_events(prefix)
            observed=ad.path_observations(case,'combined',ad.trace.decode(base.wire(events)))
            self.assertEqual(observed['coverage_reasons'],[])
            self.assertEqual(observed['mechanism_reasons'],[])
            # The copied baseline is not required to retire before unlock.
            baseline=[r for r in events if not (prefix and len(r)>2 and r[2].get('op')==3)]
            self.assertEqual(ad.path_observations(case,'copied',ad.trace.decode(base.wire(baseline)))['mechanism_reasons'],[])
            if prefix:
                broken=ad.path_observations(case,'combined',ad.trace.decode(base.wire(baseline)))
                self.assertIn('positive prefix not retired exactly once',broken['mechanism_reasons'])
            # Matching bytes cannot cover a missing actual handler or redispatch.
            for event in (18,5):
                broken=[r for r in events if not (r[0]==event and (event==18 or r[2].get('peek')==6))]
                result=ad.path_observations(case,'combined',ad.trace.decode(base.wire(broken)))
                self.assertTrue(result['coverage_reasons'])

    def test_controller_and_trace_geometry_must_match(self):
        f,r,_=evidence();f['case']='changed-mouse-off';f['reader']['output_hex']='62';f['reader']['reads'][0]['count']=1
        f.update(initial_left=0,initial_top=0,initial_width=80,initial_height=24)
        f['input_batches']=[dict(requested=2,accepted=2,records=[dict(type=2,x=1,y=1,flags=4,buttons=120<<16,controls=0),
            dict(type=1,unicode=98,down=True,repeat=1,virtual_key=0,scan_code=0,controls=0)])]
        f['gates']=[dict(visit=1,wait_result=0,wait_error=0,geometry_error=0,release_error=0,arrived=1,queried=1,applied=1,released=1,
                        width_before=80,width_after=79,height_before=24,height_after=24)]
        decoded=ad.trace.decode(base.wire(mouse_events()))
        self.assertEqual(ad.assess(f['case'],'combined',99,f,r,decoded)['component_verdict'],'PASS')
        f['gates'][0].update(width_before=90,width_after=89)
        self.assertIn('controller/trace geometry chain differs',ad.assess(f['case'],'combined',99,f,r,decoded)['coverage_reasons'])
        f['gates'][0].update(width_before=80,width_after=79);f['initial_width']=81
        self.assertIn('controller/trace geometry chain differs',ad.assess(f['case'],'combined',99,f,r,decoded)['coverage_reasons'])

    def test_stable_control_rejects_changed_geometry(self):
        result=ad.path_observations('stable-mouse-off','combined',ad.trace.decode(base.wire(mouse_events())))
        self.assertIn('unexpected changed geometry or signal in stable control',result['coverage_reasons'])

    def test_lifecycle_failure_cannot_pass(self):
        for name,value in dict(reaped=False,timed_out=True,cleanup_errors=1,all_writers_stopped=False,trace_written=False,assigned=False,resumed=False,queue_remaining_valid=False).items():
            f,r,e=evidence();f[name]=value
            with self.subTest(name=name):self.assertEqual(assess(f,r,e)['component_verdict'],'INCONCLUSIVE')
        for name,value in dict(reaped=False,timed_out=True,output_truncated=True,cleanup_error='kill failed',returncode=None).items():
            f,r,e=evidence();r[name]=value
            with self.subTest(name=name):self.assertEqual(assess(f,r,e)['component_verdict'],'INCONCLUSIVE')

    def test_exact_byte_failure_retained(self):
        f,r,e=evidence();f['reader']['output_hex']='6261';f['scenario_ok']=False;r['returncode']=1
        result=assess(f,r,e);self.assertEqual(result['byte_verdict'],'FAIL');self.assertEqual(result['component_verdict'],'FAIL')
        f,r,e=evidence();f['queue_remaining']=1;f['scenario_ok']=False;r['returncode']=1
        self.assertEqual(assess(f,r,e)['byte_verdict'],'FAIL')

    def test_malformed_reader_is_inconclusive_not_exception(self):
        for malformed in (1,[1],'reader',None):
            f,r,e=evidence();f['reader']=malformed
            result=assess(f,r,e)
            self.assertEqual(result['component_verdict'],'INCONCLUSIVE')
            self.assertIn('reader is not object',result['coverage_reasons'])

    def test_boolean_cannot_replace_numeric_observation(self):
        for field in ('cleanup_errors','queue_before'):
            f,r,e=evidence();f[field]=False
            self.assertEqual(assess(f,r,e)['component_verdict'],'INCONCLUSIVE')
        f,r,e=evidence();f['reader']['configured']=True
        self.assertEqual(assess(f,r,e)['component_verdict'],'INCONCLUSIVE')
        f,r,e=evidence();r['returncode']=False
        self.assertEqual(assess(f,r,e)['component_verdict'],'INCONCLUSIVE')

    def test_input_inventory_is_exact(self):
        for change in ('short','extra','split','wrong-key','duplicate'):
            f,r,e=evidence();batch=f['input_batches'][0]
            if change=='short':batch['accepted']=1
            elif change=='extra':batch['records'].append(dict(type=1,unicode=99,down=True,repeat=1,virtual_key=0,scan_code=0,controls=0))
            elif change=='split':f['input_batches']=[dict(requested=1,accepted=1,records=[record]) for record in batch['records']]
            elif change=='wrong-key':batch['records'][0]['unicode']=27
            else:f['input_batches'].append(copy.deepcopy(batch))
            self.assertEqual(assess(f,r,e)['component_verdict'],'INCONCLUSIVE')

    def test_missing_or_inconsistent_observations(self):
        for name,value in dict(configured=0,finished=0,termios_restored=0,vmin=2,canonical=1,select_called=1).items():
            f,r,e=evidence();f['reader'][name]=value
            with self.subTest(name=name):self.assertEqual(assess(f,r,e)['component_verdict'],'INCONCLUSIVE')
        f,r,e=evidence();f['reader']['reads'][0]['count']=1
        self.assertEqual(assess(f,r,e)['component_verdict'],'INCONCLUSIVE')
        f,r,e=evidence();e[-1]=(15,[1,1,0,6])
        self.assertEqual(assess(f,r,e)['component_verdict'],'INCONCLUSIVE')

    def test_trace_identity_and_missing_return(self):
        f,r,e=evidence();f['generation']=100
        self.assertEqual(assess(f,r,e)['component_verdict'],'INCONCLUSIVE')
        self.assertEqual(assess(*evidence()[:2],e[:-1])['component_verdict'],'INCONCLUSIVE')

    def test_unwitnessed_mismatch_does_not_establish_stage_failure(self):
        for field,value in (('input',None),('timed_out',True)):
            f,r,e=evidence();f['reader']['output_hex']='6261';f['scenario_ok']=False;r['returncode']=1
            if field=='input':f['input_batches'][0]['accepted']=1
            else:f[field]=value
            row=assess(f,r,e);row.update(order=1,case='raw-1',variant='copied')
            self.assertEqual(row['byte_verdict'],'FAIL')
            self.assertEqual(row['component_verdict'],'INCONCLUSIVE')
            self.assertEqual(matrix.aggregate([row])['stage_verdict'],'INCONCLUSIVE')

    def test_failed_unlock_and_reused_gate_not_coverage(self):
        events=mouse_events()
        broken=[(11,[1,2,0,1,5],r[2]) if r[0]==11 else r for r in events]
        result=ad.path_observations('changed-mouse-off','combined',ad.trace.decode(base.wire(broken)))
        self.assertIn('input unlock failed or missing',result['coverage_reasons'])
        change=next(r for r in events if r[0]==6 and r[1][6]==1)
        at=events.index(change)
        broken=events[:at]+[change]+events[at:]
        result=ad.path_observations('changed-mouse-off','combined',ad.trace.decode(base.wire(broken)))
        self.assertIn('gate/query chain reused or unordered',result['coverage_reasons'])
        broken=list(events);item=broken.pop(at);broken.insert(2,item)
        result=ad.path_observations('changed-mouse-off','combined',ad.trace.decode(base.wire(broken)))
        self.assertTrue(result['coverage_reasons'])

    def test_fixed_opposing_orders_and_no_retry(self):
        calls=[];saves=[]
        def execute(n,o,c,v):
            calls.append((o,c,v));return dict(component_verdict='INCONCLUSIVE' if n==3 else 'FAIL',byte_verdict='FAIL')
        rows=runner.execute_schedule(execute,lambda rows:saves.append(len(rows)))
        self.assertEqual(calls,matrix.trials()[:3]);self.assertEqual(saves,[1,2,3]);self.assertEqual(len(rows),3)
        self.assertEqual(matrix.ORDERS,(('copied','combined'),('combined','copied')))
        self.assertEqual(len(matrix.trials()),60)
        rows.reverse();self.assertFalse(matrix.aggregate(rows)['schedule_valid'])

    def test_json_crlf_and_mutations(self):
        with tempfile.TemporaryDirectory(dir=HERE) as d:
            p=Path(d)/'result.json';p.write_bytes(b'{\r\n "schema": 2\r\n}\r\n')
            self.assertEqual(ad.bounded_json(p),{'schema':2})
            for blob in (b'{"a":1,"a":2}',b'{"x":NaN}',b'{}{}',b'[]',b'x'*65537):
                p.write_bytes(blob)
                with self.assertRaises(ValueError):ad.bounded_json(p)

    def test_fresh_controller_review_and_source_mutation_gate(self):
        with tempfile.TemporaryDirectory(dir=HERE) as d:
            controller=Path(d)/'controller.exe';controller.write_bytes(b'not a binary; hash test only')
            sources={'test':'a'}
            review=dict(controller_job_ownership_reviewed=True,controller_input_schedule_reviewed=True,
                controller_sha256=runner.digest(controller),fixture_source_sha256=sources)
            runner.fixture_review(review,controller,sources)
            for key in ('controller_job_ownership_reviewed','controller_input_schedule_reviewed'):
                bad=dict(review);bad[key]=False
                with self.assertRaises(ValueError):runner.fixture_review(bad,controller,sources)
            with self.assertRaises(ValueError):runner.fixture_review(review,controller,{'test':'b'})
            controller.write_bytes(b'mutated')
            with self.assertRaises(ValueError):runner.fixture_review(review,controller,sources)

    def test_collector_excludes_binaries_and_rejects_links(self):
        with tempfile.TemporaryDirectory(dir=HERE) as d:
            root=Path(d);source=root/'source';source.mkdir();(source/'results.json').write_text('{}')
            private=source/'private-fixtures';private.mkdir();(private/'msys-2.0.dll').write_bytes(b'private')
            run=source/'1-raw-1-copied';run.mkdir();(run/'fixture.json').write_text('{}');(run/'reader.exe').write_bytes(b'private')
            out=root/'out';manifest=collect.collect(source,out)
            self.assertEqual({x['path'] for x in manifest},{'results.json','1-raw-1-copied/fixture.json'})
            (run/'trace.bin').symlink_to(private/'msys-2.0.dll')
            with self.assertRaises(ValueError):collect.collect(source,root/'bad')

if __name__=='__main__':unittest.main()
