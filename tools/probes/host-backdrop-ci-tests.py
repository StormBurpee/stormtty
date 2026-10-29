import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("probe_ci", Path(__file__).with_name("host-backdrop-ci.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
PASS = "Owned surface Gaussian graph: constructed=1 scalar_sweep=0,8,24,48 software=1 host_pixel_proof=0\nHost backdrop smoke: result=0 software=1 host_active=0"


class Classification(unittest.TestCase):
    def test_strict_success(self):
        self.assertEqual(module.classify(0, False, True, "", PASS), "warp-graph-lifecycle-passed")
        for text in ("", PASS.replace("constructed=1", "constructed=0"), PASS.replace("software=1", "software=0")):
            self.assertEqual(module.classify(0, False, True, "", text), "failed")

    def test_recovery_requires_every_transition(self):
        recovery = "Opaque child recovery: injected=1 child=1 resized=1 restored=1 result=0 physical_blur_verified=0"
        self.assertEqual(module.classify(0, False, True, "", recovery, True), "opaque-child-recovery-passed")
        for field in ("child", "resized", "restored", "injected"):
            self.assertEqual(module.classify(0, False, True, "", recovery.replace(field+"=1", field+"=0"), True), "failed")
        self.assertEqual(module.classify(0, False, True, "", PASS, True), "failed")
        for code, timeout, reaped, cleanup in ((1, False, True, ""), (0, True, True, ""), (0, False, False, ""), (0, False, True, "cleanup")):
            self.assertEqual(module.classify(code, timeout, reaped, cleanup, recovery, True), "failed")

    def test_unsupported_is_not_pass(self):
        self.assertEqual(module.classify(3, False, True, "", "PROBE_RUNTIME_UNSUPPORTED=1"), "runtime-unsupported")
        for code, text in ((1, "PROBE_RUNTIME_UNSUPPORTED=1"), (3, ""), (3, "PROBE_RUNTIME_UNSUPPORTED=0")):
            self.assertEqual(module.classify(code, False, True, "", text), "failed")

    def test_timeout_cleanup_and_cancellation_fail(self):
        for code, timeout, reaped, cleanup in ((0, True, True, ""), (3, True, True, ""), (0, False, False, ""), (0, False, True, "kill failed"), (2, False, True, "")):
            self.assertEqual(module.classify(code, timeout, reaped, cleanup, PASS+"PROBE_RUNTIME_UNSUPPORTED=1"), "failed")


if __name__ == "__main__":
    unittest.main()
