#!/usr/bin/env python3
"""Portable safety check for the opt-in trace workflow; never dispatches a job."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
VARIANTS = ('original', 'control', 'instrumented')


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)

    def test_workflow_gates_evidence_not_assertion_failures(self):
        workflow = HERE.parents[2] / '.github/workflows/msys-resize-trace.yml'
        text = workflow.read_text()
        section = text.split('      - name: Separate assertion failures', 1)[1].split('      - name:', 1)[0]
        code = '\n'.join(line[10:] for line in section.split('        run: |\n', 1)[1].splitlines())
        compile(code, str(workflow), 'exec')
        output = self.root / 'github-output'
        evidence = self.root / 'evidence/idle/results.json'
        evidence.parent.mkdir(parents=True)
        rows = [dict(variant=variant, provider=provider, workload='idle', exit_code=1,
                     trace_error='', input_evidence_available=True, reaped=True,
                     timed_out=False, cleanup_error='')
                for variant in VARIANTS for provider in ('inbox', 'packaged')]
        for field, value, expected in [(None, None, 0), ('trace_error', 'missing identity', 1),
                                       ('timed_out', True, 1), ('input_evidence_available', False, 1)]:
            changed = [dict(row) for row in rows]
            if field:
                changed[0][field] = value
            evidence.write_text(json.dumps(changed))
            output.write_text('')
            result = subprocess.run([__import__('sys').executable, '-c', code],
                                    env={**os.environ, 'EXPERIMENT_ROOT': str(self.root), 'GITHUB_OUTPUT': str(output)},
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, expected, result.stderr)
            self.assertEqual(output.read_text(), 'valid=' + ('true' if expected == 0 else 'false') + '\n')
        self.assertIn("branches: ['experiment/msys-resize-trace']", text)
        self.assertNotIn('continue-on-error:', text)
        upload = text.split('          path: |', 2)[-1]
        self.assertNotIn('**', upload)
        self.assertNotIn('*.dll', upload)
        self.assertNotIn('/evidence/idle/*.log', upload)
        self.assertIn('/resize-trace-phase-*/*.log', upload)


if __name__ == '__main__':
    unittest.main()
