#!/usr/bin/env python3
"""Local POSIX probe mechanics; not native Windows candidate qualification."""
import pathlib
import re
import subprocess
import sys

probe, shell, output = (pathlib.Path(x).resolve() for x in sys.argv[1:])
output.mkdir()
fixture = pathlib.Path(__file__).with_name('bundled_shell_fixture.zsh').read_text()
pattern = re.compile(r'^RESIZE_CASE_SUMMARY code=([0-8]) phase=([0-9]+) expected_cols=([0-9]+) expected_rows=([0-9]+) observed_cols=([0-9]+) observed_rows=([0-9]+) substage=([0-9]+) protocol_step=([0-9]+) reason=([0-9]+) write_result=([0-9]+) expected_prompt_sequence=([0-9]+) prompt_status_known=([0-9]+) prompt_status=([0-9]+) child_exit_known=([0-9]+) child_exit_code=([0-9]+) resize_accepted=([0-9]+)$', re.M)


def run(name, extra='', *, expected=0, repeats=50, normal=False):
    source = output / (name + '.zsh')
    source.write_text(fixture + extra)
    options = ['--resize-repeats', str(repeats), '--resize-summary']
    if not normal:
        options += ['--resize-one-shot']
    result = subprocess.run([str(probe), str(shell), str(source), str(output / name), *options], capture_output=True, text=True, timeout=90)
    log = result.stdout + result.stderr
    (output / (name + '.log')).write_text(log)
    assert result.returncode == expected, (name, result.returncode, log[-2000:])
    matches = pattern.findall(result.stdout)
    assert len(matches) == 1, (name, matches)
    numeric = tuple(map(int, matches[0]))
    assert all(0 <= value <= 32767 for value in numeric[:10]), numeric
    assert 0 <= numeric[10] <= 0xffffffff and numeric[11] in (0,1) and 0 <= numeric[12] <= 255
    assert numeric[13] in (0,1) and 0 <= numeric[14] <= 0xffffffff and numeric[15] in (0,1)
    if expected == 0:
        assert numeric[6:15] == (0,)*9 and numeric[15] == 1, numeric
    assert (numeric[0] == 0) == (expected == 0), numeric
    return log, numeric[:6]


# These functions represent every external-job route in the ordinary workload.
# If any is called in builtin-only mode, the case fails visibly.
tripwire = '\n' + '\n'.join(f'{name}() {{ print EXTERNAL_JOB_FORBIDDEN; return 71 }}' for name in (
    'stormtty_probe_native', 'stormtty_probe_native_size', 'stormtty_probe_wait',
    'stormtty_probe_resize_diagnostic')) + '\n'
log, summary = run('oneshot-50', tripwire)
assert summary == (0, 50, 100, 30, 100, 30), summary
assert log.count('text=stormtty_probe_size\n') == 50
assert '[resize phase 50] PASS' in log
assert 'EXTERNAL_JOB_FORBIDDEN' not in log
assert 'text=stormtty_probe_native' not in log
assert 'text=stormtty_probe_journal_' not in log
assert 'text=stormtty_probe_wait' not in log
log, summary = run('idle', tripwire, repeats=1)
assert summary == (0, 1, 111, 35, 111, 35), summary
assert log.count('text=stormtty_probe_size\n') == 1
log, summary = run('stale-grid', tripwire + '\nstormtty_probe_size() { stormtty_probe_emit SIZE:100x30; stormtty_probe_emit "SIZE_ACK:${stormtty_probe_sequence}:100x30" }\n', expected=1)
assert summary == (4, 1, 111, 35, 100, 30), summary
assert log.count('text=stormtty_probe_size\n') == 1
assert log.index('RESIZE_CASE_SUMMARY') < log.index('Original resize failure frame')
assert 'text=stormtty_probe_resize_diagnostic' not in log
log, summary = run('stale-sequence', '\nstormtty_probe_size() { stormtty_probe_emit SIZE:111x35; stormtty_probe_emit SIZE_ACK:0:111x35 }\n', expected=1)
assert summary == (4, 1, 111, 35, 0, 0), summary
# Out-of-range/unparseable ACK payload never enters the whitelisted summary.
for label, grid in [('oversized-grid', '32768x35'), ('overflow-grid', '999999999999999999999x35')]:
    log, summary = run(label, '\nstormtty_probe_size() { stormtty_probe_emit SIZE:111x35; stormtty_probe_emit "SIZE_ACK:${stormtty_probe_sequence}:'+grid+'" }\n', expected=1)
    assert summary == (4, 1, 111, 35, 0, 0), summary
log, summary = run('command-failed', '\nstormtty_probe_size() { return 19 }\n', expected=1)
assert summary == (3, 1, 111, 35, 0, 0), summary
assert log.count('text=stormtty_probe_size\n') == 1
# Numeric attribution is captured at the original failing operation, before
# diagnostics can issue more commands or overwrite the protocol state.
def detail(log):
    return tuple(map(int, pattern.findall(log)[0]))[6:]
