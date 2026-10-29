"""Portable synthetic wire-format tests, not native qualification."""
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('passive_parser', Path(__file__).with_name('parser.py'))
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)


def record(event=1, seq=0, thread=11, role=1, read=1, peek=0, op=0,
           parent=0, flags=1, committed=1, generation=9, payload=()):
    return struct.pack('<IHHQQIHHQQQQ8Q', committed, 1, event, seq,
                       generation, thread, role, flags, read, peek, op, parent,
                       *(tuple(payload) + (0,) * (8-len(payload))))


def mapping(records=(), capacity=None, reserved=None, overflow=0, finalized=1):
    capacity = max(1, len(records)) if capacity is None else capacity
    reserved = len(records) if reserved is None else reserved
    header = struct.pack('<8sIIIIQQIIQQ', b'STPTH001', 1, 64, 128, capacity,
                         reserved, overflow, 1, finalized, 9, 0)
    return header + b''.join(records) + bytes(128 * (capacity-len(records)))


def transcript(positive=False, variant='refined'):
    rows = []
    def emit(event, payload=(), **kw):
        rows.append(record(event=event, seq=len(rows), payload=payload, **kw))
    requested = 64 if positive else 2
    emit(1, (requested,0,2,0,0))
    emit(2, (0,1,1,0,0,requested-1))
    emit(3, (1,requested,2))
    # Real master read/write observations, no payload bytes.
    emit(16, (1,1,1,1,0,0,1,0), thread=12, role=2, read=0, op=20)
    emit(16, (2,1,1,1,0,0,1,1), thread=12, role=2, read=0, op=21)
    index = 1 if positive else 0
    total = 3 if positive else 2
    stat = 1 if positive else 0
    emit(4, (1,0,total,requested-1,1,0,0), peek=1)
    emit(5, (2,index,total,stat,0,index,1), peek=1)
    emit(6, (2,index,80,24,79,24,1,1), peek=1, op=2)
    def retire():
        emit(7, (1,1,1,2,2), peek=1, op=3)
        emit(8, (1,1,1,0,0,0), peek=1, op=3)
        emit(9, (1,0,2,1), peek=1, op=3)
    if positive and variant == 'refined': retire()
    emit(10, (1,2), peek=1, op=4)
    emit(11, (1,2,1), peek=1, op=4)
    emit(12, (28,2,index), peek=1, op=5)
    emit(18, (28,1), role=3, peek=1, op=5)
    emit(19, (28,1), role=3, peek=1, op=5)
    emit(13, (28,2,index), peek=1, op=5)
    emit(10, (1,1), peek=1, op=6)
    emit(11, (1,1,0), peek=1, op=6)
    if variant == 'stock':
        if positive: retire()
        else:
            emit(7, (2,2,2,2,1), peek=1, op=3)
            emit(8, (2,1,2,0,0,0), peek=1, op=3)
            emit(9, (2,0,2,0), peek=1, op=3)
        emit(14, (1,index,2,1,1), peek=1)
    else:
        emit(14, (stat,index,index,positive,1), peek=1)
        if not positive:
            emit(4, (1,0,total-index,requested-1,1,index,positive), peek=2)
    emit(2, (1,1,2,0,0,requested-2))
    emit(15, (2,2,0))
    if positive and variant == 'refined':
        emit(1, (64,0,1,0,0), read=2)
        emit(4, (1,0,2,64,1,0,0), read=2, peek=2)
        emit(2, (0,1,1,0,0,63), read=2, peek=2)
        emit(15, (1,1,0), read=2, peek=2)
    return rows


