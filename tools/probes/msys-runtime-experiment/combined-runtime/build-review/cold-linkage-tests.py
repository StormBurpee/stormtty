#!/usr/bin/env python3
"""Synthetic cold-linkage byte tests. No native code, loader or execution approval."""
import hashlib,importlib.util,struct,unittest
from pathlib import Path
HERE=Path(__file__).resolve().parent

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result

cold=module('cold_linkage_tests_target',HERE/'cold-linkage.py')
gate=module('cold_linkage_pe',HERE.parent.parent/'passive-path/linked-preflight.py')
ACCESSORS=('GetLastError','SetLastError','GetCurrentThreadId')
EXPORTS=('stormtty_combined_handler_v2','stormtty_combined_init_v2',
         'stormtty_combined_configure_v2','stormtty_combined_external_arm_v2')

def put(pe,address,fmt,*values):
    data=struct.pack(fmt,*values);pe.data[address:address+len(data)]=data

def string(pe,address,text):
    data=text.encode('ascii')+b'\0';pe.data[address:address+len(data)]=data

def fixture():
    # Use the production PE range/string/import-thunk methods over synthetic
    # mapped sections; this is not a mock that silently accepts invalid RVAs.
    pe=object.__new__(gate.PE);pe.data=bytearray(0x3000);pe.base=0x180000000
    pe.sections=[(b'.text',0,0,0x1800,0),(b'.rdata',0,0x1800,0x1800,0x1800)]
    pe.imports_directory=(0x100,40);pe.export=(0x800,0x100)
    put(pe,0x100,'<IIIII',0x200,0,0,0x300,0x400);string(pe,0x300,'KERNEL32.dll')
    for i,name in enumerate(ACCESSORS):
        symbol=0x500+i*64;put(pe,symbol,'<H',i);string(pe,symbol+2,name)
        put(pe,0x200+i*8,'<Q',symbol);put(pe,0x400+i*8,'<Q',symbol)
        thunk=0x1000+i*16;put(pe,thunk,'<2si',b'\xff\x25',0x400+i*8-thunk-6)
    names=(*EXPORTS,'__errno');count=len(names)
    put(pe,0x800,'<IIHHIIIIIII',0,0,0,0,0,1,count,count,0x900,0xa00,0xb00)
    for i,name in enumerate(names):
        put(pe,0x900+i*4,'<I',0x1200+i*16)
        put(pe,0xa00+i*4,'<I',0xc00+i*64);put(pe,0xb00+i*2,'<H',i)
        string(pe,0xc00+i*64,name)
    pe.exports=pe.read_exports();pe.imports=pe.read_imports()
    reader=object.__new__(gate.PE);reader.data=bytearray(0x3000);reader.base=0x100400000
    reader.sections=list(pe.sections)
    for i,name in enumerate(EXPORTS):string(reader,0x1900+i*64,name)
    return pe,reader