assert detail(log) == (2, 4, 3, 0, 2, 1, 19, 0, 0, 1), detail(log)
log, summary = run('journal-reset-failed', '\nstormtty_probe_journal_reset() { return 19 }\n', expected=1, normal=True)
assert summary == (3, 1, 111, 35, 0, 0), summary
assert detail(log)[:4] == (1, 4, 3, 0) and detail(log)[5:7] == (1, 19), detail(log)
assert detail(log)[-1] == 0 # Failed before request_resize, not a resize observation.
log, summary = run('size-child-exit', '\nstormtty_probe_size() { exit 23 }\n', expected=1)
assert summary == (3, 1, 111, 35, 0, 0), summary
assert detail(log) == (2, 4, 1, 0, 2, 0, 0, 1, 23, 1), detail(log)
log, summary = run('normal', repeats=2, normal=True)
assert summary == (0, 2, 100, 30, 100, 30), summary
assert 'text=stormtty_probe_native_size' in log
assert 'text=stormtty_probe_wait first' in log
# An early setup error has no phase or grid and no free-form summary fields.
result = subprocess.run([str(probe), str(shell), str(output/'idle.zsh'), str(output/'idle'),
                         '--resize-summary', '--resize-one-shot', '--resize-repeats', '1'],
                        capture_output=True, text=True, timeout=5)
assert result.returncode == 1
assert pattern.findall(result.stdout) == [('1',) + ('0',)*15]
for options in (['--resize-one-shot'], ['--resize-one-shot', '--resize-one-shot', '--resize-repeats', '1'],
                ['--resize-summary', '--resize-summary'], ['--resize-one-shot', '--resize-trace-idle', '--resize-repeats', '1']):
    result = subprocess.run([str(probe), str(shell), str(output/'idle.zsh'), str(output/'invalid'), *options], capture_output=True, timeout=5)
    assert result.returncode == 2, options
    assert not (output/'invalid').exists()
# Extract the actual telemetry/protocol structs into a host-only mock test.
# This exercises rejected writes and absent prior readiness deterministically,
# without adding shell jobs, sleeps, runtime retries or test-only probe flags.
cpp_source = pathlib.Path(__file__).with_name('bundled_shell_probe.cpp').read_text()
summary_source = cpp_source[cpp_source.index('struct ResizeSummary {'):cpp_source.index('template<class F> bool until')]
protocol_source = cpp_source[cpp_source.index('struct PromptProtocol {'):cpp_source.index('void dump_journal')]
mock = output / 'protocol-telemetry.cpp'
mock.write_text(r'''#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <iostream>
#include <cassert>
enum class SessionSendResult { accepted, would_block, closed, too_large };
enum class Key { enter };
struct KeyInput { Key key{}; };
struct Session {
    std::string frame;
    SessionSendResult text_result=SessionSendResult::accepted, key_result=SessionSendResult::accepted;
    bool throw_text=false;
    struct Info { std::string error; std::optional<std::uint32_t> exit_code; } state;
    Info info() { return state; }
    SessionSendResult text(std::string) { if(throw_text) throw std::runtime_error("synthetic"); return text_result; }
    SessionSendResult key(KeyInput) { return key_result; }
};
std::string screen(const std::shared_ptr<Session>& s) { return s->frame; }
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
template<class F> bool until(F predicate) { return predicate(); }
void expect(const std::shared_ptr<Session>& s,const std::string& token) { require(s->frame.find(token)!=std::string::npos,"missing"); }
void command(const std::shared_ptr<Session>&,std::string) {}
''' + summary_source + protocol_source + r'''
int main() {
    auto s=std::make_shared<Session>(); ResizeSummary summary; summary.enabled=true;
    PromptProtocol p{s}; p.summary=&summary;
    auto fails=[&](auto f) { bool threw=false; try { f(); } catch(...) { threw=true; } assert(threw); };
    fails([&]{p.send("synthetic");});
    assert(summary.protocol_step==1 && summary.reason==1 && summary.expected_prompt_sequence==1);
    s->frame="__STORMTTY_PROMPT:1:19__"; s->text_result=SessionSendResult::would_block;
    fails([&]{p.send("synthetic");});
    assert(summary.protocol_step==2 && summary.reason==2 && summary.write_result==2);
    s->text_result=SessionSendResult::accepted; s->key_result=SessionSendResult::closed;
    fails([&]{p.send("synthetic");});
    assert(summary.protocol_step==3 && summary.reason==2 && summary.write_result==3);
    s->key_result=SessionSendResult::accepted; p.send("synthetic");
    assert(p.sequence==2 && summary.protocol_step==0); // Prior readiness accepts nonzero status.
    s->frame="__STORMTTY_PROMPT:1:19__";
    fails([&]{p.ready(0);});
    assert(summary.protocol_step==4 && summary.reason==1 && summary.expected_prompt_sequence==2 &&
           summary.prompt_status_known==0 && summary.prompt_status==0); // Earlier sequence is not evidence.
    s->frame="__STORMTTY_PROMPT:2:19__";
    fails([&]{p.ready(0);});
    assert(summary.protocol_step==4 && summary.reason==3 && summary.prompt_status_known==1 && summary.prompt_status==19);
    s->frame="__STORMTTY_PROMPT:1:0__"; p.sequence=1; s->throw_text=true;
    fails([&]{p.send("synthetic");});
    assert(summary.protocol_step==2 && summary.reason==4 && summary.write_result==0);
    s->state.exit_code=0xffffffff; summary.emit(s);
    assert(summary.child_exit_known==1 && summary.child_exit_code==0xffffffff);
}
''')
compiled = output / 'protocol-telemetry'
subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', str(mock), '-o', str(compiled)], check=True)
subprocess.run([str(compiled)], check=True, capture_output=True, text=True)
print('PASS one-shot builtin-only 50 phases, one-shot stale/command failure, bounded stage/prompt/status/exit summaries, unchanged normal workload, CLI exclusivity')