class PredicateTests(unittest.TestCase):
    def test_separate_zero_and_positive_paths(self):
        for positive in (False, True):
            case = 'positive-prefix' if positive else 'partial-zero'
            self.assertEqual(p.analyze(mapping(transcript(positive)), case)['verdict'], 'PATH_OBSERVED')
            other = 'partial-zero' if positive else 'positive-prefix'
            self.assertEqual(p.analyze(mapping(transcript(positive)), other)['verdict'], 'INCONCLUSIVE')

    def test_stock_and_refined_have_separate_rules(self):
        self.assertEqual(p.analyze(mapping(transcript(variant='stock')), variant='stock')['verdict'], 'PATH_OBSERVED')
        self.assertEqual(p.analyze(mapping(transcript(variant='stock')))['verdict'], 'INCONCLUSIVE')

    def test_each_mandatory_event_missing_is_inconclusive(self):
        rows = transcript(True)
        for event in (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,18,19):
            filtered = [r for r in rows if struct.unpack_from('<H',r,6)[0] != event]
            rewritten=[]
            for i,r in enumerate(filtered):
                b=bytearray(r);struct.pack_into('<Q',b,8,i);rewritten.append(bytes(b))
            self.assertEqual(p.analyze(mapping(rewritten),'positive-prefix')['verdict'], 'INCONCLUSIVE', event)

    def test_unreturned_later_read_blocks_full_path(self):
        rows=transcript()
        rows.append(record(1,len(rows),read=99,payload=(64,0,1)))
        self.assertEqual(p.analyze(mapping(rows))['verdict'],'INCONCLUSIVE')

    def test_complete_trace_overflow_unfinalized_still_inconclusive(self):
        for kw in ({'overflow':1}, {'finalized':0}):
            self.assertEqual(p.analyze(mapping(transcript(), **kw))['verdict'], 'INCONCLUSIVE')

    def test_unimplemented_control_case_stays_inconclusive(self):
        self.assertEqual(p.analyze(mapping(transcript()), 'resize')['verdict'], 'INCONCLUSIVE')
        self.assertEqual(p.analyze(mapping(transcript(True, 'stock')), 'positive-prefix', 'stock')['verdict'], 'INCONCLUSIVE')

    def test_unspecified_retirement_reason_requires_chronology(self):
        for variant,positive,case in (('refined',True,'positive-prefix'), ('stock',False,'partial-zero')):
            rows=transcript(positive,variant)
            for i,r in enumerate(rows):
                if struct.unpack_from('<H',r,6)[0]==7:
                    b=bytearray(r);struct.pack_into('<Q',b,96,0);rows[i]=bytes(b)
            self.assertEqual(p.analyze(mapping(rows),case,variant)['verdict'], 'PATH_OBSERVED')

    def test_positive_prefix_requires_separate_tail_read(self):
        rows=transcript(True)
        rows=[r for r in rows if struct.unpack_from('<Q',r,32)[0]!=2]
        self.assertEqual(p.analyze(mapping(rows),'positive-prefix')['verdict'], 'INCONCLUSIVE')
        # Old impossible model: copy b+c into first read although b met VMIN.
        for i,r in enumerate(rows):
            event=struct.unpack_from('<H',r,6)[0]
            b=bytearray(r)
            if event==2 and struct.unpack_from('<Q',r,64)[0]==1:
                struct.pack_into('<Q',b,72,2);struct.pack_into('<Q',b,80,3)
                struct.pack_into('<Q',b,104,61)
            if event==15:
                struct.pack_into('<Q',b,64,3);struct.pack_into('<Q',b,72,3)
            rows[i]=bytes(b)
        self.assertEqual(p.analyze(mapping(rows),'positive-prefix')['verdict'], 'INCONCLUSIVE')

    def test_reader_context_cannot_change_threads(self):
        rows=transcript()
        for i,r in enumerate(rows):
            if struct.unpack_from('<H',r,6)[0]==5:
                b=bytearray(r);struct.pack_into('<I',b,24,99);rows[i]=bytes(b)
        self.assertEqual(p.analyze(mapping(rows))['verdict'], 'INCONCLUSIVE')

    def test_bad_counts_and_handler_id_fail_closed(self):
        for event, offset, value in ((8,80,0), (9,64,0), (11,80,99),
                                     (18,48,99), (19,72,2), (5,112,0)):
            rows=transcript(True)
            for i,r in enumerate(rows):
                if struct.unpack_from('<H',r,6)[0]==event:
                    b=bytearray(r);struct.pack_into('<Q',b,offset,value);rows[i]=bytes(b);break
            self.assertEqual(p.analyze(mapping(rows),'positive-prefix')['verdict'], 'INCONCLUSIVE')


