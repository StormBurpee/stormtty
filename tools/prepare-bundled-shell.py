#!/usr/bin/env python3
"""Build-only pinned shell preparation. Never called by the application at launch."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parent

def recipe_digest():
    paths = [p for p in TOOLS.glob("shell-*") if p.is_file()]
    paths += [p for folder in ("shell-notices", "shell-tools-notices") for p in (TOOLS / folder).rglob("*") if p.is_file()]
    paths.append(Path(__file__).resolve())
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(path.relative_to(TOOLS).as_posix().encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()

def prepare(runtime, cache, offline=False, zstd=None):
    if not runtime.is_absolute() or not cache.is_absolute():
        raise RuntimeError("Build runtime/cache paths must be absolute")
    if runtime.is_symlink() or (runtime.exists() and getattr(runtime.lstat(),"st_file_attributes",0)&0x400):
        raise RuntimeError("Build runtime must not be a linked/reparse directory")
    runtime=runtime.resolve(); cache=cache.resolve()
    receipt=runtime.with_name(runtime.name+".build-recipe.json")
    recipe=recipe_digest()
    previous=json.loads(receipt.read_text()) if receipt.is_file() else {}
    owned=previous.get("schema")==1 and previous.get("build_owned") is True
    if runtime.exists() and not owned:
        raise RuntimeError("Refusing to replace a runtime not owned by this build helper")
    required=("etc/fstab","etc/nsswitch.conf","usr/bin/zsh.exe","usr/bin/msys-2.0.dll","stormtty/zsh/.zshrc","stormtty/vendor/oh-my-zsh/oh-my-zsh.sh","stormtty/vendor/addons.lock.json","usr/bin/awk.exe","usr/bin/msys-readline8.dll","usr/bin/msys-mpfr-6.dll","ucrt64/bin/fzf.exe","ucrt64/bin/zoxide.exe","stormtty/vendor/shell-tools/tools.lock.json","stormtty-shell-manifest.json","notices/stormtty-review/THIRD-PARTY-NOTICES.txt")
    if owned and previous.get("recipe")==recipe and all((runtime/p).is_file() for p in required): return
    runtime.parent.mkdir(parents=True,exist_ok=True)
    guard=runtime.with_name(runtime.name+".build-active")
    try: fd=os.open(guard,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600)
    except FileExistsError as error:
        raise RuntimeError(f"Shell preparation already active: {guard}. If interrupted, confirm no build is running before removing this guard.") from error
    try:
        with os.fdopen(fd,"w") as stream: stream.write(f"pid={os.getpid()}\n")
        with tempfile.TemporaryDirectory(prefix=".stormtty-build-shell-",dir=runtime.parent) as temp:
            root=Path(temp)/"runtime"; packet=Path(temp)/"packet"
            command=[sys.executable,str(TOOLS/"shell-bootstrap.py"),"--destination",str(root),"--cache",str(cache)]
            if offline: command.append("--offline")
            if zstd: command += ["--zstd",str(zstd)]
            subprocess.run(command,check=True)
            command=[sys.executable,str(TOOLS/"shell-notices.py"),"--runtime",str(root),"--cache",str(cache),"--output",str(packet)]
            if offline: command.append("--offline")
            subprocess.run(command,check=True)
            shutil.copytree(packet/"runtime-notices",root/"notices/stormtty-review")
            shutil.copyfile(packet/"packet-manifest.json",root/"notices/stormtty-review/packet-manifest.json")
            manifest_path=root/"stormtty-shell-manifest.json"
            manifest=json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["release_notices_complete"]=False
            manifest["base_runtime_notices_complete"]=True  # Tool static closure remains pending.
            manifest["notice_packet"]="notices/stormtty-review/packet-manifest.json"
            # This records actual attached texts only, not public source access
            # or a supported signed binary release.
            manifest_path.write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
            old=Path(temp)/"old"
            if runtime.exists(): runtime.rename(old)
            try: root.rename(runtime)
            except OSError:
                if old.exists(): old.rename(runtime)
                raise
            fresh_receipt=Path(temp)/"recipe.json"
            fresh_receipt.write_text(json.dumps({"schema":1,"build_owned":True,"recipe":recipe,"release_qualified":False})+"\n")
            fresh_receipt.replace(receipt)
        print(f"Prepared bundled upstream Zsh and notices for app build: {runtime}",flush=True)
    finally: guard.unlink(missing_ok=True)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime",type=Path,required=True)
    parser.add_argument("--cache",type=Path,required=True)
    parser.add_argument("--offline",action="store_true")
    parser.add_argument("--zstd",type=Path)
    args=parser.parse_args()
    try: prepare(args.runtime,args.cache,args.offline,args.zstd);return 0
    except (OSError,RuntimeError,ValueError,subprocess.CalledProcessError) as error:
        print(f"bundled-shell build: {error}",file=sys.stderr);return 1

if __name__=="__main__": raise SystemExit(main())
