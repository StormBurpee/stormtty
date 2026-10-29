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


RESERVED=('STORMTTY_NATIVE_ZSH_NO_DUMP', 'STORMTTY_NATIVE_ZSH_INIT_EVENT')

def resolver_overrides(result):
    if result.returncode:
        raise AssertionError('Native home resolver failed: '
            f'exit={result.returncode} exit_u32={result.returncode & 0xffffffff} '
            f'stdout_bytes={len(result.stdout)} stderr_bytes={len(result.stderr)}')
    overrides={}
    for line in result.stdout.decode('ascii').splitlines():
        name,value=line.split('=',1)
        if name not in ('HOME','STORMTTY_EMPTY_HOME',*RESERVED) or name in overrides:
            raise AssertionError('Unexpected or duplicate resolver override')
        if name in RESERVED:
            if value!='-': raise AssertionError('Reserved launch variable was not removed')
            overrides[name]=None
        else:
            overrides[name]=bytes.fromhex(value).decode('utf-8')
    if 'STORMTTY_EMPTY_HOME' not in overrides or any(name not in overrides for name in RESERVED):
        raise AssertionError('Missing required resolver override')
    return overrides

def apply_overrides(environment, overrides):
    # Windows environment names are case-insensitive; a plain dict is not.
    for name,value in overrides.items():
        for existing in list(environment):
            if existing.upper()==name.upper(): del environment[existing]
        if value is not None: environment[name]=value


def qualify(runtime, resolver_probe):
    runtime = runtime.resolve(strict=True)
    shell = runtime / 'usr/bin/zsh.exe'
    resolver_probe = resolver_probe.resolve(strict=True)
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
        # Host must clear inherited launch-control values, including default OFF.
        base.update({name.lower():'untrusted-inherited-value' for name in RESERVED})
        # Run the exact C++ wide-API helper used by the app, not a Python substitute.
        def run(changes, command="print -r -- HOME=$HOME CWD=$PWD; print -r -- PRESET=$STORMTTY_MANAGED_ZSH"):
            env = dict(base)
            for name, value in changes.items():
                if value is None: env.pop(name, None)
                else: env[name] = value
            resolved = subprocess.run([str(resolver_probe), '--managed-home-environment'],
                                      cwd=project, env=env, capture_output=True, timeout=30)
            overrides = resolver_overrides(resolved)
            if 'HOME' in env:
                assert 'HOME' not in overrides, 'Resolver overwrote explicit HOME'
            apply_overrides(env,overrides)
            assert all(name.upper() not in RESERVED for name in env)
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
        check({'HOME': '/explicit/custom home', 'STORMTTY_EMPTY_HOME': '1'}, '/explicit/custom home')
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
    parser.add_argument('--resolver-probe', type=Path, required=True)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.exit(2, 'Windows runtime required; native path checks were NOT run.\n')
    qualify(args.runtime, args.resolver_probe)


if __name__ == '__main__':
    main()
