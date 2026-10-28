#!/usr/bin/env python3
"""Windows-only managed runtime path regression gate (not a ConPTY/UI test)."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def decode_output(result):
    """Keep reader threads binary; a charset mismatch is evidence, never success."""
    try:
        return result.stdout.decode('utf-8'), result.stderr.decode('utf-8')
    except UnicodeDecodeError as error:
        raise AssertionError(
            f'Runtime output is not UTF-8: {error}; exit={result.returncode}; '
            f'stdout_bytes={result.stdout!r}; stderr_bytes={result.stderr!r}'
        ) from error


def qualify(runtime):
    runtime = runtime.resolve(strict=True)
    shell = runtime / 'usr/bin/zsh.exe'
    original = dict(os.environ)
    # Keep the drive test outside Windows TEMP: /tmp deliberately takes priority
    # over /c for paths inside the usertemp mount.
    with tempfile.TemporaryDirectory(prefix='.stormtty-path-test-', dir=Path.cwd()) as temporary:
        scratch = Path(temporary)
        home = scratch / 'home café 路径'
        project = scratch / 'project café 路径'
        temp = scratch / 'temp café 路径'
        for path in (home, project, temp):
            path.mkdir()
        (home / '.zshrc').write_text('print DOTFILE_TRIPWIRE\n', encoding='utf-8')
        base = {k: v for k, v in original.items() if k.upper() not in
                ('HOME', 'HOMEDRIVE', 'HOMEPATH', 'USERPROFILE', 'MSYS', 'ENV', 'BASH_ENV',
                 'ZDOTDIR', 'STORMTTY_EMPTY_HOME')}
        base.update(PATH=str(shell.parent) + ';' + original.get('PATH', ''),
                    ZDOTDIR=str(runtime / 'stormtty/zsh'), STORMTTY_SESSION_ID='path-test',
                    TERM='xterm-256color', MSYSTEM='MSYS', LANG='C.UTF-8', LC_ALL='C.UTF-8',
                    USERPROFILE=str(home), TMP=str(temp), TEMP=str(temp))
        # These are the same child-only empty-HOME marker semantics as the host.
        def run(changes, command="print -r -- HOME=$HOME CWD=$PWD; print -r -- PRESET=$STORMTTY_MANAGED_ZSH"):
            env = dict(base)
            for name, value in changes.items():
                if value is None: env.pop(name, None)
                else: env[name] = value
            env['STORMTTY_EMPTY_HOME'] = '1' if env.get('HOME') == '' else ''
            diagnostic = 'print -r -- "LOCALE=$LANG|$LC_ALL"; /usr/bin/locale charmap; '
            result = subprocess.run([str(shell), '-d', '-i', '-c', diagnostic + command], cwd=project,
                                    env=env, capture_output=True, timeout=30)
            try:
                stdout, stderr = decode_output(result)
            except AssertionError as error:
                raise AssertionError(f'case={ascii(changes)}: {error}') from error
            if result.returncode or 'DOTFILE_TRIPWIRE' in stdout + stderr:
                raise AssertionError(f'Zsh failed or read user dotfiles: {result.returncode}: {ascii(stdout)}\n{ascii(stderr)}')
            return stdout
        def posix(path):
            value = path.as_posix()
            assert len(path.drive) == 2 and path.drive[1] == ':', 'drive test needs a local Windows workspace'
            return '/' + value[0].lower() + value[2:]
        cwd = posix(project)
        def check(changes, expected):
            output = run(changes)
            assert f'HOME={expected} CWD={cwd}' in output, output
            assert 'PRESET=1' in output, output
            assert run(changes, 'print -r -- MARKER=${+STORMTTY_EMPTY_HOME}').strip().endswith('MARKER=0')
        check({}, posix(home))  # absent HOME -> USERPROFILE
        check({'HOMEDRIVE': project.drive}, posix(home))  # incomplete pair falls through
        check({'HOMEDRIVE': project.drive, 'HOMEPATH': str(project)[2:]}, cwd)
        check({'HOME': str(home)}, posix(home))
        check({'HOME': '/explicit/custom home'}, '/explicit/custom home')
        check({'HOME': '/cygdrive/c/explicit'}, '/cygdrive/c/explicit')  # never silently repair callers
        check({'HOME': '//server/share/custom'}, '//server/share/custom')
        check({'HOME': ''}, '')
        output = run({}, 'cd -- "$HOME" && print -r -- AT_HOME=$PWD; cd /c && print -r -- DRIVE=$PWD; '
                         ': > /tmp/stormtty-path-write; print -r -- TEMP_OK')
        assert 'AT_HOME=' + posix(home) in output and 'DRIVE=/c' in output and 'TEMP_OK' in output, output
        assert (temp / 'stormtty-path-write').is_file(), '/tmp did not map to Windows temp'
        assert not (home / '.zcompdump').exists() and not (home / '.zsh_history').exists()
        assert (home / '.zshrc').read_text(encoding='utf-8') == 'print DOTFILE_TRIPWIRE\n'
    assert dict(os.environ) == original, 'parent environment changed'
    print('PASS: managed HOME fallback/explicit/empty, Unicode CWD, drive paths, usertemp, dotfile isolation; not ConPTY/UI qualification')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.exit(2, 'Windows runtime required; native path checks were NOT run.\n')
    qualify(args.runtime)


if __name__ == '__main__':
    main()
