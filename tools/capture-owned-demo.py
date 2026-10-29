#!/usr/bin/env python3
"""Owned demonstration only: real Git repos/Zsh/renderer, no qualification replacement."""
from __future__ import annotations
import argparse
from contextlib import contextmanager
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile

MARKER = 'StormTTY owned presentation fixture v1\n'


def fixture_environment(root: Path, git: Path):
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('GIT_')}
    home = root / 'home'; home.mkdir(exist_ok=True)
    config = root / 'gitconfig'; config.write_text('', encoding='utf-8')
    env.update(HOME=str(home), USERPROFILE=str(home), APPDATA=str(home/'appdata'),
               LOCALAPPDATA=str(home/'local'), XDG_CONFIG_HOME=str(home/'xdg'),
               GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=str(config),
               GIT_TERMINAL_PROMPT='0', LC_ALL='C',
               PATH=str(git.parent)+os.pathsep+env.get('PATH',''))
    return env


def create_fixture(root: Path, git: Path, env):
    # The caller exclusively created root. No existing checkout is inspected.
    if any(root.iterdir()):
        # Environment preparation owns exactly these two initial entries.
        if set(p.name for p in root.iterdir()) != {'home', 'gitconfig'}:
            raise ValueError('fixture must be newly owned')
    (root/'fixture.marker').write_text(MARKER, encoding='utf-8', newline='\n')
    for name in ('Theme Lab', 'Notes'):
        repo=root/name; repo.mkdir()
        def git_run(*args):
            return subprocess.run([str(git),'-c','init.templateDir=', '-C',str(repo),*args],
                                  env=env,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20)
        git_run('init','-b','palette-review')
        git_run('config','user.name','StormTTY Owned Demo')
        git_run('config','user.email','owned-demo@example.invalid')
        git_run('config','core.autocrlf','false')
        git_run('config','core.hooksPath',str(root/'no-hooks'))
        (repo/'README.md').write_text('# Owned StormTTY demo\n\nDisposable files created only for native UI screenshots.\n',encoding='utf-8')
        (repo/'theme.toml').write_text('name = "Graphite"\naccent = "#8F98FF"\n',encoding='utf-8')
        git_run('add','README.md','theme.toml')
        git_run('-c','commit.gpgsign=false','commit','-m','Create owned presentation fixture')
        (repo/'theme.toml').write_text('name = "Graphite"\naccent = "#8F98FF"\n# Compare palette previews in native Settings.\n',encoding='utf-8')
        (repo/'ideas.md').write_text('# Palette notes\n\nReview contrast and keyboard focus at narrow widths.\n',encoding='utf-8')
        actual=git_run('status','--short','--branch').stdout.decode()
        if '## palette-review' not in actual or ' M theme.toml' not in actual or '?? ideas.md' not in actual:
            raise RuntimeError('real Git state did not match the owned fixture')


@contextmanager
def child_environment(env):
    previous=dict(os.environ)
    try:
        os.environ.clear();os.environ.update(env);yield
    finally:
        os.environ.clear();os.environ.update(previous)


def can_remove_fixture(launched, result):
    if not launched:
        return True
    # Parent reaping alone is not descendant-cleanup evidence. On timeout or
    # any cleanup error retain the root, even if the parent was eventually reaped.
    return (result is not None and result.reaped and not result.cleanup_error
            and not result.timed_out and result.returncode == 0)


def checked_fixture_path(root, path, identity):
    """No links/junctions, escapes or replacement of the exclusively created root."""
    relative = path.relative_to(root)
    if '..' in relative.parts:
        raise ValueError('fixture path escapes root')
    current = root
    for part in (None, *relative.parts):
        if part is not None:
            current = current / part
        info = current.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError('fixture contains a link or reparse point')
        if current == root and (info.st_dev, info.st_ino) != identity:
            raise ValueError('fixture root identity changed')
        if current != path and not stat.S_ISDIR(info.st_mode):
            raise ValueError('fixture ancestor is not a directory')
    if not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
        raise ValueError('fixture contains a special file')
    if stat.S_ISREG(info.st_mode) and info.st_nlink != 1:
        raise ValueError('fixture contains a multiply linked file')
    return info


