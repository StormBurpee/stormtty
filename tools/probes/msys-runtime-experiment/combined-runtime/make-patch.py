#!/usr/bin/env python3
"""Exact A/B diagnostic source generation only; never downloads or builds."""
import argparse
import difflib
import hashlib
import importlib.util
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('passive_patch', HERE.parent/'passive-path/make-patch.py')
passive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(passive)
SOURCES = {
    'copied': 'cf4eba2150f2b99dfe7c55c8d73ac0f578d7675e649b8468a364bfb8fe2efdb2',
    'combined': '4d32b6219c1241a9fea998909ea8451799bf0c4f57d0eb004f88b9b9e9a57e66',
}
# Stable, exact-source return sites, in lexical order. Kept distinct from ingress.
REASONS = ('BACKGROUND', 'TIMEOUT_COPIED', 'TIMEOUT_EMPTY', 'NONBLOCK_COPIED',
           'NONBLOCK_EMPTY', 'COMPLETE', 'ERROR_RETURN', 'SIGNAL_COPIED', 'SIGNAL_EMPTY')


def instrument(source):
    s = passive.instrument(source)
    s = passive.replace(s, '"stormtty-passive-path.h"', '"stormtty-combined-path.h"')
    a = s.index('void\nfhandler_console::read (')
    b = s.index('\nfhandler_console::input_states\nfhandler_console::process_input_message', a)
    t = s[a:b]
    t = passive.replace(t, 'switch (cygwait (get_handle (), timeout))',
        'switch (st_path::read_wait (timeout, [&] { return cygwait (get_handle (), timeout); }))')
    t = passive.replace(t, 'case WAIT_SIGNALED:\n\t  goto sig_exit;',
        'case WAIT_SIGNALED:\n          st_path::ingress (st_path::WAIT_SIGNAL, WAIT_SIGNALED);\n\t  goto sig_exit;')
    t = passive.replace(t, 'case input_winch:\n\t  release_input_mutex ();',
        'case input_winch:\n          st_path::ingress (ret == input_winch ? st_path::INPUT_WINCH : st_path::INPUT_SIGNAL, ret);\n\t  release_input_mutex ();')
    t = passive.replace(t, 'if (global_sigs[get_ttyp ()->last_sig].sa_flags & SA_RESTART)',
        'if (st_path::restart_decision (ret, get_ttyp ()->last_sig,\n                !!(global_sigs[get_ttyp ()->last_sig].sa_flags & SA_RESTART)))')
    token = 'st_path::read_end (buflen, copied_chars, errno)'
    if t.count(token) != len(REASONS):
        raise ValueError('read return sites changed')
    for reason in REASONS:
        t = t.replace(token, 'st_path::read_end (buflen, copied_chars, errno, st_path::'+reason+')', 1)
    s = s[:a]+t+s[b:]
    # This switch is in process_input_message ONLY. No hook in master or select.
    a = s.index('fhandler_console::process_input_message (')
    b = s.index('\nvoid\nfhandler_console::discard_key_events', a)
    t = s[a:b]
    t = passive.replace(t, 'con.use_mouse, con_ra.ralen);',
        'con.use_mouse, con_ra.ralen, con.use_focus);')
    t = passive.replace(t, '\tcase MOUSE_EVENT:\n',
        '\tcase MOUSE_EVENT:\n          st_path::mouse_rendezvous ();\n')
    s = s[:a]+t+s[b:]
    return s


def generate(source, variant):
    if variant not in SOURCES or hashlib.sha256(source.encode()).hexdigest() != SOURCES[variant]:
        raise ValueError('unverified exact A/B source')
    new = instrument(source)
    patch = ''.join(difflib.unified_diff(source.splitlines(True), new.splitlines(True),
        fromfile='a/winsup/cygwin/fhandler/console.cc', tofile='b/winsup/cygwin/fhandler/console.cc'))
    patch += ''.join(difflib.unified_diff([], (HERE/'observer.hpp').read_text().splitlines(True),
        fromfile='/dev/null', tofile='b/winsup/cygwin/local_includes/stormtty-combined-path.h'))
    return new, patch


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('source', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('variant', choices=tuple(SOURCES))
    args = p.parse_args()
    new, patch = generate(args.source.read_text(), args.variant)
    args.output.mkdir(exist_ok=False)
    (args.output/'console.cc').write_text(new)
    (args.output/'diagnostic.patch').write_text(patch)
    identity = dict(schema=2, scope='perturbed-scheduling-diagnostic-only', variant=args.variant,
        source_sha256=SOURCES[args.variant], instrumented_sha256=hashlib.sha256(new.encode()).hexdigest(),
        patch_sha256=hashlib.sha256(patch.encode()).hexdigest(),
        header_sha256=hashlib.sha256((HERE/'observer.hpp').read_bytes()).hexdigest(),
        native_qualified=False, linked_gate_passed=False)
    (args.output/'identity.json').write_text(json.dumps(identity, indent=2)+'\n')
