#!/usr/bin/env python3
"""Offline validation/copy of an already staged qualification runtime. Never downloads."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import sys
import tempfile

MAX_FILES = 100_000
MAX_BYTES = 256 * 1024 * 1024
MANIFEST = "stormtty-shell-manifest.json"
RECEIPT = "stormtty-developer-runtime.json"
REQUIRED = ("usr/bin/zsh.exe", "usr/bin/msys-2.0.dll", "stormtty/zsh/.zshrc", "etc/fstab", "etc/nsswitch.conf", "stormtty/vendor/addons.lock.json", "stormtty/vendor/oh-my-zsh/oh-my-zsh.sh", "stormtty/vendor/zsh-autosuggestions/zsh-autosuggestions.zsh", "stormtty/vendor/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh")

CONFIG = {"etc/fstab": "shell-runtime.fstab", "etc/nsswitch.conf": "shell-runtime.nsswitch.conf", "stormtty/vendor/addons.lock.json": "shell-addons.lock.json"}

CONFIG.update({'stormtty/vendor/shell-tools/tools.lock.json': 'shell-tools.lock.json', 'stormtty/vendor/shell-tools/sources.lock.json': 'shell-tools-sources.lock.json', 'stormtty/vendor/shell-tools/init.zsh': 'shell-tools.zsh', 'stormtty/vendor/shell-tools/zoxide.zsh': 'shell-tools-zoxide.zsh'})
REQUIRED += tuple(name for name in CONFIG if name not in REQUIRED) + ("usr/bin/awk.exe", "usr/bin/gawk.exe", "usr/bin/msys-readline8.dll", "usr/bin/msys-mpfr-6.dll", "ucrt64/bin/fzf.exe", "ucrt64/bin/zoxide.exe", "ucrt64/share/fzf/completion.zsh", "ucrt64/share/fzf/key-bindings.zsh", "ucrt64/share/zsh/site-functions/_zoxide")

REQUIRED += ("usr/bin/nano.exe", "usr/bin/file.exe", "usr/bin/msys-magic-1.dll", "usr/bin/msys-bz2-1.dll", "usr/bin/msys-lzma-5.dll", "usr/share/misc/magic.mgc")

def validation_inputs(lock, preset):
    return [digest(lock), digest(preset), digest(Path(__file__))] + [digest(Path(__file__).with_name(name)) for name in CONFIG.values()]


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""): h.update(data)
    return h.hexdigest()


def regular(path):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
        raise RuntimeError(f"Expected a regular non-reparse file: {path}")
    return info


def relative(name):
    p = PurePosixPath(name)
    if not name or len(name) > 1024 or p.is_absolute() or "\\" in name or ":" in name or any(ord(c) < 32 for c in name):
        raise RuntimeError("Unsafe runtime manifest path")
    parts = name.split("/")
    if any(x in ("", ".", "..") or x.endswith((" ", ".")) or re.fullmatch(r"(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?", x) for x in parts):
        raise RuntimeError("Nonportable runtime manifest path")
    return p


def load(path, limit=16 * 1024 * 1024):
    if regular(path).st_size > limit: raise RuntimeError("Metadata exceeds its size bound")
    return json.loads(path.read_text(encoding="utf-8"))


def inventory(source):
    result = {}
    total = 0
    for base, directories, entries in os.walk(source, followlinks=False):
        for name in directories:
            info = (Path(base) / name).lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                raise RuntimeError("Runtime contains a linked/reparse directory")
        for name in entries:
            path = Path(base) / name
            rel = path.relative_to(source).as_posix()
            relative(rel)
            info = regular(path)
            total += info.st_size
            result[rel] = [info.st_size, info.st_mtime_ns]
            if total > MAX_BYTES or len(result) > MAX_FILES: raise RuntimeError("Runtime exceeds total byte/file bound")
    return result


def validate(source, lock_path, preset):
    if not source.is_dir() or source.is_symlink() or getattr(source.lstat(), "st_file_attributes", 0) & 0x400:
        raise RuntimeError("Runtime root must be a real non-reparse directory")
    snapshot = inventory(source)
    lock = load(lock_path)
    manifest = load(source / MANIFEST)
    if (manifest.get("schema") != 1 or manifest.get("bundle_id") != lock.get("bundle_id") or
        manifest.get("platform") != "windows-x86_64" or manifest.get("lock_sha256") != digest(lock_path) or
        manifest.get("qualification_only") != lock.get("qualification_only") or
        manifest.get("default_activation_allowed") != lock.get("default_activation_allowed") or
        manifest.get("shell") != REQUIRED[0] or manifest.get("managed_zdotdir") != "stormtty/zsh"):
        raise RuntimeError("Runtime manifest does not match the exact developer qualification lock")
    if manifest.get("managed_tools_lock_sha256") != digest(Path(__file__).with_name("shell-tools.lock.json")):
        raise RuntimeError("Runtime managed tools lock is stale")
    records = manifest.get("files", [])
    if not isinstance(records, list) or not 1 <= len(records) <= MAX_FILES: raise RuntimeError("Invalid runtime file inventory")
    names = {}
    for record in records:
        name = str(relative(record["path"]))
        if name.casefold() in names: raise RuntimeError("Duplicate runtime manifest path")
        names[name.casefold()] = name
    # Reject links before opening payload paths, including a linked parent of
    # a listed file. Bound the notice overlay as well as the manifest payload.
    files = []
    total = 0
    seen = set()
    for base, directories, entries in os.walk(source, followlinks=False):
        for name in directories:
            info = (Path(base) / name).lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                raise RuntimeError("Runtime contains a linked/reparse directory")
        for name in entries:
            path = Path(base) / name
            rel = path.relative_to(source).as_posix()
            relative(rel)
            total += regular(path).st_size
            if total > MAX_BYTES: raise RuntimeError("Runtime exceeds total byte bound")
            if rel.casefold() in seen: raise RuntimeError("Case-colliding runtime files")
            seen.add(rel.casefold())
            if rel != MANIFEST and rel.casefold() not in names and not rel.startswith("notices/"):
                raise RuntimeError(f"Unlisted runtime file: {rel}")
            files.append(path)
            if len(files) > MAX_FILES: raise RuntimeError("Runtime exceeds file-count bound")
    expected = {}
    for record in records:
        name = str(relative(record["path"]))
        path = source / name
        if regular(path).st_size != record["bytes"] or digest(path) != record["sha256"]:
            raise RuntimeError(f"Runtime file integrity check failed: {name}")
        expected[path] = record["sha256"]
    for name in REQUIRED:
        if name.casefold() not in names: raise RuntimeError(f"Required runtime file is absent: {name}")
    if digest(source / REQUIRED[2]) != digest(preset):
        raise RuntimeError("Managed preset is stale; stage a fresh runtime with the current shell-bootstrap.py")
    for name, asset in CONFIG.items():
        if digest(source / name) != digest(Path(__file__).with_name(asset)):
            raise RuntimeError("Managed runtime configuration is stale; stage a fresh runtime with shell-bootstrap.py")
    # Snapshot expected hashes before copy; a source changed between validation
    # and staging must fail rather than validating against its new bytes.
    for path in files:
        if path not in expected: expected[path] = digest(path)
    for notice in ("notices/THIRD-PARTY-NOTICES.txt", "notices/source-manifest.json", "notices/shell-bootstrap.lock.json"):
        if source / notice not in files: raise RuntimeError(f"Runtime notice is missing: {notice}")
    return expected, {"schema": 1, "bundle_id": manifest["bundle_id"], "manifest_sha256": digest(source / MANIFEST),
                           "build_owned_runtime": True, "release_qualified": False, "source_snapshot": snapshot,
                           "validation_inputs": validation_inputs(lock_path, preset)}


def stage(source, destination, files, receipt):
    # Derive inventory-relative names before canonicalizing. Windows temp paths
    # can be spelled with 8.3 aliases while resolve() returns long names; mixing
    # those representations can miss overlap or break relative_to().
    relative_files = {path.relative_to(source): expected for path, expected in files.items()}
    source = source.resolve(strict=True)
    destination = destination.absolute()
    if destination.is_symlink() or (destination.exists() and getattr(destination.lstat(), "st_file_attributes", 0) & 0x400):
        raise RuntimeError("Destination must not be a linked/reparse directory")
    destination = destination.resolve()
    if source == destination or source in destination.parents or destination in source.parents:
        raise RuntimeError("Source and app-owned destination must not overlap")
    if destination.exists():
        existing = load(destination / RECEIPT)
        if not (existing.get("build_owned_runtime") is True or existing.get("developer_opt_in") is True) or existing.get("schema") != 1:
            raise RuntimeError("Refusing to replace a destination not owned by build runtime staging")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".stormtty-runtime-", dir=destination.parent) as temp:
        fresh = Path(temp) / "new"; fresh.mkdir()
        for name, expected in relative_files.items():
            target = fresh / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / name, target)
            if digest(target) != expected: raise RuntimeError("Runtime changed after validation")
        (fresh / RECEIPT).write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        old = Path(temp) / "old"
        if destination.exists(): destination.rename(old)
        try: fresh.rename(destination)
        except OSError:
            if old.exists(): old.rename(destination)
            raise
    print(f"Staged bundled managed Zsh beside app: {destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--preset", type=Path, required=True)
    parser.add_argument("--destination", type=Path)
    parser.add_argument("--stamp", type=Path)
    parser.add_argument("--repair-if-needed", action="store_true", help="Fast output-presence check; validate/copy only if staging outputs disappeared")
    args = parser.parse_args()
    try:
        if not args.source.is_absolute(): raise RuntimeError("Configured runtime root must be absolute")
        if args.source.is_symlink() or getattr(args.source.lstat(), "st_file_attributes", 0) & 0x400:
            raise RuntimeError("Runtime root must not be linked/reparse")
        args.source = args.source.resolve(strict=True)
        if args.repair_if_needed:
            if not args.destination or not args.stamp: raise RuntimeError("Repair requires destination and stamp")
            if args.stamp.is_file() and all((args.destination / p).is_file() for p in (*REQUIRED, MANIFEST, RECEIPT)):
                receipt = load(args.destination / RECEIPT)
                if receipt.get("build_owned_runtime") is True and receipt.get("validation_inputs") == validation_inputs(args.lock, args.preset) and receipt.get("source_snapshot") == inventory(args.source):
                    return 0
        files, receipt = validate(args.source, args.lock, args.preset)
        if args.destination: stage(args.source, args.destination, files, receipt)
        if args.stamp: args.stamp.write_text(json.dumps(receipt) + "\n", encoding="utf-8")
        print(f"Validated {len(files)} staged runtime files; no download or release gate change")
        return 0
    except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
        print(f"bundled-shell: {error}. Restage with tools/shell-bootstrap.py, close StormTTY, then configure/build again.", file=sys.stderr)
        return 1

if __name__ == "__main__": raise SystemExit(main())
