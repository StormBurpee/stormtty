"""Portable provenance safety and exact source tests, not Windows execution."""
import hashlib,importlib.util,json,os,pathlib,shutil,struct,subprocess,tempfile,unittest
from unittest.mock import patch
HERE=pathlib.Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('identities',HERE/'identities.py');identity=importlib.util.module_from_spec(spec);spec.loader.exec_module(identity);stage=identity.stage

def pe(tag=b''):
 b=bytearray(512);b[:2]=b'MZ';struct.pack_into('<I',b,60,128);b[128:132]=b'PE\0\0'
 struct.pack_into('<H',b,132,0x8664);struct.pack_into('<HHH',b,148,240,0x2000,0x20b)
 return bytes(b)+tag

class Runtime(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.root=pathlib.Path(self.tmp.name)
  self.base=self.root/'original';(self.base/'usr/bin').mkdir(parents=True)
  (self.base/'usr/bin/msys-2.0.dll').write_bytes(pe(b'official'));(self.base/'usr/bin/zsh.exe').write_bytes(b'fixed-shell')
  manifest={'bundle_id':'fixture','files':[{'path':'usr/bin/'+n,'directory':False,'sha256':stage.sha(self.base/'usr/bin'/n)} for n in ('msys-2.0.dll','zsh.exe')]}
  (self.base/'stormtty-shell-manifest.json').write_text(json.dumps(manifest));self.before=(self.base/'stormtty-shell-manifest.json').read_bytes()
  self.source=b'fixture\n';digest=hashlib.sha256(self.source).hexdigest();self.mapping={f:digest for f in ('console.cc','termios.cc','fhandler.h')}
  for name,value in {'SOURCE_SHA256':digest,'BEFORE_SHA256':digest,'STOCK_SHA256':digest,'AFTER_SHA256':digest,'ORIGINAL_SOURCE_FILES_SHA256':self.mapping,'SOURCE_FILES_SHA256':{'stock':self.mapping,'copied':self.mapping}}.items():
   p=patch.object(stage,name,value);p.start();self.addCleanup(p.stop)
 def build(self,variant):
  b=self.root/(variant+'-build');p=b/'provenance';p.mkdir(parents=True)
  dll=b/'experimental-msys-2.0.dll';dll.write_bytes(pe(variant.encode()))
  for n in ('msys2-runtime-3.6.11-4.src.tar.zst','console-before.cc','console-after.cc','pacman-before.txt','pacman-after.txt','compiler-packages.txt'): (p/n).write_bytes(self.source)
  for f in self.mapping:
   for suffix in ('before','after'):(p/(f+'.'+suffix)).write_bytes(self.source)
  for src in (HERE/'0007-preserve-copied-on-signal.patch',HERE.parent/'0000-utils-const-compat.patch'):shutil.copyfile(src,p/src.name)
  (p/'compiler-mode.txt').write_bytes(stage.COMPILER_MODE[variant]);(p/'variant.txt').write_text(variant);(p/'hashes.txt').write_text(stage.sha(dll)+'  DLL\n');(p/'dll-exports.txt').write_text('Export Table fixture\n')
  return b
 def roots(self):
  roots={'original':self.base}
  for v in ('stock','copied'):
   roots[v]=self.root/v;stage.stage(self.base,self.build(v),roots[v])
  return roots
 def test_three_variants_and_untouched_original(self):
  roots=self.roots();self.assertEqual(set(identity.identities(roots)),set(roots));self.assertEqual((self.base/'stormtty-shell-manifest.json').read_bytes(),self.before)
  for v in ('stock','copied'):
   info=json.loads((roots[v]/'stormtty-shell-manifest.json').read_text())['experimental_runtime']
   self.assertFalse(info['trace_enabled']);self.assertFalse(info['retirement_patch_applied']);self.assertEqual(info['behavior_patch_applied'],v=='copied')
 def test_marker_forbidden_even_when_self_attested(self):
  roots=self.roots();p=roots['copied']/'usr/bin/msys-2.0.dll';p.write_bytes(pe(b'stormtty_path_init_v1'))
  with self.assertRaisesRegex(ValueError,'Diagnostic'):identity.identities(roots)
 def test_pe_validation(self):
  p=self.root/'test.dll'
  for data in (b'MZ',pe().replace(b'PE\0\0',b'NOPE'),pe().replace(b'\x64\x86',b'\x4c\x01'),pe(b'STORMTTY_PATH')):
   p.write_bytes(data)
   with self.assertRaises(ValueError):stage.verify_dll(p)
 def test_missing_export_report(self):
  b=self.build('stock');(b/'provenance/dll-exports.txt').unlink()
  with self.assertRaises(OSError):stage.stage(self.base,b,self.root/'stock')
 def test_export_report_marker(self):
  b=self.build('stock');(b/'provenance/dll-exports.txt').write_text('stormtty_path_handler_v1')
  with self.assertRaises(ValueError):stage.stage(self.base,b,self.root/'stock')
 def test_existing_destination(self):
  b=self.build('stock');dest=self.root/'stock';dest.mkdir()
  with self.assertRaises(ValueError):stage.stage(self.base,b,dest)
 def test_destination_inside_build_rejected(self):
  b=self.build('stock')
  with self.assertRaises(ValueError):stage.stage(self.base,b,b/'nested')
 def test_alias_roots_rejected(self):
  with self.assertRaises(ValueError):identity.identities({'original':self.base,'stock':self.base,'copied':self.base})
 def test_symbolic_base_rejected(self):
  b=self.build('stock');alias=self.root/'alias';alias.symlink_to(self.base,target_is_directory=True)
  with self.assertRaises(ValueError):stage.stage(alias,b,self.root/'stock')
 def test_bad_source_or_patch_or_inventory(self):
  for n in ('console.cc.after','0007-preserve-copied-on-signal.patch','0000-utils-const-compat.patch','pacman-after.txt','compiler-mode.txt'):
   b=self.build('stock');(b/'provenance'/n).write_bytes(b'changed')
   with self.assertRaises(ValueError,msg=n):stage.stage(self.base,b,self.root/'stock')
   shutil.rmtree(b)
 def test_identity_rejects_flags_or_inventory_mutation(self):
  roots=self.roots();p=roots['copied']/'stormtty-shell-manifest.json';original=p.read_text()
  for key,value in (('trace_enabled',True),('retirement_patch_applied',True),('behavior_patch_applied',False)):
   m=json.loads(original);m['experimental_runtime'][key]=value;p.write_text(json.dumps(m))
   with self.assertRaises(ValueError,msg=key):identity.identities(roots)
  p.write_text(original);m=json.loads(original);m['files'].append(m['files'][0]);p.write_text(json.dumps(m))
  with self.assertRaisesRegex(ValueError,'duplicate'):identity.identities(roots)
 def test_identity_rejects_export_provenance_mutation(self):
  roots=self.roots();(roots['copied']/'experimental-runtime-provenance/dll-exports.txt').write_text('changed')
  with self.assertRaises(ValueError):identity.identities(roots)
 def test_identity_requires_three_roots(self):
  with self.assertRaises(ValueError):identity.identities({'original':self.base})
 def test_build_contract_and_locked_patch(self):
  s=(HERE/'build.sh').read_text();self.assertNotIn('0006-',s);self.assertNotIn('PASSIVE_CONSOLE_TRACE',s);self.assertNotIn('-Wno-error',s)
  self.assertIn('--verify-dll',s);self.assertIn('objdump -p',s);self.assertIn('makepkg --config',s)
  ids=json.loads((HERE/'behavior-identities.json').read_text());self.assertEqual(stage.sha(HERE/'0007-preserve-copied-on-signal.patch'),ids['patch_sha256'])
 @unittest.skipUnless(os.environ.get('PARTIAL_READ_SOURCE'),'exact stock source path required')
 def test_exact_patch_application(self):
  ids=json.loads((HERE/'behavior-identities.json').read_text());src=pathlib.Path(os.environ['PARTIAL_READ_SOURCE']);self.assertEqual(stage.sha(src),ids['stock_console_sha256'])
  p=self.root/'winsup/cygwin/fhandler/console.cc';p.parent.mkdir(parents=True);shutil.copyfile(src,p)
  subprocess.run(['git','apply',str(HERE/'0007-preserve-copied-on-signal.patch')],cwd=self.root,check=True)
  self.assertEqual(stage.sha(p),ids['candidate_console_sha256'])
if __name__=='__main__':unittest.main()
