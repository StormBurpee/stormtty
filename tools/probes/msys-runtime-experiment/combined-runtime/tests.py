#!/usr/bin/env python3
"""Portable regressions; explicit retained source input, no network/build MSYS."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import struct
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SOURCE_ROOT = None


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result)
    return result


generator = module('combined_generator', HERE/'make-patch.py')
trace = module('combined_trace', HERE/'trace.py')
linked = module('combined_linked', HERE/'linked-gate.py')


class ExactSource(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='.source-tests-', dir=HERE)
        cls.tree = Path(cls.temp.name)
        for name in ('fhandler/console.cc', 'local_includes/fhandler.h'):
            target = cls.tree/'winsup/cygwin'/name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((SOURCE_ROOT/'winsup/cygwin'/name).read_bytes())
        cls.original = (cls.tree/'winsup/cygwin/fhandler/console.cc').read_bytes()
        cls.header = (cls.tree/'winsup/cygwin/local_includes/fhandler.h').read_bytes()
        cls.sources = {}
        for name in ('copied', 'combined'):
            patch = ROOT/('partial-read/0007-preserve-copied-on-signal.patch' if name == 'copied' else '0006-refined-prefix.patch')
            subprocess.run(['git', 'apply', '--check', str(patch)], cwd=cls.tree, check=True, capture_output=True)
            subprocess.run(['git', 'apply', str(patch)], cwd=cls.tree, check=True, capture_output=True)
            cls.sources[name] = (cls.tree/'winsup/cygwin/fhandler/console.cc').read_text()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_behavior_patches_unchanged(self):
        for path, expected in (
            ('0006-refined-prefix.patch', '1afc3483da5dcc8848f336619f858c0ffcfbef608d1446464e6566135b2aff66'),
            ('partial-read/0007-preserve-copied-on-signal.patch', '2a827b8e656975d8f34511152a664783fe48c1f6199ecd5506825af92bd7b508')):
            self.assertEqual(hashlib.sha256((ROOT/path).read_bytes()).hexdigest(), expected)

    def test_behavior_patch_order(self):
        console = self.tree/'winsup/cygwin/fhandler/console.cc'
        header = self.tree/'winsup/cygwin/local_includes/fhandler.h'
        console.write_bytes(self.original); header.write_bytes(self.header)
        for patch in ('0006-refined-prefix.patch', 'partial-read/0007-preserve-copied-on-signal.patch'):
            subprocess.run(['git', 'apply', str(ROOT/patch)], cwd=self.tree, check=True, capture_output=True)
        self.assertEqual(console.read_text(), self.sources['combined'])
        self.assertEqual(hashlib.sha256(header.read_bytes()).hexdigest(), 'ef5f8cd8db11a874807d37661a87ac0a65a9e5bad04c6f37039bd9b814a5f3d4')

    def test_exact_generation_hashes_and_apply(self):
        identity = json.loads((HERE/'identities.json').read_text())
        self.assertEqual(identity['header_sha256'], hashlib.sha256((HERE/'observer.hpp').read_bytes()).hexdigest())
        for name, source in self.sources.items():
            new, patch = generator.generate(source, name)
            self.assertEqual((new, patch), generator.generate(source, name))
            self.assertEqual(patch, (HERE/(name+'.patch')).read_text())
            pinned = identity['variants'][name]
            self.assertEqual(pinned['source_sha256'], hashlib.sha256(source.encode()).hexdigest())
            self.assertEqual(pinned['instrumented_sha256'], hashlib.sha256(new.encode()).hexdigest())
            self.assertEqual(pinned['patch_sha256'], hashlib.sha256(patch.encode()).hexdigest())
            with tempfile.TemporaryDirectory(dir=HERE) as d:
                path = Path(d)/'winsup/cygwin/fhandler/console.cc';path.parent.mkdir(parents=True);path.write_text(source)
                subprocess.run(['git', 'apply', '--check', str(HERE/(name+'.patch'))], cwd=d, check=True, capture_output=True)
                subprocess.run(['git', 'apply', str(HERE/(name+'.patch'))], cwd=d, check=True, capture_output=True)
                self.assertEqual(path.read_text(), new)
                self.assertEqual((Path(d)/'winsup/cygwin/local_includes/stormtty-combined-path.h').read_bytes(), (HERE/'observer.hpp').read_bytes())

    def test_reject_source_drift(self):
        for name, source in self.sources.items():
            with self.assertRaisesRegex(ValueError, 'unverified'):
                generator.generate(source+'\n', name)
        with self.assertRaises(ValueError):
            generator.generate(self.sources['copied'], 'unknown')

    def test_fail_closed_anchors(self):
        with self.assertRaises(ValueError):
            generator.passive.replace('duplicate duplicate', 'duplicate', 'changed')

    def test_no_added_runtime_input_or_signal_apis(self):
        for name, source in self.sources.items():
            new, _ = generator.generate(source, name)
            for token in ('ReadConsoleInputW (', 'WriteConsoleInputW (', 'PeekConsoleInputW (',
                          'kill_pgrp (SIGWINCH)', 'con.fillin (', 'acquire_input_mutex (',
                          'release_input_mutex (', 'acquire_attach_mutex (', 'release_attach_mutex ('):
                self.assertEqual(source.count(token), new.count(token), (name, token))
            self.assertEqual(source.count('cygwait (get_handle (), timeout)'), new.count('cygwait (get_handle (), timeout)'))

    def test_mouse_hook_after_attach_release_before_geometry(self):
        for name, source in self.sources.items():
            new, _ = generator.generate(source, name)
            a = new.index('fhandler_console::process_input_message (')
            b = new.index('\nvoid\nfhandler_console::discard_key_events', a)
            body = new[a:b]
            self.assertEqual(new.count('st_path::mouse_rendezvous ();'), 1)
            hook = body.index('st_path::mouse_rendezvous ();')
            self.assertLess(body.index('release_attach_mutex ();'), body.index('st_path::dispatch ('))
            self.assertLess(body.index('st_path::dispatch ('), hook)
            self.assertTrue(body[hook:].split('\n', 2)[1].lstrip().startswith(('send_winch_maybe', 'if (send_winch_maybe')))

    def test_all_return_reasons_and_ingresses(self):
        for name, source in self.sources.items():
            new, _ = generator.generate(source, name)
            self.assertEqual(re.findall(r'st_path::read_end \(buflen, copied_chars, errno, st_path::(\w+)\)', new), list(generator.REASONS))
            self.assertIn('st_path::ingress (st_path::WAIT_SIGNAL, WAIT_SIGNALED)', new)
            self.assertIn('ret == input_winch ? st_path::INPUT_WINCH : st_path::INPUT_SIGNAL', new)
            self.assertIn('st_path::restart_decision (ret, get_ttyp ()->last_sig,', new)
            self.assertIn('!!(global_sigs[get_ttyp ()->last_sig].sa_flags & SA_RESTART)', new)

    def test_cygwait_restart_contract_unchanged(self):
        for name, digest in (
            ('cygwait.cc', '921b698deb9c2fa02f5b2894e78d570e1bd35c0245458c7d9bb561ee7cd5b95f'),
            ('local_includes/cygwait.h', '22a6f8b292570ef76c17613f91f30ea0ea1be2bf9da521ca011f78df45b0d75d')):
            self.assertEqual(hashlib.sha256((SOURCE_ROOT/'winsup/cygwin'/name).read_bytes()).hexdigest(), digest)
        source = (SOURCE_ROOT/'winsup/cygwin/cygwait.cc').read_text()
        self.assertIn('else if (_my_tls.call_signal_handler () || is_cw_sig_restart)\n\t    continue;', source)


class Surface(unittest.TestCase):
    def test_no_build_and_native_launcher_requires_review(self):
        self.assertFalse((HERE/'build.sh').exists())
        runner=(HERE/'run.py').read_text()
        self.assertIn('bindings[variant]=linked.inspect',runner)
        self.assertIn('fixture_review(review,args.controller,sources)',runner)
        self.assertIn("if os.name!='nt'",runner)
        self.assertIn('matrix_adjudicator_ready=False', (HERE/'trace.py').read_text())

    def test_observer_bounded_and_private(self):
        source = (HERE/'observer.hpp').read_text()
        for forbidden in ('printf(', 'fprintf(', 'malloc(', 'new ', 'CreateThread(', 'getenv(', 'Sleep(', 'FlushConsoleInputBuffer', 'SuspendThread'):
            self.assertNotIn(forbidden, source)
        for required in ('attempt<32', 'slot>=capacity', 'visits>8', 'rendezvous_timeout_ms=1500', 'errno=posix;SetLastError(win)'):
            self.assertIn(required, source)

    def test_handler_has_no_wait_or_mutable_context_access(self):
        handler = (HERE/'observer.hpp').read_text().split('void stormtty_combined_handler_v2', 1)[1]
        for forbidden in ('WaitFor', 'SetEvent', 'st_path::context(', 'contexts[', 'GetProcAddress', 'MapViewOfFile', 'gates.'):
            self.assertNotIn(forbidden, handler)
        self.assertIn('st_path::Context local{}', handler)
        self.assertIn('__atomic_load_n(&st_path::reader_read', handler)

    def test_fixture_no_queue_mutation_or_signal_suppression(self):
        source = (HERE/'controller-protocol.h').read_text()+(HERE/'reader-protocol.h').read_text()
        for forbidden in ('ReadConsoleInput', 'WriteConsoleInput', 'FlushConsoleInput', 'SuspendThread', 'sigprocmask', 'sigfillset', 'Sleep('):
            self.assertNotIn(forbidden, source)
        self.assertIn('sigemptyset', source)
        self.assertIn('action.sa_flags=restart?SA_RESTART:0', source)


class PortableAPIDoubles(unittest.TestCase):
    def test_observer_and_controller(self):
        with tempfile.TemporaryDirectory(dir=HERE) as d:
            binary = Path(d)/'portable-test'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                '-I'+str(HERE/'tests'), str(HERE/'tests/observer-portable.cpp'), '-o', str(binary)], check=True, capture_output=True)
            for case in ('success', 'prefix', 'timeout', 'set-failed', 'wait-failed', 'disabled', 'wait', 'external', 'controller',
                         'invalid-config', 'no-read', 'wrong-index', 'master', 'zero-return'):
                with self.subTest(case=case):
                    subprocess.run([str(binary), case], check=True, capture_output=True, timeout=5)

    def test_reader_c_abi_surface(self):
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
            '-I'+str(HERE/'tests'), '-fsyntax-only', str(HERE/'tests/reader-portable.c')], check=True, capture_output=True)


class LinkedReviewContract(unittest.TestCase):
    def test_missing_or_old_native_review_rejected(self):
        for review in ({}, {'schema':1,'variant':'copied'}, {'schema':2,'variant':'combined'}):
            with self.assertRaises(ValueError):
                linked.review_contract(review,'copied')

    def test_new_review_requires_every_manual_call_graph_and_binding_obligation(self):
        review=dict(schema=2,variant='copied',handler_export=linked.HANDLER,
            approved_for_fixture_execution=True,runtime_handler_call_graph_reviewed=True,
            reader_handler_call_graph_reviewed=True,reader_callback_binding_reviewed=True,
            rendezvous_unreachable_from_handlers_reviewed=True)
        self.assertIs(linked.review_contract(review,'copied'),review)
        for key in ('approved_for_fixture_execution','runtime_handler_call_graph_reviewed',
                    'reader_handler_call_graph_reviewed','reader_callback_binding_reviewed',
                    'rendezvous_unreachable_from_handlers_reviewed'):
            changed=dict(review);changed[key]=False
            with self.subTest(key=key),self.assertRaises(ValueError):
                linked.review_contract(changed,'copied')

    def test_linked_gate_does_not_load_or_execute(self):
        source=(HERE/'linked-gate.py').read_text()
        for forbidden in ('LoadLibrary','ctypes','subprocess.run','create_task'):
            self.assertNotIn(forbidden,source)
        self.assertIn('general_async_signal_safety_proven=False',source)
        self.assertIn('fixtures_executed=False',source)
        self.assertIn('unreviewed linked handler callee',source)

    def test_shared_pe_and_linked_relocation_regressions(self):
        subprocess.run(['python3',str(ROOT/'passive-path/linked-preflight-tests.py')],check=True,capture_output=True)

    def test_reader_ordinal_and_delayed_imports_rejected(self):
        fixtures=module('pe_fixtures',ROOT/'passive-path/linked-preflight-tests.py')
        data,off=fixtures.fixture();self.assertTrue(linked.ReaderPE(bytes(data)).imports)
        struct.pack_into('<Q',data,off(0x2700),1<<63|7)
        with self.assertRaisesRegex(ValueError,'ordinal'):linked.ReaderPE(bytes(data))
        data,off=fixtures.fixture();struct.pack_into('<II',data,0x98+112+13*8,0x3000,32)
        with self.assertRaisesRegex(ValueError,'delayed'):linked.ReaderPE(bytes(data))

    def test_reader_unexpected_dll_rejected(self):
        checker=module('reader_imports',ROOT/'check-reader-imports.py')
        self.assertEqual(checker.check('DLL Name: msys-2.0.dll\nDLL Name: KERNEL32.dll'),{'msys-2.0.dll','kernel32.dll'})
        for imports in ('DLL Name: KERNEL32.dll', 'DLL Name: msys-2.0.dll\nDLL Name: user32.dll'):
            with self.assertRaises(ValueError):checker.check(imports)


def wire(events, **changes):
    values = dict(magic=b'STCMB002', version=2, hs=64, rs=128, cap=max(1, len(events)), used=len(events),
                  overflow=0, initialized=1, finalized=1, generation=99, flags=0)
    values.update(changes)
    blob = trace.HEADER.pack(*values.values())
    for i, event in enumerate(events):
        row = dict(committed=1, version=2, event=event[0], sequence=i, generation=99, thread=10,
                   role=1, flags=1, read=1, peek=0, op=0, parent=0)
        row.update(event[2] if len(event)>2 else {})
        data = list(event[1])+[0]*(8-len(event[1]))
        blob += trace.RECORD.pack(*row.values(), *data)
    blob += bytes(max(0, values['cap']-len(events))*128)
    return blob


class Trace(unittest.TestCase):
    basic = [(1, [64,0,2,0,0]), (2, [0,1,1,0,0,63]), (15, [1,1,0,8])]

    def test_complete_is_not_qualification(self):
        result = trace.observe(trace.decode(wire(self.basic)))
        self.assertTrue(result['complete']);self.assertFalse(result['native_qualified'])
        self.assertFalse(result['matrix_adjudicator_ready'])
        self.assertEqual(result['coverage_verdict'],'INCONCLUSIVE')

    def test_missing_read_return(self):
        self.assertFalse(trace.observe(trace.decode(wire(self.basic[:-1])))['complete'])

    def test_invalid_schema(self):
        for change in (dict(version=1),dict(magic=b'STPTH001'),dict(rs=64),dict(generation=0),dict(flags=1)):
            with self.subTest(change=change), self.assertRaises(trace.TraceError):
                trace.decode(wire(self.basic, **change))

    def test_overflow_unfinalized_or_context_error(self):
        for change in (dict(overflow=1), dict(finalized=0), dict(initialized=0)):
            self.assertFalse(trace.observe(trace.decode(wire(self.basic, **change)))['complete'])
        events = self.basic[:1]+[(20,[6])]+self.basic[1:]
        self.assertFalse(trace.observe(trace.decode(wire(events)))['complete'])

    def test_wait_signal_distinct_from_input_winch(self):
        for kind in (1,2):
            events = self.basic[:2]+[(21,[0xffffffff,1]), (22,[1,1]), (23,[kind,1,1])]+self.basic[2:]
            result = trace.observe(trace.decode(wire(events)))
            self.assertTrue(result['complete']);self.assertEqual(result['ingresses'][0]['kind'],kind)

    def test_gate_timeout_not_complete(self):
        for result in (258, 0xffffffff):
            events = self.basic[:1]+[(25,[1,0,1500,0],{'peek':2}),
                (26,[1,0,result,1,0,0,1],{'peek':2})]+self.basic[1:]
            self.assertFalse(trace.observe(trace.decode(wire(events)))['complete'])

    def test_gate_pair_and_sequence(self):
        events = self.basic[:1]+[(25,[1,0,1500,0],{'peek':2}),
            (26,[1,0,0,1,0,0,0],{'peek':2})]+self.basic[1:]
        self.assertTrue(trace.observe(trace.decode(wire(events)))['complete'])
        events[2] = (26,[1,0,0,1,0,0,0],{'peek':3})
        self.assertFalse(trace.observe(trace.decode(wire(events)))['complete'])

    def test_external_handler_token_and_pair(self):
        events = [(28,[10,42],{'read':0})]+self.basic[:1]+[(18,[10,1,2,42],{'role':3}),
            (19,[10,1,2,42],{'role':3})]+self.basic[1:]
        self.assertTrue(trace.observe(trace.decode(wire(events)))['complete'])
        events[3] = (19,[10,1,2,43],{'role':3})
        self.assertFalse(trace.observe(trace.decode(wire(events)))['complete'])

    def test_internal_handler_requires_active_same_operation(self):
        events = self.basic[:1]+[(12,[28,2,0],{'op':4}),
            (18,[28,1,1,0],{'role':3,'op':4}), (19,[28,1,1,0],{'role':3,'op':4}),
            (13,[28,2,0],{'op':4})]+self.basic[1:]
        self.assertTrue(trace.observe(trace.decode(wire(events)))['complete'])
        events[2] = (18,[28,1,1,0],{'role':3,'op':7})
        self.assertFalse(trace.observe(trace.decode(wire(events)))['complete'])

    def assert_incomplete(self, events):
        result = trace.observe(trace.decode(wire(events)))
        self.assertFalse(result['complete'], result)
        self.assertEqual(result['coverage_verdict'], 'INCONCLUSIVE')

    def test_operation_stacks_valid_retirement_nested_lock(self):
        operations = [(7,[1,1,1,1],{'op':4,'peek':2}),
            (10,[1,1],{'op':5,'parent':4,'peek':2}),
            (11,[1,1,0],{'op':5,'peek':2}),
            (8,[1,1,1,0,0,0],{'op':4,'peek':2}),
            (9,[1,1,1,0],{'op':4,'peek':2})]
        self.assertTrue(trace.observe(trace.decode(wire(self.basic[:1]+operations+self.basic[1:])))['complete'])
        for index, replacement in (
            (1,(10,[1,1],{'op':5,'parent':0,'peek':2})),
            (1,(10,[1,1],{'op':4,'parent':4,'peek':2})),
            (2,(11,[2,1,0],{'op':5,'peek':2})),
            (2,(11,[1,1,0],{'op':4,'peek':2})),
            (3,(8,[1,1,1],{'op':4,'peek':99})),
            (4,(9,[1,1,1,0],{'op':4,'peek':3})),
            (4,(13,[1,1,1,0],{'op':4,'peek':2}))):
            changed=list(operations);changed[index]=replacement
            with self.subTest(index=index,replacement=replacement):
                self.assert_incomplete(self.basic[:1]+changed+self.basic[1:])

    def test_unpaired_and_reused_operation_ids(self):
        for events in (
            [(10,[1,1],{'op':5})], [(11,[1,1,0],{'op':5})],
            [(7,[1,1,1,1],{'op':5})], [(9,[1,1,1,0],{'op':5})],
            [(8,[1,1,1],{'op':5})],
            [(10,[1,1],{'op':5}),(11,[1,1,0],{'op':5})]*2):
            with self.subTest(events=events):
                self.assert_incomplete(self.basic[:1]+events+self.basic[1:])

    def test_reader_and_handler_thread_ownership(self):
        for event in ((2,[0,1,1],{'thread':999}), (21,[100,0],{'thread':999})):
            self.assert_incomplete(self.basic[:1]+[event]+self.basic[1:])
        events=[(28,[10,42],{'read':0})]+self.basic[:1]+[
            (18,[10,1,2,42],{'role':3,'thread':999}),
            (19,[10,1,2,42],{'role':3,'thread':999})]+self.basic[1:]
        self.assert_incomplete(events)
        events[0]=(28,[10,42],{'read':0,'thread':999})
        self.assert_incomplete(events) # Arm and handler match, read does not.

    def test_overlapping_reads_rejected(self):
        for thread in (10,999):
            self.assert_incomplete(self.basic[:1]+[(1,[1,0,1],{'read':2,'thread':thread}),
                (15,[1,1,0,6],{'read':2,'thread':thread})]+self.basic[1:])

    def test_external_token_is_one_shot(self):
        events=[(28,[10,42],{'read':0})]+self.basic[:1]
        for count in (1,2):
            events += [(18,[10,count,2,42],{'role':3}), (19,[10,count,2,42],{'role':3})]
        self.assert_incomplete(events+self.basic[1:])

    def test_external_internal_overlap_and_cross_source_nesting(self):
        prefix=[(28,[10,42],{'read':0})]+self.basic[:1]+[(12,[28,2,0],{'op':4})]
        external=[(18,[10,1,2,42],{'role':3}), (19,[10,1,2,42],{'role':3})]
        suffix=[(13,[28,2,0],{'op':4})]+self.basic[1:]
        self.assert_incomplete(prefix+external+suffix)
        self.assert_incomplete(prefix+[(18,[28,1,1,0],{'role':3,'op':4})]+external+
            [(19,[28,1,1,0],{'role':3,'op':4})]+suffix)

    def test_missing_wait_end_and_unknown_return_reason(self):
        for events in (self.basic[:1]+[(21,[100,0])]+self.basic[1:],
                       self.basic[:-1]+[(15,[1,1,0,0])]):
            self.assertFalse(trace.observe(trace.decode(wire(events)))['complete'])


if __name__ == '__main__':
    p=argparse.ArgumentParser();p.add_argument('--source-root',required=True,type=Path)
    args, unittest_args=p.parse_known_args();SOURCE_ROOT=args.source_root
    unittest.main(argv=[__file__]+unittest_args)