class ColdLinkage(unittest.TestCase):
    def setUp(self):self.pe,self.reader=fixture()
    def evidence(self,calls=None):
        if calls is None:calls=[(name,0x1000+i*16) for i,name in enumerate(ACCESSORS)]+[('emit',0x1300),('__errno',0x1240)]
        return cold.evidence(self.pe,self.reader,EXPORTS,calls,0x1300,0x1240,ACCESSORS,'emit')
    def test_excerpt_bounds_and_exact_bytes(self):
        got=cold.excerpt(self.pe,0x100,20);raw=self.pe.at(0x100,20)
        self.assertEqual(got['hex'],raw.hex());self.assertEqual(got['sha256'],hashlib.sha256(raw).hexdigest())
        for size in (0,-1,257):
            with self.assertRaises(ValueError):cold.excerpt(self.pe,0x100,size)
        with self.assertRaises(ValueError):cold.excerpt(self.pe,0x2fff,2)
        with self.assertRaises(ValueError):cold.name_excerpt(self.pe,0x300,'wrong.dll')
    def test_import_full_chain(self):
        got=cold.import_binding(self.pe,0x408,('kernel32.dll','SetLastError'))
        self.assertEqual({k:v['rva'] for k,v in got.items()},dict(descriptor=0x100,lookup=0x208,iat=0x408,dll=0x300,hint=0x540,symbol=0x542))
        self.assertEqual(got['lookup']['hex'],got['iat']['hex'])
        self.assertEqual(got['hint']['hex'],'0100')
    def test_original_thunk_absent_uses_iat(self):
        put(self.pe,0x100,'<I',0)
        got=cold.import_binding(self.pe,0x400,('kernel32.dll','GetLastError'))
        self.assertEqual(got['lookup'],got['iat'])
    def test_import_rejects_ordinal_bound_wrong_and_missing(self):
        for mutation in ('ordinal','bound','wrong_dll','wrong_symbol','terminated'):
            with self.subTest(mutation=mutation):
                self.pe,_=fixture()
                if mutation=='ordinal':put(self.pe,0x200,'<Q',(1<<63)|3)
                elif mutation=='bound':put(self.pe,0x400,'<Q',0x180123456)
                elif mutation=='wrong_dll':string(self.pe,0x300,'OTHER.dll')
                elif mutation=='wrong_symbol':string(self.pe,0x502,'Wrong')
                else:put(self.pe,0x200,'<Q',0)
                with self.assertRaises(ValueError):cold.import_binding(self.pe,0x400,('kernel32.dll','GetLastError'))
        self.pe,_=fixture()
        for slot in (0x3f8,0x401,0x418):
            with self.assertRaises(ValueError):cold.import_binding(self.pe,slot,('kernel32.dll','GetLastError'))
    def test_export_directory_name_ordinal_function(self):
        got=cold.export_binding(self.pe,EXPORTS[2])
        self.assertEqual(got['code_rva'],0x1220)
        self.assertEqual(got['directory']['size'],40)
        self.assertEqual(got['name_pointer']['rva'],0xa08)
        self.assertEqual(got['ordinal']['hex'],'0200')
        self.assertEqual(got['function']['hex'],'20120000')
        self.assertEqual(got['name']['name'],EXPORTS[2])
    def test_export_rejects_missing_ordinal_forwarder_drift_and_ambiguity(self):
        for mutation in ('missing','ordinal','forwarded','drift','duplicate','bounds'):
            with self.subTest(mutation=mutation):
                self.pe,_=fixture();name=EXPORTS[0]
                if mutation=='missing':name='not_exported'
                elif mutation=='ordinal':put(self.pe,0xb00,'<H',5)
                elif mutation=='forwarded':put(self.pe,0x900,'<I',0x810);self.pe.exports[name]=0x810
                elif mutation=='drift':put(self.pe,0x900,'<I',0x1400)
                elif mutation=='duplicate':put(self.pe,0xa04,'<I',0xc00);put(self.pe,0xb02,'<H',0)
                else:put(self.pe,0x800+24,'<I',0)
                with self.assertRaises(ValueError):cold.export_binding(self.pe,name)
    def test_reader_unique_rdata_exact_name_bytes(self):
        got=cold.reader_names(self.reader,EXPORTS)
        for i,name in enumerate(EXPORTS):
            self.assertEqual(got[name]['rva'],0x1900+i*64)
            self.assertEqual(bytes.fromhex(got[name]['hex']),name.encode()+b'\0')
        string(self.reader,0x1100,EXPORTS[0]) # An unrelated .text spelling is not a match.
        self.assertEqual(cold.reader_names(self.reader,EXPORTS),got)
    def test_reader_rejects_absent_and_ambiguous_names(self):
        with self.assertRaises(ValueError):cold.reader_names(self.reader,('missing',))
        string(self.reader,0x1d00,EXPORTS[0])
        with self.assertRaises(ValueError):cold.reader_names(self.reader,EXPORTS)
    def test_reader_pointer_pool_exact_selected_eight_byte_records(self):
        names=cold.reader_names(self.reader,EXPORTS)
        put(self.reader,0x1e00,'<Q',self.reader.base+names[EXPORTS[0]]['rva'])
        put(self.reader,0x1e08,'<Q',self.reader.base+names[EXPORTS[2]]['rva'])
        put(self.reader,0x1e10,'<Q',self.reader.base+0x1111) # Unrelated pointer excluded.
        put(self.reader,0x1100,'<Q',self.reader.base+names[EXPORTS[0]]['rva']) # .text excluded.
        pointers=cold.reader_name_pointers(self.reader,names)
        self.assertEqual([r['rva'] for r in pointers[EXPORTS[0]]],[0x1e00])
        self.assertEqual([r['rva'] for r in pointers[EXPORTS[2]]],[0x1e08])
        self.assertEqual(pointers[EXPORTS[1]],[])
        for name,records in pointers.items():
            for record in records:
                self.assertEqual(record['size'],8)
                self.assertEqual(struct.unpack('<Q',bytes.fromhex(record['hex']))[0],self.reader.base+names[name]['rva'])
        plain=cold.evidence(self.pe,fixture()[1],EXPORTS,[(name,0x1000+i*16) for i,name in enumerate(ACCESSORS)],0x1300,0x1240,ACCESSORS,'emit')
        full=self.evidence()
        self.assertEqual(full['selected_bytes']-plain['selected_bytes'],16)
        old=cold.MAX_BYTES
        try:
            cold.MAX_BYTES=full['selected_bytes']-1
            with self.assertRaises(ValueError):self.evidence()
        finally:cold.MAX_BYTES=old
    def test_reader_pointer_pool_bound_and_unaligned_noise(self):
        names=cold.reader_names(self.reader,EXPORTS);target=self.reader.base+names[EXPORTS[0]]['rva']
        put(self.reader,0x1e01,'<Q',target)
        self.assertEqual(cold.reader_name_pointers(self.reader,names)[EXPORTS[0]],[])
        for i in range(5):put(self.reader,0x1f00+i*8,'<Q',target)
        with self.assertRaises(ValueError):cold.reader_name_pointers(self.reader,names)
    def test_evidence_exact_thunks_and_no_approval(self):
        result=self.evidence()
        self.assertFalse(result['fixtures_executed']);self.assertFalse(result['approved_for_fixture_execution'])
        self.assertEqual(set(result['accessors']),set(ACCESSORS))
        for i,name in enumerate(ACCESSORS):
            record=result['accessors'][name]
            self.assertEqual(record['thunk']['rva'],0x1000+i*16)
            self.assertTrue(record['thunk']['hex'].startswith('ff25'))
            self.assertEqual(record['binding']['iat']['rva'],0x400+i*8)
    def test_evidence_rejects_bad_thunk_unknown_and_missing_callee(self):
        self.pe.data[0x1000]=0xe9
        with self.assertRaises(ValueError):self.evidence()
        self.pe,_=fixture()
        with self.assertRaises(ValueError):self.evidence([('malloc',0x1300)])
        with self.assertRaises(ValueError):self.evidence([])
        with self.assertRaises(ValueError):self.evidence([('emit',0x1310)])
        with self.assertRaises(ValueError):self.evidence([('__errno',0x1250)])
        self.pe.imports[0x400]=('other.dll','GetLastError')
        with self.assertRaises(ValueError):self.evidence()
    def test_evidence_rejects_ambiguous_accessor(self):
        put(self.pe,0x1100,'<2si',b'\xff\x25',0x400-0x1100-6)
        calls=[(name,0x1000+i*16) for i,name in enumerate(ACCESSORS)]+[(ACCESSORS[0],0x1100)]
        with self.assertRaises(ValueError):self.evidence(calls)
    def test_total_evidence_byte_bound(self):
        old=cold.MAX_BYTES
        try:
            cold.MAX_BYTES=1
            with self.assertRaises(ValueError):self.evidence()
        finally:cold.MAX_BYTES=old

if __name__=='__main__':unittest.main()
