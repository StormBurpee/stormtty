"""Source-control-flow model only, not a Windows/native/runtime test.
Models the proposed both-event i-prefix retirement and exact caller status deltas.
"""
import unittest

def process(q, variant, changed=True, limit=100, remove_resize=False, mouse_report=False):
    snapshot=list(q); ready=[]; status='processing'; discards=[]
    def discard(n):
        if n: # mandatory: native discard(0) means stale default count, not no-op
            discards.append(n); del q[:n]
    for i,event in enumerate(snapshot):
        if event in ('mouse','resize') and changed:
            changed=False
            if variant in ('both','refined'): discard(i)
            if remove_resize: q[:]=[x for x in q if x!='resize']
            if variant in ('both','refined'):
                result = status if variant=='refined' and event=='mouse' else 'winch'
                return ready,q,result,discards
            if event=='resize':
                discard(i+1); return ready,q,'winch',discards
        if event == 'mouse' and mouse_report:
            ready.append('mouse-report'); status='ok'
        elif event.startswith('key:'):
            ready.append(event[4:]); status='ok'
        if len(ready)>=limit:
            discard(i+1); return ready,q,status,discards
    discard(len(snapshot)); return ready,q,status,discards

def second_read_iteration(variant):
    # first iteration consumed/copied 'a'; VMIN=2, buflen=2 => read_more
    copied=['a']
    buf,q,status,_=process(['mouse','key:b'],variant,limit=1)
    # Returning SIGWINCH handler without SA_RESTART, no pending interrupt later.
    if status=='winch': return -1,copied,q
    copied+=buf
    return len(copied),copied,q

class Review(unittest.TestCase):
    def test_refined_mouse_preserves_processing_and_fresh_queue(self):
        self.assertEqual(process(['mouse','key:b'],'refined')[:3],
                         ([],['mouse','key:b'],'processing'))
        # read()/select() retry from fresh snapshot with unchanged dimensions.
        self.assertEqual(process(['mouse','key:b'],'refined',changed=False)[:3],
                         (['b'],[],'ok'))
    def test_refined_ready_prefix_kept(self):
        self.assertEqual(process(['key:a','mouse','key:b'],'refined')[:3],
                         (['a'],['mouse','key:b'],'ok'))
    def test_refined_resize_status_preserved(self):
        self.assertEqual(process(['key:a','resize','key:b'],'refined')[:3],
                         (['a'],['resize','key:b'],'winch'))
    def test_refined_vmin_two_partial_copy_completes(self):
        copied=['a']
        buf,q,status,_=process(['mouse','key:b'],'refined',limit=1)
        self.assertEqual(status,'processing')
        self.assertEqual(buf,[])
        # read_more preserves copied_chars through input_processing retry.
        buf,q,status,_=process(q,'refined',changed=False,limit=1)
        copied+=buf
        self.assertEqual((len(copied),copied,q,status),(2,['a','b'],[],'ok'))
    def test_refined_mouse_report_once_on_fresh_peek(self):
        buf,q,status,_=process(['mouse','key:a'],'refined',mouse_report=True)
        self.assertEqual((buf,status),([],'processing'))
        buf,q,status,_=process(q,'refined',changed=False,mouse_report=True)
        self.assertEqual((buf,q,status),(['mouse-report','a'],[],'ok'))
    def test_refined_short_read_boundary(self):
        stock=process(['key:a','mouse','key:b'],'stock')
        refined=process(['key:a','mouse','key:b'],'refined')
        self.assertEqual(stock[0],['a','b'])
        self.assertEqual(refined[0],['a'])
        self.assertEqual(process(refined[1],'refined',changed=False)[0],['b'])
    def test_geometry_storm_can_defer_mouse_until_quiescent(self):
        q=['mouse','key:a']
        for _ in range(20):
            buf,q,status,_=process(q,'refined',changed=True)
            self.assertEqual((buf,status),([],'processing'))
        self.assertEqual(process(q,'refined',changed=False)[0],['a'])
    def test_new_mouse_error(self):
        self.assertEqual(process(['mouse','key:a'],'stock')[2],'ok')
        self.assertEqual(process(['mouse','key:a'],'both')[2],'winch')
    def test_new_loss_of_already_copied_byte(self):
        self.assertEqual(second_read_iteration('stock'),(2,['a','b'],[]))
        self.assertEqual(second_read_iteration('both'),(-1,['a'],['mouse','key:b']))
    def test_zero_prefix_is_not_discard_zero_call(self):
        self.assertEqual(process(['resize','key:a'],'both')[3],[])
    def test_target_race_improved(self):
        self.assertEqual(process(['resize','key:ESC'],'stock',remove_resize=True)[1],[])
        self.assertEqual(process(['resize','key:ESC'],'both',remove_resize=True)[1],['key:ESC'])
    def test_read_limit_avoids_signal_identically(self):
        for variant in ('stock','both'):
            self.assertEqual(process(['key:a','mouse','key:b'],variant,limit=1)[:3],
                             (['a'],['mouse','key:b'],'ok'))
    def test_unchanged_dimensions_equivalent(self):
        self.assertEqual(process(['mouse','resize','key:a'],'stock',changed=False),
                         process(['mouse','resize','key:a'],'both',changed=False))
if __name__=='__main__': unittest.main()