def remove_fixture(root, identity, launched, result):
    """Fail closed; only the caller's freshly created, marker-owned tree is eligible."""
    if not can_remove_fixture(launched, result):
        raise ValueError('clean owned-process completion was not confirmed')
    marker = root / 'fixture.marker'
    def validate(path):
        info = checked_fixture_path(root, path, identity)
        marker_info = checked_fixture_path(root, marker, identity)
        if not stat.S_ISREG(marker_info.st_mode) or marker.read_bytes() != MARKER.encode():
            raise ValueError('fixture ownership marker missing or changed')
        return info

    # Inspect the complete tree before deleting anything. Keep the marker until
    # all children are gone so failure leaves an identifiable retained fixture.
    def preflight(path):
        info = validate(path)
        if stat.S_ISDIR(info.st_mode):
            for child in path.iterdir():
                preflight(child)
    preflight(root)
    retried = set()
    def readonly_git_object(function, path, exc_info):
        error = exc_info[1]
        path = Path(path)
        info = validate(path)
        parts = path.relative_to(root).parts
        git_object = (len(parts) == 5 and parts[0] in ('Theme Lab', 'Notes')
                      and parts[1:3] == ('.git', 'objects')
                      and len(parts[3]) == 2 and len(parts[4]) == 38
                      and all(c in '0123456789abcdef' for c in parts[3] + parts[4]))
        if (sys.platform != 'win32' or function is not os.unlink
                or not isinstance(error, PermissionError) or getattr(error, 'winerror', None) != 5
                or not stat.S_ISREG(info.st_mode) or not git_object or path in retried
                or not getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_READONLY):
            raise error
        retried.add(path)
        # Windows chmod changes only the readonly attribute, not ACLs. One
        # retry only; sharing violations and all other failures retain the tree.
        os.chmod(path, stat.S_IWRITE)
        validate(path)
        os.unlink(path)

    for child in root.iterdir():
        if child == marker:
            continue
        info = validate(child)
        if stat.S_ISDIR(info.st_mode):
            shutil.rmtree(child, onerror=readonly_git_object)
        else:
            child.unlink()
    validate(marker)
    marker.unlink()
    root.rmdir()


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--app',type=Path,required=True)
    parser.add_argument('--shell-runtime',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if os.name!='nt': raise SystemExit('Native owned-demo capture requires Windows')
    git_path=shutil.which('git')
    if not git_path: raise SystemExit('Git is required; no synthetic Git output fallback')
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    root=Path(tempfile.mkdtemp(prefix='StormTTY.Presentation.')).resolve()
    root_stat=root.lstat();identity=(root_stat.st_dev,root_stat.st_ino)
    result=None;launched=False
    try:
        env=fixture_environment(root,Path(git_path).resolve());create_fixture(root,Path(git_path).resolve(),env)
        spec=importlib.util.spec_from_file_location('owned_demo_runner',Path(__file__).with_name('run-owned-process.py'))
        runner=importlib.util.module_from_spec(spec);sys.modules[spec.name]=runner;spec.loader.exec_module(runner)
        with child_environment(env):
            launched=True
            result=runner.run_owned([str(args.app.resolve()),'--smoke-presentation','--workspace-fixture',str(root),
                                    '--shell-runtime',str(args.shell_runtime.resolve()),'--capture',str(output/'demo.png')],
                                   timeout=90,cwd=root,output_dir=output)
        log=result.output_tail.decode('utf-8',errors='replace')
        (output/'owned-demo.log').write_text(json.dumps(result.metadata(),sort_keys=True)+'\n'+log,encoding='utf-8')
        expected=['workspace-owned-demo','navigator-owned-demo','background-owned-demo']
        missing=[name for name in expected if not (output/('demo-'+name+'.png')).is_file()]
        passed=not(result.exit_code or not result.reaped or result.cleanup_error or result.timed_out or result.output_truncated or missing)
        passed=passed and 'Owned presentation capture: real managed Zsh' in log
        (output/'owned-demo-manifest.json').write_text(json.dumps({'source_sha':os.environ.get('GITHUB_SHA','local'),
            'owned_demo':True,'presentation_only':True,'user_data_used':False,'qualified_paste_or_performance':False,
            'passed':passed,'missing':missing},indent=2),encoding='utf-8')
        print(log)
        return 0 if passed else 1
    finally:
        try:
            remove_fixture(root,identity,launched,result)
        except (OSError, ValueError) as error:
            print('Owned demo fixture retained: '+str(root)+': '+str(error),file=sys.stderr)
            raise



if __name__=='__main__':raise SystemExit(main())
