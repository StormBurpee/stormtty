#!/usr/bin/env python3
"""Deterministic queue/caller model, NOT MSYS execution or a native reproduction.

Models the proposed WINDOW_BUFFER_SIZE_EVENT retirement ordering only. Cursor
cache changes, real line editing, signals, Windows I/O, mutex correctness and
scheduler timing are deliberately outside this model. Negative tests document
partial-discard and earlier mouse-unlock limitations; they are NOT fixed here.
"""
from collections import deque
from dataclasses import dataclass, field
import unittest

RESIZE = ('resize', '')
MOUSE = ('mouse', '')
def key(value):
    return ('key', value)


@dataclass
class State:
    queue: deque
    buffered: list = field(default_factory=list)
    discarded: list = field(default_factory=list)
    events: list = field(default_factory=list)

    def discard(self, count, limit=None):
        # Native discard_key_events returns void and may stop after an error or
        # zero-length ReadConsoleInputW. limit models that incomplete operation.
        self.events.append(('discard-request', count))
        available = min(count, len(self.queue), count if limit is None else limit)
        for _ in range(available):
            self.discarded.append(self.queue.popleft())


def process(state, *, retirement, changed=True, during_unlock=None,
            escape=False, discard_limit=None, mouse_unlock=None,
            key_makes_ready=False, caller='read'):
    """One simplified process_input_message call; caller selects len semantics.

    Prefix keys can be line-edit buffered without input_ready. A ready key
    returns before a later resize for select (len=0) or a satisfied read. The
    test's key_makes_ready abstracts this predicate, not all canonical editing.
    """
    if caller not in ('read', 'select'):
        raise ValueError('unknown caller')
    peek = list(state.queue)
    state.events.append(('peek', len(peek)))
    for i, record in enumerate(peek):
        if record[0] == 'key':
            state.buffered.append(record[1])
            if key_makes_ready:
                state.discard(i + 1, discard_limit)
                return 'input_ok'
        elif record == MOUSE:
            # Official console.cc:1649 calls send_winch_maybe here too. If it
            # changes size, it unlocks before the later resize case is reached.
            if mouse_unlock:
                state.events.append(('mouse-unlock', i))
                mouse_unlock(state)
        elif record == RESIZE:
            if retirement:
                state.discard(i + 1, discard_limit)
            if changed:
                state.events.append(('signal-unlock', i))
                if during_unlock:
                    during_unlock(state)
                if escape:
                    state.events.append(('handler-escape', i))
                    return 'escaped'  # Models longjmp: no return/discard after it.
            if retirement:
                # New early return even if dimensions compare equal. Caller
                # must invoke processing again for remaining ordinary input.
                return 'input_winch' if changed else 'input_processing'
            if changed:
                state.discard(i + 1, discard_limit)
                return 'input_winch'
            # Old unchanged-size branch continues over the same peek snapshot.
    state.discard(len(peek), discard_limit)
    return 'input_processing'


def master_removes_resize(state):
    # Model only the presupposed master interleaving: remove resize, preserve
    # ordinary key records. Does not assert actual master execution happened.
    state.queue = deque(r for r in state.queue if r != RESIZE)


