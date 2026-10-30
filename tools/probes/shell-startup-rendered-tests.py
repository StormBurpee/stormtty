#!/usr/bin/env python3
"""POSIX ZLE -> current TerminalCore regression; not Windows qualification.

Requires pexpect and the built startup_diagnostic_observation_tests executable.
Only authored fixture output is captured, in memory, and only numeric status is
printed. No user startup files, raw transcript files or clipboard are used.
"""
import argparse
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import pexpect


class BoundedCapture(io.StringIO):
    def write(self,text):
        if self.tell()+len(text)>65536:
            raise RuntimeError('fixture output too large')
        return super().write(text)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--observer',type=Path,required=True)
    parser.add_argument('--preset',type=Path,default=Path(__file__).resolve().parents[1]/'shell-preset.zsh')
    args=parser.parse_args()
    if os.name=='nt':parser.error('POSIX PTY regression; native Windows has a separate actual-app gate')
    shell=shutil.which('zsh')
    if not shell:raise RuntimeError('zsh required')
    observer=str(args.observer.resolve())
    for width in (20,40,80,120):
        for user_hook in (False,True):
            with tempfile.TemporaryDirectory() as directory:
                root=Path(directory)
                home=root/'home';home.mkdir(mode=0o700)
                zdot=root/'zdot';zdot.mkdir()
                shutil.copyfile(args.preset,zdot/'.zshrc')
                if user_hook:
                    state=home/'.stormtty/zsh';state.mkdir(parents=True,mode=0o700)
                    state.parent.chmod(0o700)
                    (state/'config.zsh').write_text('function user_line_init() { print -r -- STARTUP_USER_HOOK; }; zle -N zle-line-init user_line_init\n')
                env=dict(os.environ,HOME=str(home) if user_hook else '',ZDOTDIR=str(zdot),
                         STORMTTY_SESSION_ID='authored',TERM='xterm-256color',
                         STORMTTY_NATIVE_STARTUP_DIAGNOSTIC='ready' if user_hook else 'disabled',
                         STORMTTY_NATIVE_STARTUP_STATE_MS='0')
                child=pexpect.spawn(shell,['-di'],env=env,cwd=directory,encoding='utf-8',
                                    timeout=15,dimensions=(32,width))
                capture=BoundedCapture();child.logfile_read=capture
                try:
                    child.expect_exact('\x1b[?2004h')
                    child.sendline('print -r -- STARTUP_INPUT$((40+2))')
                    child.expect_exact('STARTUP_INPUT42\r\n')
                    child.expect_exact('\x1b[?2004h')
                    transcript=capture.getvalue()
                    if len(transcript.encode())>65536:raise RuntimeError('fixture output too large')
                    if user_hook and transcript.count('STARTUP_USER_HOOK')!=2:
                        raise RuntimeError('user line-init hook was not preserved')
                    # Leave the editor alive until after replay: match the native
                    # app's snapshot following actual input and prompt redisplay.
                    replay=subprocess.run([observer,'--replay',str(width)],input=transcript,
                                          text=True,capture_output=True,timeout=15)
                    if replay.returncode or replay.stdout!='STARTUP_RENDERED_TEST code=0\n':
                        raise RuntimeError('rendered diagnostic invalid')
                    child.sendline('exit 0');child.expect(pexpect.EOF);child.close()
                    if child.exitstatus!=0 or capture.getvalue().count('STORMTTY_STARTUP schema=')!=1:
                        raise RuntimeError('fixture exit or one-shot behavior invalid')
                finally:
                    child.close(force=True)
    print('STARTUP_RENDERED_MATRIX widths=4 hooks=2 code=0')


if __name__=='__main__':
    try:main()
    except Exception:
        print('STARTUP_RENDERED_MATRIX code=1')
        raise SystemExit(1)
