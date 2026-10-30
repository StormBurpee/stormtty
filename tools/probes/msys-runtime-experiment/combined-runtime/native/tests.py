#!/usr/bin/env python3
"""Portable source/fixture contract checks, not Windows API or ABI evidence."""
from pathlib import Path
import subprocess
import tempfile
import unittest
HERE=Path(__file__).resolve().parent

class NativeSource(unittest.TestCase):
    def test_c_syntax_with_portable_api_declarations(self):
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-pedantic',
            '-fsyntax-only','-I'+str(HERE/'tests'),str(HERE/'controller.c'),str(HERE/'reader.c')],check=True,capture_output=True)

    def test_cases_are_bounded_and_unique(self):
        source=r'''
#include <assert.h>
#include "cases.h"
int main(void){
 assert(sizeof st_cases/sizeof st_cases[0]==15);
 for(size_t i=0;i<sizeof st_cases/sizeof st_cases[0];++i){
  const st_case *c=&st_cases[i];assert(st_find_case(c->name)==c);
  assert(c->gates<=3&&c->gate_index<=1&&c->requested<=64);
  assert(strlen(c->expected)>0&&strlen(c->expected)<ST_MAX_OUTPUT);
  assert(!c->select_first||!c->gates);
  for(size_t j=0;j<i;++j)assert(strcmp(c->name,st_cases[j].name));
 }
 uint64_t value=0;assert(st_decimal("18446744073709551615",&value)&&value==UINT64_MAX);
 assert(!st_decimal("18446744073709551616",&value));assert(!st_decimal("0",&value));assert(!st_decimal("-1",&value));assert(!st_decimal("1\r",&value));
 assert(!st_find_case("copied-positive"));return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=HERE) as directory:
            path=Path(directory)/'test.c';binary=Path(directory)/'test';path.write_text(source)
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-pedantic','-I'+str(HERE),str(path),'-o',str(binary)],check=True,capture_output=True)
            subprocess.run([str(binary)],check=True,capture_output=True)

    def test_no_perturbation_shortcuts(self):
        source=(HERE/'controller.c').read_text()+(HERE/'reader.c').read_text()
        for forbidden in ('Sleep(', 'SuspendThread(', 'FlushConsoleInput', 'ReadConsoleInput', 'sigprocmask(', 'kill(', 'TerminateThread('):
            self.assertNotIn(forbidden,source)
        self.assertEqual(source.count('WriteConsoleInputW('),1)
        self.assertIn('accepted!=input_count',source)
        self.assertIn('PROC_THREAD_ATTRIBUTE_HANDLE_LIST',source)
        self.assertNotIn('AssignProcessToJobObject',source)
        self.assertNotIn('TerminateProcess',source)
        self.assertIn('InitializeProcThreadAttributeList(NULL,2,0,&attr_size)',source)
        self.assertIn('InitializeProcThreadAttributeList(si.lpAttributeList,2,0,&attr_size)',source)
        job_attribute='UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,creation_jobs,sizeof creation_jobs,NULL,NULL)'
        self.assertIn('if(!'+job_attribute+')goto fail;',source)
        self.assertLess(source.index(job_attribute),source.index('CreateProcessA(argv[1]'))
        self.assertLess(source.index('CreateProcessA(argv[1]'),source.index('ResumeThread(pi.hThread)'))
        # The job handle is a creation attribute, never inherited by the child.
        self.assertNotIn('inherited[8]=job',source)
        self.assertNotIn('inherited[7]=job',source)
        self.assertIn('JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE',source)

    def test_snapshot_requires_job_zero_and_primary_reap(self):
        source=(HERE/'controller.c').read_text()
        self.assertIn('if(!accounting.ActiveProcesses){all_writers_stopped=1;break;}',source)
        self.assertIn('if(trace_view&&trace_file!=INVALID_HANDLE_VALUE&&all_writers_stopped&&reaped)',source)
        self.assertIn('if(reaped&&all_writers_stopped&&observed&&observed->magic',source)
        self.assertIn('GetQueuedCompletionStatus',source)
        self.assertIn('reader\\\":',source)

if __name__=='__main__':unittest.main()