class RetirementModel(unittest.TestCase):
    def test_old_resize_unlock_interleaving_can_discard_new_key(self):
        s = State(deque([RESIZE, key('a'), key('b')]))
        self.assertEqual(process(s, retirement=False, during_unlock=master_removes_resize), 'input_winch')
        self.assertEqual(s.discarded, [key('a')])
        self.assertEqual(list(s.queue), [key('b')])

    def test_retire_before_signal_avoids_second_discard(self):
        for interleave in (None, master_removes_resize):
            with self.subTest(interleave=bool(interleave)):
                s = State(deque([RESIZE, key('a'), key('b')]))
                self.assertEqual(process(s, retirement=True, during_unlock=interleave), 'input_winch')
                self.assertEqual(s.discarded, [RESIZE])
                self.assertEqual(list(s.queue), [key('a'), key('b')])
                self.assertEqual(sum(e[0] == 'discard-request' for e in s.events), 1)

    def test_buffered_prefix_key_not_lost_or_duplicated(self):
        for caller in ('read', 'select'):
            with self.subTest(caller=caller):
                s = State(deque([key('p'), RESIZE, key('q')]))
                self.assertEqual(process(s, retirement=True, caller=caller), 'input_winch')
                self.assertEqual(s.buffered, ['p'])
                self.assertEqual(s.discarded, [key('p'), RESIZE])
                self.assertEqual(list(s.queue), [key('q')])
                process(s, retirement=True, changed=False, caller=caller, key_makes_ready=True)
                self.assertEqual(s.buffered, ['p', 'q'])

    def test_ready_prefix_returns_before_resize_for_both_callers(self):
        for caller in ('read', 'select'):
            with self.subTest(caller=caller):
                s = State(deque([key('p'), RESIZE]))
                self.assertEqual(process(s, retirement=True, caller=caller, key_makes_ready=True), 'input_ok')
                self.assertEqual(list(s.queue), [RESIZE])
                self.assertFalse(any(e[0] == 'signal-unlock' for e in s.events))

    def test_unchanged_size_returns_early_and_caller_reprocesses_key(self):
        for caller in ('read', 'select'):
            with self.subTest(caller=caller):
                s = State(deque([RESIZE, key('q')]))
                self.assertEqual(process(s, retirement=True, changed=False, caller=caller), 'input_processing')
                self.assertEqual(list(s.queue), [key('q')])
                self.assertEqual(s.buffered, [])
                self.assertFalse(any(e[0] == 'signal-unlock' for e in s.events))
                self.assertEqual(process(s, retirement=True, changed=False, caller=caller, key_makes_ready=True), 'input_ok')
                self.assertEqual(s.buffered, ['q'])
                self.assertEqual(list(s.queue), [])

    def test_key_arriving_during_signal_unlock_survives(self):
        def arrival(state):
            master_removes_resize(state)
            state.queue.append(key('new'))
        s = State(deque([RESIZE]))
        self.assertEqual(process(s, retirement=True, during_unlock=arrival), 'input_winch')
        self.assertEqual(list(s.queue), [key('new')])
        self.assertEqual(s.discarded, [RESIZE])

    def test_signal_handler_escape_has_already_retired_resize(self):
        for retirement in (False, True):
            with self.subTest(retirement=retirement):
                s = State(deque([RESIZE, key('q')]))
                self.assertEqual(process(s, retirement=retirement, escape=True), 'escaped')
                self.assertEqual(list(s.queue), [key('q')] if retirement else [RESIZE, key('q')])
                self.assertEqual(s.events[-1][0], 'handler-escape')

    def test_partial_discard_is_not_a_success_guarantee(self):
        # Explicit limitation: void discard may leave the resize in the queue.
        s = State(deque([key('p'), RESIZE, key('q')]))
        self.assertEqual(process(s, retirement=True, discard_limit=1), 'input_winch')
        self.assertEqual(s.discarded, [key('p')])
        self.assertEqual(list(s.queue), [RESIZE, key('q')])
        self.assertEqual(s.buffered, ['p'])

    def test_zero_discard_can_leave_resize_for_duplicate_observation(self):
        s = State(deque([RESIZE, key('q')]))
        self.assertEqual(process(s, retirement=True, discard_limit=0), 'input_winch')
        self.assertEqual(list(s.queue), [RESIZE, key('q')])
        self.assertEqual(s.discarded, [])

    def test_earlier_mouse_unlock_invalidates_snapshot_unfixed(self):
        def earlier_reader(state):
            # Another reader consumes the old mouse and resize during the
            # mouse-triggered signal unlock. The current peek's i remains1.
            state.queue.popleft()
            state.queue.popleft()
        s = State(deque([MOUSE, RESIZE, key('new')]))
        process(s, retirement=True, changed=False, mouse_unlock=earlier_reader)
        self.assertEqual(s.discarded, [key('new')])
        self.assertEqual(list(s.queue), [])
        self.assertIn(('discard-request', 2), s.events)
        # This failing property is deliberately asserted: retirement at the
        # resize case alone does NOT make every earlier peek index protected.


if __name__ == '__main__':
    unittest.main()
