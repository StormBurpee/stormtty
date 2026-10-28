#!/usr/bin/env python3
"""Deterministic model of a suspected upstream race; NOT a native reproduction.

Model only: no copied runtime code and no production behavior changes.
See bundled_shell_resize_investigation.md for source and evidence limitations.
"""
from collections import deque
import unittest

RESIZE = object()
COMMAND = 'stormtty_probe_size'


def interleave(*, master_runs_during_signal, consume_before_signal):
    queue = deque([RESIZE, *COMMAND])
    # The reader peeks the leading resize record while owning input_mutex.
    assert queue[0] is RESIZE
    discarded = []
    if consume_before_signal:
        discarded.append(queue.popleft())
    # send_winch_maybe releases input_mutex around signal delivery.
    if master_runs_during_signal:
        # Master drains records, removes resize, writes all ordinary keys back.
        remaining = [record for record in queue if record is not RESIZE]
        queue.clear()
        queue.extend(remaining)
    # Reader resumes using its stale peek index i=0, discarding i+1 records.
    if not consume_before_signal:
        discarded.append(queue.popleft())
    return ''.join(record for record in queue if record is not RESIZE), discarded


class ResizeRaceModel(unittest.TestCase):
    def test_current_without_interleaving_preserves_input(self):
        remaining, discarded = interleave(master_runs_during_signal=False, consume_before_signal=False)
        self.assertEqual(remaining, COMMAND)
        self.assertEqual(discarded, [RESIZE])

    def test_current_interleaving_loses_exactly_first_character(self):
        remaining, discarded = interleave(master_runs_during_signal=True, consume_before_signal=False)
        self.assertEqual(remaining, 'tormtty_probe_size')
        self.assertEqual(discarded, ['s'])

    def test_retire_record_before_unlock_preserves_input(self):
        for master in (False, True):
            with self.subTest(master=master):
                remaining, discarded = interleave(master_runs_during_signal=master, consume_before_signal=True)
                self.assertEqual(remaining, COMMAND)
                self.assertEqual(discarded, [RESIZE])


if __name__ == '__main__':
    unittest.main()
