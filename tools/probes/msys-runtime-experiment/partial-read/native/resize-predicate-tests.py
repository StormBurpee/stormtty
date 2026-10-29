import copy,importlib.util,pathlib,unittest
HERE=pathlib.Path(__file__).resolve().parent
s=importlib.util.spec_from_file_location('resize',HERE/'resize-predicate.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)

def rows(case='partial-zero',positive=False):
    n=64 if case=='positive-prefix' else 2;c=0 if case=='zero-copy' else 1
    out=[]
    def add(event,payload=(),peek=2,op=0,role=1,read=1):
        out.append(dict(event=event,sequence=len(out),process=1,thread=7,role=role,flags=1,read=read,peek=peek,op=op,parent=0,payload=tuple(payload)+(0,)*(8-len(payload))))
    add(1,(n,0,2),peek=0)
    if c:add(2,(0,1,1,0,1,n-1),peek=1);add(3,(1,n,2),peek=1)
    add(4,(1,0,3,n-c,1));add(5,(4,0,3,0,0,0,c));add(6,(4,0,110,30,109,30,1,1))
    add(10,(1,2),op=10);add(11,(1,2,1),op=10)
    add(12,(28,4,0),op=11);add(18,(28,1),op=11,role=3);add(19,(28,1),op=11,role=3);add(13,(28,4,0),op=11)
    add(10,(1,1),op=12);add(11,(1,1,0),op=12)
    add(14,(3,0,0,0,c));add(15,(c if positive else (1<<64)-1,c,71 if positive else 4))
    return out

def blob(rs,finalized=1,overflow=0):
    size=len(rs)+1
    b=m.parser.HEADER.pack(b'STPTH001',1,64,128,size,len(rs),overflow,1,finalized,1,0)
    for i,r in enumerate(rs):
        b+=m.parser.RECORD.pack(1,1,r['event'],i,r['process'],r['thread'],r['role'],r['flags'],r['read'],r['peek'],r['op'],r['parent'],*r['payload'])
    return b+bytes(128)

class Tests(unittest.TestCase):
    def check(self,rs,case='partial-zero',variant='stock'):
        return m.analyze(blob(rs),case,variant)
    def test_fixed_cases_expected_outcomes(self):
        for case in ('partial-zero','positive-prefix','zero-copy'):
            for variant in ('stock','copied'):
                r=self.check(rows(case,variant=='copied' and case!='zero-copy'),case,variant)
                self.assertEqual(r['verdict'],'RESIZE_OBSERVED');self.assertTrue(r['expected_return_match']);self.assertFalse(r['native_qualification'])
    def test_positive_errno_not_required_zero(self):
        self.assertTrue(self.check(rows(positive=True),variant='copied')['expected_return_match'])
    def test_observation_separate_from_expected_return(self):
        r=self.check(rows(),variant='copied');self.assertEqual(r['verdict'],'RESIZE_OBSERVED');self.assertFalse(r['expected_return_match'])
    def test_unknown_variant_not_relabelled(self):
        self.assertEqual(self.check(rows(),variant='refined')['verdict'],'INCONCLUSIVE')
    def test_later_unreturned_read_preserves_local_observation(self):
        rs=rows();r=copy.deepcopy(rs[0]);r.update(read=2,sequence=len(rs));rs.append(r)
        self.assertEqual(self.check(rs)['verdict'],'RESIZE_OBSERVED')
    def test_missing_each_essential_event(self):
        rs=rows()
        for i in range(len(rs)):
            self.assertEqual(self.check(rs[:i]+rs[i+1:])['verdict'],'INCONCLUSIVE',i)
    def test_identity_and_numeric_corruption(self):
        for event,field,value in ((18,'thread',8),(19,'op',99),(13,'peek',99),(15,'read',2),(2,'role',3),(3,'role',3),(10,'role',3),(11,'role',3),(18,'role',1),(19,'role',1),(10,'op',11)):
            rs=rows();next(r for r in rs if r['event']==event)[field]=value
            self.assertEqual(self.check(rs)['verdict'],'INCONCLUSIVE',(event,field))
        for event,lane,value in ((1,0,64),(1,2,1),(2,0,1),(2,5,0),(5,0,2),(5,1,1),(6,4,110),(6,6,0),(12,0,2),(19,1,2),(14,0,1),(15,1,0),(15,2,5),(11,2,0)):
            rs=rows();r=next(r for r in rs if r['event']==event);p=list(r['payload']);p[lane]=value;r['payload']=tuple(p)
            self.assertEqual(self.check(rs)['verdict'],'INCONCLUSIVE',(event,lane))
    def test_duplicate_signal_or_return_and_bad_order(self):
        for event in (1,5,12,13,15,18,6):
            rs=rows();rs.append(copy.deepcopy(next(r for r in rs if r['event']==event)))
            self.assertEqual(self.check(rs)['verdict'],'INCONCLUSIVE',event)
        rs=rows();i=next(i for i,r in enumerate(rs) if r['event']==18);rs[i],rs[i+1]=rs[i+1],rs[i]
        self.assertEqual(self.check(rs)['verdict'],'INCONCLUSIVE')
    def test_incomplete_invalid(self):
        for b in (b'',blob(rows(),finalized=0),blob(rows(),overflow=1)):
            self.assertEqual(m.analyze(b,'partial-zero','stock')['verdict'],'INCONCLUSIVE')
    def test_zero_copy_cannot_include_copy(self):
        rs=rows('zero-copy');r=copy.deepcopy(rows()[1]);rs.insert(1,r)
        self.assertEqual(self.check(rs,'zero-copy')['verdict'],'INCONCLUSIVE')

    def test_zero_copy_cannot_claim_read_more(self):
        rs=rows('zero-copy');r=copy.deepcopy(rows()[2]);rs.insert(1,r)
        self.assertEqual(self.check(rs,'zero-copy')['verdict'],'INCONCLUSIVE')

if __name__=='__main__':unittest.main()
