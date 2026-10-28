#!/usr/bin/env python3
"""Synthetic checker mechanics only, not a Windows build or runtime test."""
import contextlib
import io
import pathlib
import tempfile
import unittest
from check_input_evidence_isolation import check


class IsolationChecks(unittest.TestCase):
    def run_case(self, prefix, suffix, contamination='', missing_marker=False):
        with tempfile.TemporaryDirectory() as directory:
            build = pathlib.Path(directory)
            def archives(qualification):
                return ' '.join(prefix + 'stormtty_' + qualification + name + suffix
                                for name in ('session', 'process'))
            (build / 'build.ninja').write_text(
                'build bundled_shell_probe.exe: LINKER ' + archives('qualification_') + '\n' +
                'build stormtty.exe: LINKER ' + archives('') + ' ' + contamination + '\n')
            (build / 'stormtty.exe').write_bytes(b'normal artifact')
            # Deliberately omit C++ class/type names, as optimized MSVC may do.
            (build / 'bundled_shell_probe.exe').write_bytes(
                b'PTY_INPUT_EVIDENCE' if missing_marker else b'PTY_INPUT_EVIDENCE PTY_WRITE')
            with contextlib.redirect_stdout(io.StringIO()):
                check(build)

    def test_gnu(self):
        self.run_case('lib', '.a')

    def test_msvc(self):
        self.run_case('', '.lib')

    def test_msvc_contamination_rejected(self):
        with self.assertRaises(AssertionError):
            self.run_case('', '.lib', 'stormtty_qualification_process.lib')

    def test_gnu_contamination_rejected(self):
        with self.assertRaises(AssertionError):
            self.run_case('lib', '.a', 'libstormtty_qualification_session.a')

    def test_missing_format_rejected(self):
        with self.assertRaises(AssertionError):
            self.run_case('', '.lib', missing_marker=True)


if __name__ == '__main__':
    unittest.main()
