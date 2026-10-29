"""Differential simplified stock/retirement model, not native proof.

Keys model raw line_edit; mouse payload can be disabled; geometry observation
changes once at first mouse/resize. read_limit=None means no early ready return.
A fault limit caps actual discard count; interleave removes resize during signal.
"""
from collections import deque
import unittest
R=('resize','')
M=('mouse','')
def K(x):return ('key',x)
class Escaped(Exception):pass

def run(events,variant,*,changed=True,mouse_report=False,read_limit=None,
        discard_limit=None,interleave=False,escape=False):
 q=deque(events);buf=[];discards=[];trace=[];stat='processing'
 def discard(n):
  actual=min(n,len(q),n if discard_limit is None else discard_limit)
  trace.append(('discard',n,actual))
  for _ in range(actual):discards.append(q.popleft())
 def send():
  nonlocal changed,q
  if not changed:return False
  changed=False;trace.append('signal')
  if interleave:q=deque(e for e in q if e!=R)
  if escape:raise Escaped(buf.copy(),list(q),trace.copy())
  return True
 for i,e in enumerate(events):
  if e==M:
   send()
   if mouse_report:buf.append('mouse');stat='ok'
  elif e==R:
   if variant=='early':
    discard(i+1)
    return buf,list(q),'winch' if send() else stat,trace
   if variant=='conditional' and changed:
    discard(i+1);send()
    return buf,list(q),'winch',trace
   if send():
    discard(i+1)
    return buf,list(q),'winch',trace
   continue
  elif e[0]=='key':buf.append(e[1]);stat='ok'
  if stat=='ok' and read_limit is not None and len(buf)>=read_limit:
   discard(i+1);return buf,list(q),stat,trace
 discard(len(events))
 return buf,list(q),stat,trace

class TargetedComparison(unittest.TestCase):
 def test_new_regression_original_early_patch_after_mouse(self):
  events=[M,R,K('a'),K('b')]
  stock=run(events,'stock',interleave=True)
  early=run(events,'early',interleave=True)
  conditional=run(events,'conditional',interleave=True)
  self.assertEqual(stock[:3],(['a','b'],[],'ok'))
  self.assertEqual(early[:3],([], [K('b')], 'processing'))
  self.assertEqual(conditional,stock)

 def test_original_early_patch_changes_unchanged_size_return(self):
  events=[R,K('a')]
  stock=run(events,'stock',changed=False)
  early=run(events,'early',changed=False)
  self.assertEqual(stock[2],'ok');self.assertEqual(early[2],'processing')
  self.assertEqual(run(events,'conditional',changed=False),stock)

 def test_conditional_refines_target_race(self):
  events=[R,K('\x1b'),K('[')]
  stock=run(events,'stock',interleave=True)
  candidate=run(events,'conditional',interleave=True)
  self.assertEqual(stock[1],[K('[')])
  self.assertEqual(candidate[1],[K('\x1b'),K('[')])
  self.assertEqual(stock[2],candidate[2])

 def test_partial_discard_without_interleave_same_bytes_and_status(self):
  for n in (0,1,2,3,None):
   for events in ([R,K('a')],[K('p'),R,K('a')]):
    stock=run(events,'stock',discard_limit=n)
    candidate=run(events,'conditional',discard_limit=n)
    self.assertEqual(stock[:3],candidate[:3])

 def test_ready_prefix_returns_before_resize_identically(self):
  events=[K('p'),R,K('a')]
  for limit in (1,2,None):
   stock=run(events,'stock',read_limit=limit)
   candidate=run(events,'conditional',read_limit=limit)
   self.assertEqual(stock[:3],candidate[:3])

 def test_mouse_unmodified_for_both_report_modes(self):
  for report in (False,True):
   for limit in (1,2,3,None):
    events=[M,R,K('a'),K('b')]
    stock=run(events,'stock',mouse_report=report,read_limit=limit,interleave=True)
    candidate=run(events,'conditional',mouse_report=report,read_limit=limit,interleave=True)
    self.assertEqual(stock,candidate)

 def test_handler_escape_stock_keeps_processed_prefix_candidate_retires_it(self):
  events=[K('p'),R,K('a')]
  with self.assertRaises(Escaped) as stock:
   run(events,'stock',escape=True)
  with self.assertRaises(Escaped) as candidate:
   run(events,'conditional',escape=True)
  self.assertEqual(stock.exception.args[0],['p'])
  self.assertEqual(candidate.exception.args[0],['p'])
  self.assertEqual(stock.exception.args[1],events)
  self.assertEqual(candidate.exception.args[1],[K('a')])

 def test_partial_discard_still_has_preexisting_duplicate_prefix(self):
  events=[K('p'),R,K('a')]
  for variant in ('stock','conditional'):
   first=run(events,variant,discard_limit=0)
   second=run(first[1],variant,changed=False)
   self.assertEqual(first[0]+second[0],['p','p','a'])

if __name__=='__main__':unittest.main(verbosity=2)
