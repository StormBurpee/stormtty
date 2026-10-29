#!/usr/bin/env python3
"""Linux semantic scaffold ONLY. Never evidence of MSYS/Win32 correctness."""
import os
import pathlib
import select
import struct
import subprocess
import tempfile
import time
import unittest

HERE = pathlib.Path(__file__).resolve().parent
class BridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='bridge-', dir=os.environ.get('TMPDIR', str(HERE)))
        cls.broker = pathlib.Path(cls.tmp.name) / 'broker'
        cls.fixture = pathlib.Path(cls.tmp.name) / 'fixture'
        for src, dest in [('broker', cls.broker), ('fixture', cls.fixture)]:
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', str(HERE / (src + '.c')), '-lutil', '-o', str(dest)], check=True, timeout=30)
    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()
    def setUp(self): self.p = None; self.control = None
    def tearDown(self):
        if self.p:
            if self.p.poll() is None: self.p.kill()
            self.p.wait(timeout=4)
            for f in (self.p.stdin, self.p.stdout, self.p.stderr): f.close()
        if self.control is not None: os.close(self.control)
    def launch(self, mode='raw', executable=None):
        r, self.control = os.pipe()
        self.p = subprocess.Popen([str(self.broker), '--control-fd', str(r), '--', str(executable or self.fixture), mode], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, pass_fds=(r,))
        os.close(r)
        return self.p
    def read(self, count, timeout=4):
        result = bytearray(); until = time.monotonic() + timeout
        while len(result) < count:
            left = until - time.monotonic()
            self.assertGreater(left, 0, 'read deadline')
            self.assertTrue(select.select([self.p.stdout], [], [], left)[0], 'read readiness deadline')
            chunk = os.read(self.p.stdout.fileno(), min(65536, count - len(result)))
            if not chunk: break
            result.extend(chunk)
        return bytes(result)
    def ready(self): self.assertEqual(self.read(6), b'READY\n')
    def send(self, data): self.p.stdin.write(data); self.p.stdin.flush()
    def test_raw_vt_unicode_paste_fragmented(self):
        self.launch(); self.ready()
        data = b'\x1b[D\x1b[200~' + '中🙂e\u0301'.encode() + b'\r\n\x00\xff\x1b[201~'
        payload = struct.pack('<I', len(data)) + data
        for b in payload: self.send(bytes([b]))
        self.assertEqual(self.read(len(data)), data); self.assertEqual(self.p.wait(timeout=4), 0)
    def test_bounded_large_bidirectional(self):
        self.launch(); self.ready()
        data = bytes(range(256)) * 2048
        pending = memoryview(struct.pack('<I', len(data)) + data)
        received = bytearray(); until = time.monotonic() + 5
        os.set_blocking(self.p.stdin.fileno(), False)
        while pending or len(received) < len(data):
            left = until - time.monotonic(); self.assertGreater(left, 0)
            ready_r, ready_w, _ = select.select([self.p.stdout], [self.p.stdin] if pending else [], [], left)
            if ready_w:
                n = os.write(self.p.stdin.fileno(), pending[:4096]); pending = pending[n:]
            if ready_r:
                chunk = os.read(self.p.stdout.fileno(), 4096)
                self.assertTrue(chunk); received.extend(chunk)
                self.assertLessEqual(len(received), len(data))
        self.assertEqual(received, data); self.assertEqual(self.p.wait(timeout=4), 0)
    def test_partial_control_eof(self):
        self.launch(); self.ready(); os.write(self.control, b'ST\x01'); os.close(self.control); self.control = None
        self.assertEqual(self.p.wait(timeout=4), 125)
    def test_host_input_eof_is_not_tty_eof(self):
        self.launch(); self.ready(); self.p.stdin.close()
        self.assertEqual(self.p.wait(timeout=4), 125)
    def test_all_output_bytes(self):
        self.launch('bytes'); self.assertEqual(self.read(256), bytes(range(256))); self.assertEqual(self.p.wait(timeout=4), 0)
    def test_resize_fragmented(self):
        self.launch('resize'); self.ready()
        frame = b'ST\x01\x01' + struct.pack('<HH', 132, 43)
        for b in frame: os.write(self.control, bytes([b]))
        # Readiness is not a resize ACK; scaffold gives controller a scheduling turn.
        time.sleep(.05); self.send(b'x')
        self.assertEqual(self.read(7), b'132,43\n'); self.assertEqual(self.p.wait(timeout=4), 0)
    def test_ctrl_c(self):
        self.launch('signal'); self.ready(); self.send(b'\x03')
        self.assertEqual(self.read(4), b'INT\n'); self.assertEqual(self.p.wait(timeout=4), 42)
    def test_tty_eof(self):
        self.launch('eof'); self.ready(); self.send(b'\x04')
        self.assertEqual(self.read(4), b'EOF\n'); self.assertEqual(self.p.wait(timeout=4), 0)
    def test_invalid_grid(self):
        self.launch(); self.ready(); os.write(self.control, b'ST\x01\x01' + struct.pack('<HH', 0, 24))
        self.assertEqual(self.p.wait(timeout=4), 125)
    def test_control_eof(self):
        self.launch(); self.ready(); os.close(self.control); self.control = None
        self.assertEqual(self.p.wait(timeout=4), 125)
    def test_output_backpressure_close(self):
        self.launch('flood'); self.ready(); time.sleep(.1)
        os.write(self.control, b'ST\x01\x02\0\0\0\0')
        self.assertIn(self.p.wait(timeout=4), (0, 129, 137))
    def test_exec_failure(self):
        self.launch(executable='/nonexistent/stormtty-synthetic-fixture')
        self.assertEqual(self.p.wait(timeout=4), 127)
    def test_malformed_frame(self):
        self.launch(); self.ready(); os.write(self.control, b'NO\x01\x01\x50\0\x18\0')
        self.assertEqual(self.p.wait(timeout=4), 125)

if __name__ == '__main__': unittest.main(verbosity=2)
