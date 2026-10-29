#!/usr/bin/env python3
import importlib.util,pathlib,struct,unittest
HERE=pathlib.Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('linked',HERE/'linked-preflight.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
def fixture():
 b=bytearray(0x3200);b[:2]=b'MZ';struct.pack_into('<I',b,60,0x80);b[0x80:0x84]=b'PE\0\0'
 struct.pack_into('<HH',b,0x84,0x8664,1);struct.pack_into('<H',b,0x94,240)
 opt=0x98;struct.pack_into('<H',b,opt,0x20b);struct.pack_into('<Q',b,opt+24,0x180000000)
 struct.pack_into('<I',b,opt+108,16);struct.pack_into('<IIII',b,opt+112,0x2000,0x400,0x2600,40)
 struct.pack_into('<8sIIII',b,opt+240,b'.text\0\0\0',0x3000,0x1000,0x3000,0x200)
 def off(r):return r-0x1000+0x200
 def put(r,data):b[off(r):off(r)+len(data)]=data
 def pack(r,fmt,*args):struct.pack_into(fmt,b,off(r),*args)
 pack(0x2000,'<IIHHIIIIIII',0,0,0,0,0,1,2,2,0x2100,0x2200,0x2250)
 pack(0x2100,'<II',0x1100,0x1200);pack(0x2200,'<II',0x2300,0x2320);pack(0x2250,'<HH',0,1)
 put(0x2300,b'__errno\0');put(0x2320,b'stormtty_path_handler_v1\0');put(0x1100,m.ERRNO_BYTES)
 pack(0x2600,'<IIIII',0x2700,0,0,0x2690,0x2800);put(0x2690,b'KERNEL32.dll\0')
 names=['GetLastError','SetLastError','GetCurrentThreadId']
 for i,name in enumerate(names):
  pack(0x2700+8*i,'<Q',0x2900+64*i);pack(0x2800+8*i,'<Q',0x2900+64*i)
  put(0x2900+64*i,b'\0\0'+name.encode()+b'\0');r=0x1500+16*i
  put(r,b'\xff\x25'+struct.pack('<i',0x2800+8*i-r-6))
 return b,off
class Linked(unittest.TestCase):
 def test_export_tls_leaf_and_import_thunk(self):
  b,_=fixture();p=m.PE(bytes(b));self.assertEqual(p.code_export('__errno'),0x1100)
  self.assertEqual(p.at(0x1100,len(m.ERRNO_BYTES)),m.ERRNO_BYTES)
  for i,name in enumerate(('GetLastError','SetLastError','GetCurrentThreadId')):self.assertEqual(p.os_thunk(0x1500+16*i,name),0x2800+8*i)
 def test_reject_forwarded_export_and_wrong_import(self):
  b,off=fixture();struct.pack_into('<I',b,off(0x2100),0x2300)
  with self.assertRaisesRegex(ValueError,'forwarded'):m.PE(bytes(b)).code_export('__errno')
  b,off=fixture();b[off(0x2690):off(0x2690)+12]=b'EVILNAME.dll'
  with self.assertRaisesRegex(ValueError,'wrong OS'):m.PE(bytes(b)).os_thunk(0x1500,'GetLastError')
 def test_reject_truncation_machine_and_header(self):
  b,_=fixture()
  for data in (b'',bytes(b[:300]),bytes(b[:0x2600])):
   with self.assertRaises(ValueError):m.PE(data)
  struct.pack_into('<H',b,0x84,0x14c)
  with self.assertRaises(ValueError):m.PE(bytes(b))
 def test_linked_code_only_masks_actual_relocations(self):
  b,off=fixture();body=' 0: e8 00 00 00 00 call 5\n 1: IMAGE_REL_AMD64_REL32 __errno\n 5: c3 ret\n'
  code=b'\xe8'+struct.pack('<i',0x1100-0x1200-5)+b'\xc3';b[off(0x1200):off(0x1200)+len(code)]=code
  length,calls=m.linked_body(m.PE(bytes(b)),0x1200,0,body);self.assertEqual(length,6);self.assertEqual(calls,[('__errno',0x1100)])
  b[off(0x1200)+5]=0x90
  with self.assertRaisesRegex(ValueError,'instruction mismatch'):m.linked_body(m.PE(bytes(b)),0x1200,0,body)
 def test_non_call_external_reference_and_gap_rejected(self):
  b,off=fixture();body=' 0: 48 8b 05 00 00 00 00 mov\n 3: IMAGE_REL_AMD64_REL32 malloc\n'
  b[off(0x1200):off(0x1200)+7]=bytes.fromhex('488b0500000000')
  with self.assertRaisesRegex(ValueError,'non-call'):m.linked_body(m.PE(bytes(b)),0x1200,0,body)
  with self.assertRaisesRegex(ValueError,'range/gap'):m.object_code(0,' 0: c3 ret\n 2: 90 nop\n')
 def test_no_general_safety_or_runtime_execution_claim(self):
  s=(HERE/'linked-preflight.py').read_text();self.assertIn('general_async_signal_safety_proven=False',s)
  self.assertNotIn('LoadLibrary',s);self.assertNotIn('ctypes',s);self.assertIn('timeout=20',s)
if __name__=='__main__':unittest.main()
