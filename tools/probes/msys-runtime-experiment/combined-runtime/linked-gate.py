#!/usr/bin/env python3
"""Read-only pre-execution gate. A NEW independent linked review is required.

This does not load a DLL, execute a reader, build, download or grant approval.
The older v1 linked review is intentionally unusable for this v2 observer.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('pe_gate', HERE.parent/'passive-path/linked-preflight.py')
pe_gate = importlib.util.module_from_spec(spec);spec.loader.exec_module(pe_gate)
HANDLER = 'stormtty_combined_handler_v2'
EXPORTS = (HANDLER, 'stormtty_combined_init_v2', 'stormtty_combined_configure_v2', 'stormtty_combined_external_arm_v2')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def bounded(path, limit):
    with path.open('rb') as stream:
        data = stream.read(limit+1)
    if len(data) > limit:
        raise ValueError('file bound: '+path.name)
    return data


def review_contract(review, variant):
    if review.get('schema') != 2 or review.get('variant') != variant:
        raise ValueError('new variant-specific v2 linked review required')
    required = ('approved_for_fixture_execution', 'runtime_handler_call_graph_reviewed',
                'reader_handler_call_graph_reviewed', 'reader_callback_binding_reviewed',
                'rendezvous_unreachable_from_handlers_reviewed')
    if any(review.get(key) is not True for key in required):
        raise ValueError('independent handler/import review pending')
    if review.get('handler_export') != HANDLER:
        raise ValueError('wrong handler review')
    return review


class ReaderPE(pe_gate.PE):
    # Executables need not export anything. All import/range checks are shared.
    def read_exports(self):
        return {}

    def read_imports(self):
        result = super().read_imports()
        nt = self.unpack('<I', 60)[0]; opt = nt+24
        if self.unpack('<I', opt+108)[0] > 13 and any(self.unpack('<II', opt+112+13*8)):
            raise ValueError('unreviewed delayed reader imports')
        rva, size = self.imports_directory
        for i in range(min(size//20, 128)):
            original, stamp, chain, name, first = struct.unpack('<IIIII', self.at(rva+i*20, 20))
            if not any((original, stamp, chain, name, first)):
                return result
            for j in range(8192):
                item = struct.unpack('<Q', self.at((original or first)+j*8, 8))[0]
                if not item:
                    break
                if item >> 63:
                    raise ValueError('unreviewed ordinal reader import')
        raise ValueError('reader import descriptor bound')


def inspect(runtime, reader, provenance, review_file, variant):
    if variant not in ('copied', 'combined'):
        raise ValueError('variant')
    review = review_contract(json.loads(bounded(review_file, 1024*1024)), variant)
    identity = json.loads((HERE/'identities.json').read_text())
    pinned = identity['variants'][variant]
    expected = {'console.cc.before': pinned['source_sha256'],
                'console.cc.after': pinned['instrumented_sha256'],
                'diagnostic.patch': pinned['patch_sha256'],
                'stormtty-combined-path.h': identity['header_sha256'],
                'fhandler.h.after': pinned['fhandler_sha256']}
    for name, digest in expected.items():
        if sha(bounded(provenance/name, 2*1024*1024)) != digest:
            raise ValueError('diagnostic source drift: '+name)
    for name in ('compiler-packages.txt', 'compiler-mode.txt', 'console-native-disassembly.txt'):
        if sha(bounded(provenance/name, 32*1024*1024)) != review['provenance_sha256'].get(name):
            raise ValueError('review provenance drift: '+name)
    dll = bounded(runtime, pe_gate.MAX_DLL); exe = bounded(reader, pe_gate.MAX_DLL)
    if sha(dll) != review['dll_sha256'] or sha(exe) != review['reader_sha256']:
        raise ValueError('review binary identity drift')
    pe = pe_gate.PE(dll); rp = ReaderPE(exe)
    for name in EXPORTS:
        pe.code_export(name)
    errno = pe.code_export('__errno')
    if pe.at(errno, len(pe_gate.ERRNO_BYTES)) != pe_gate.ERRNO_BYTES:
        raise ValueError('errno not reviewed native TLS leaf')
    parsed = pe_gate.functions(bounded(provenance/'console-native-disassembly.txt', 32*1024*1024).decode())
    for name in (HANDLER, pe_gate.EMITTER):
        if name not in parsed or sha(parsed[name][1].encode()) != review['object_body_sha256'].get(name):
            raise ValueError('independent object-body review drift: '+name)
    handler = pe.code_export(HANDLER)
    length, calls = pe_gate.linked_body(pe, handler, *parsed[HANDLER])
    targets = {target for name, target in calls if name == pe_gate.EMITTER}
    if len(targets) != 1:
        raise ValueError('handler emitter ambiguity')
    emitter = next(iter(targets))
    emit_length, emit_calls = pe_gate.linked_body(pe, emitter, *parsed[pe_gate.EMITTER])
    accessors = set()
    for name, target in calls+emit_calls:
        if name == pe_gate.EMITTER:
            if target != emitter:
                raise ValueError('wrong emitter target')
        elif name == '__errno':
            if target != errno:
                raise ValueError('wrong errno target')
        elif name in pe_gate.OS_ACCESSORS:
            pe.os_thunk(target, name);accessors.add(name)
        else:
            raise ValueError('unreviewed linked handler callee: '+name)
    if accessors != pe_gate.OS_ACCESSORS:
        raise ValueError('missing resolved handler accessor')
    imports = sorted(set(rp.imports.values()))
    if sorted(tuple(x) for x in review['reader_imports']) != imports:
        raise ValueError('reader import drift')
    dlls = {dll for dll, name in imports}
    if 'msys-2.0.dll' not in dlls or not dlls <= {'msys-2.0.dll', 'kernel32.dll', 'ntdll.dll'}:
        raise ValueError('unexpected reader imported DLL')
    callback = review['reader_handler']
    if not 1 <= callback['size'] <= 16384 or sha(rp.at(callback['rva'], callback['size'])) != callback['sha256']:
        raise ValueError('reviewed linked reader handler range drift')
    return dict(schema=2, scope='reviewed-linked-pre-execution-only', variant=variant,
        linked_gate_passed=True, fixtures_executed=False, native_qualified=False,
        general_async_signal_safety_proven=False, os_tls_contract_assumption=True,
        dll_sha256=sha(dll), reader_sha256=sha(exe), review_sha256=sha(review_file.read_bytes()),
        verified_runtime_ranges={'handler': [handler, length], 'emitter': [emitter, emit_length]},
        reader_imports=imports)


if __name__ == '__main__':
    p=argparse.ArgumentParser()
    for name in ('runtime', 'reader', 'provenance', 'review'):
        p.add_argument(name, type=Path)
    p.add_argument('variant', choices=('copied', 'combined'));a=p.parse_args()
    try:
        result=inspect(a.runtime,a.reader,a.provenance,a.review,a.variant)
    except (OSError,ValueError,KeyError,TypeError) as error:
        print(json.dumps(dict(schema=2,linked_gate_passed=False,fixtures_executed=False,
              native_qualified=False,review_required=True,reason=str(error)[:240]),indent=2))
        raise SystemExit(1)
    print(json.dumps(result,indent=2))