class WireTests(unittest.TestCase):
    def test_empty_is_inconclusive(self):
        self.assertEqual(p.analyze(mapping())['verdict'], 'INCONCLUSIVE')

    def test_decode_exact_layout(self):
        result = p.decode(mapping([record(payload=(64, 0, 2))]))
        self.assertEqual(result['records'][0]['payload'][:3], (64, 0, 2))
        self.assertEqual(result['records'][0]['thread'], 11)

    def test_bad_lengths_rejected(self):
        for blob in (b'', mapping()[:-1], mapping()+b'x'):
            with self.assertRaises(p.FormatError): p.decode(blob)

    def test_unknown_version_rejected(self):
        blob = bytearray(mapping()); struct.pack_into('<I', blob, 8, 2)
        with self.assertRaises(p.FormatError): p.decode(blob)

    def test_capacity_bounded(self):
        blob = bytearray(mapping()); struct.pack_into('<I', blob, 20, 16385)
        with self.assertRaises(p.FormatError): p.decode(blob)

    def test_incomplete_and_overflow_never_pass(self):
        for blob in (mapping([record(committed=0)]), mapping(overflow=1),
                     mapping(finalized=0), mapping([record(flags=3)])):
            self.assertEqual(p.analyze(blob)['verdict'], 'INCONCLUSIVE')

    def test_wrong_sequence_generation_role_thread_rejected(self):
        for kw in ({'seq':1}, {'generation':8}, {'role':0}, {'thread':0}):
            with self.assertRaises(p.FormatError): p.decode(mapping([record(**kw)]))

    def test_unknown_event_and_flags_rejected(self):
        for kw in ({'event':21}, {'flags':4}):
            with self.assertRaises(p.FormatError): p.decode(mapping([record(**kw)]))

    def test_nonzero_unused_slot_rejected(self):
        with self.assertRaises(p.FormatError):
            p.decode(mapping([record()], reserved=0))

    def test_header_fields_rejected(self):
        for offset, fmt, value in ((12, '<I', 63), (16, '<I', 127),
                                   (24, '<Q', 2), (40, '<I', 2),
                                   (44, '<I', 2), (48, '<Q', 0),
                                   (56, '<Q', 1)):
            blob = bytearray(mapping())
            struct.pack_into(fmt, blob, offset, value)
            with self.assertRaises(p.FormatError): p.decode(blob)

    def test_unpublished_garbage_is_not_interpreted(self):
        blob = bytearray(mapping([record(committed=0)]))
        blob[68:192] = bytes([255])*124
        self.assertEqual(p.analyze(blob)['reason'], 'incomplete')

    def test_context_error_is_incomplete(self):
        self.assertEqual(p.analyze(mapping([record(event=20)]))['reason'], 'incomplete')

    def test_missing_master_and_lock_evidence_cannot_pass(self):
        rows = [record(event=e, seq=i, read=1, peek=1, op=1)
                for i, e in enumerate((1, 2, 3, 4, 5, 6, 12, 18, 19, 13, 14, 15))]
        self.assertEqual(p.analyze(mapping(rows))['verdict'], 'INCONCLUSIVE')

    def test_signal_request_alone_is_not_handler(self):
        self.assertEqual(p.analyze(mapping([record(event=12, op=1)]))['verdict'],
                         'INCONCLUSIVE')

    def test_malformed_analysis_never_passes(self):
        result = p.analyze(b'bad')
        self.assertEqual(result['verdict'], 'INCONCLUSIVE')
        self.assertEqual(result['format'], 'INVALID')


class InterruptedPartialTests(unittest.TestCase):
    def rows(self):
        return [record(1,0,payload=(2,0,2,0,0)),
                record(2,1,payload=(0,1,1,0,0,1)),
                record(15,2,payload=((1<<64)-1,1,4))]

    def test_complete_interrupted_partial_is_observation_only(self):
        result=p.analyze(mapping(self.rows()))
        self.assertEqual(result['recorded_observations']['interrupted_partial_reads'],dict(count=1,max_copied=1))
        self.assertEqual(result['verdict'],'INCONCLUSIVE')
        self.assertIs(result['byte_correctness_proven'],False)

    def test_overflow_incomplete_missing_end_have_no_observation(self):
        for blob in (mapping(self.rows(),overflow=1),mapping(self.rows(),finalized=0),
                     mapping(self.rows()[:-1]),b'bad'):
            result=p.analyze(blob)
            self.assertEqual(result['recorded_observations']['interrupted_partial_reads'],dict(count=0,max_copied=0))
            self.assertEqual(result['verdict'],'INCONCLUSIVE')

    def test_wrong_errno_count_thread_and_chain_have_no_observation(self):
        for index,offset,value in ((2,80,5),(2,72,2),(1,64,1),(1,80,2),(2,24,99)):
            rows=self.rows();b=bytearray(rows[index])
            struct.pack_into('<I' if offset==24 else '<Q',b,offset,value);rows[index]=bytes(b)
            self.assertEqual(p.analyze(mapping(rows))['recorded_observations']['interrupted_partial_reads'],dict(count=0,max_copied=0))

    def test_unrelated_unclosed_read_preserves_committed_observation(self):
        rows=self.rows()+[record(1,3,read=2,payload=(2,0,2))]
        result=p.analyze(mapping(rows))
        self.assertEqual(result['recorded_observations']['interrupted_partial_reads'],dict(count=1,max_copied=1))
        self.assertEqual(result['verdict'],'INCONCLUSIVE')

    def test_complete_multiple_reads_count_without_payload(self):
        rows=self.rows()
        rows += [record(1,3,read=2,payload=(64,0,2)),
                 record(2,4,read=2,payload=(0,2,2,0,0,62)),
                 record(15,5,read=2,payload=((1<<64)-1,2,4))]
        self.assertEqual(p.analyze(mapping(rows))['recorded_observations']['interrupted_partial_reads'],dict(count=2,max_copied=2))


class ReaderSourceGuards(unittest.TestCase):
    def test_fixed_bounds_and_alarm_handler_no_logging(self):
        source=Path(__file__).with_name('reader.c').read_text()
        self.assertIn('attempt_count < 8',source)
        self.assertIn('alarm(8);',source)
        self.assertIn('setvbuf(log, NULL, _IONBF, 0)',source)
        self.assertIn('static void timeout_exit(int signal_number) { (void)signal_number; _exit(124); }',source)
        read=source.index('ssize_t n = read(')
        post=source.index('attempt_count++, requested',read)
        self.assertLess(read,post)
        self.assertNotIn('fprintf',source[source.index('static void winch'):source.index('static int decimal')])


if __name__ == '__main__': unittest.main()
