#!/usr/bin/env python3
"""Stage exact official Windows Zsh packages, without installing or executing them.

Windows x64: Python 3.12+; automatically caches the pinned official zstd build
tool when stdlib compression.zstd is unavailable. No pip/global installs.
Public redistribution additionally requires matching notices/source delivery.
"""
from __future__ import annotations
import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request

LOCK = Path(__file__).with_name("shell-bootstrap.lock.json")
METADATA = {".PKGINFO", ".BUILDINFO", ".MTREE", ".INSTALL"}
MAX_FILES = 100_000
MAX_MEMBER_BYTES = 64 * 1024 * 1024
MAX_PAYLOAD_BYTES = 256 * 1024 * 1024
MAX_TAR_BYTES = 512 * 1024 * 1024
RESERVED = re.compile(r"(?i)(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$")


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def portable_path(name: str) -> PurePosixPath:
    """Reject traversal and Windows aliases/ADS even when staging on Linux."""
    if not name or len(name) > 1024 or any(ord(c) < 32 for c in name):
        raise RuntimeError(f"Invalid archive path: {name!r}")
    if "\\" in name or ":" in name or name.startswith("/"):
        raise RuntimeError(f"Unsafe archive path: {name!r}")
    parts = name.rstrip("/").split("/")
    if any(p in ("", ".", "..") or p.endswith((" ", ".")) or RESERVED.fullmatch(p)
           for p in parts):
        raise RuntimeError(f"Non-portable archive path: {name!r}")
    return PurePosixPath(*parts)


def read_lock(path: Path) -> dict:
    lock = json.loads(path.read_text(encoding="utf-8"))
    if (lock.get("schema") != 1 or lock.get("platform") != "windows-x86_64"
            or lock.get("qualification_only") is not False
            or lock.get("default_activation_allowed") is not True
            or lock.get("public_redistribution_ready") is not False
            or lock.get("native_qualification", {}).get("status") != "passed"):
        raise RuntimeError("Unsupported or unqualified shell lock")
    portable_path(lock["bundle_id"])
    filenames: set[str] = set()
    for item in lock["packages"] + lock["sources"]:
        filename = item["filename"]
        if len(portable_path(filename).parts) != 1 or filename in filenames:
            raise RuntimeError("Invalid/duplicate locked filename")
        filenames.add(filename)
        url = urllib.parse.urlsplit(item["url"])
        if (url.scheme != "https" or url.netloc != "repo.msys2.org"
                or not url.path.startswith("/msys/") or url.query or url.fragment
                or url.path.rsplit("/", 1)[-1] != filename):
            raise RuntimeError("Package URL is not an exact official MSYS2 URL")
        if not re.fullmatch(r"[0-9a-f]{64}", item["sha256"]):
            raise RuntimeError("Invalid SHA-256 pin")
        if not isinstance(item["bytes"], int) or not 0 < item["bytes"] <= 512 * 1024 * 1024:
            raise RuntimeError("Invalid archive size pin")
    sources = {s["filename"] for s in lock["sources"]}
    names = set()
    for package in lock["packages"]:
        if (not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", package["name"])
                or package["name"] in names or package["source"] not in sources):
            raise RuntimeError("Invalid package identity/source mapping")
        names.add(package["name"])
    return lock


def download(item: dict, cache: Path, offline: bool) -> Path:
    target = cache / item["filename"]
    if target.is_symlink():
        raise RuntimeError(f"Refusing symlink in archive cache: {target}")
    if target.exists():
        if not target.is_file() or target.stat().st_size != item["bytes"] or digest(target) != item["sha256"]:
            raise RuntimeError(f"Cached archive failed integrity check: {target}; remove explicitly before retrying")
        return target
    if offline:
        raise RuntimeError(f"Offline archive missing: {target}")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".shell-download-", suffix=".part", dir=cache, delete=False) as output:
            temporary = Path(output.name)
            with urllib.request.urlopen(item["url"], timeout=120) as response:
                final = urllib.parse.urlsplit(response.geturl())
                if final.scheme != "https" or final.netloc != "repo.msys2.org":
                    raise RuntimeError("Official repository redirected outside the allowed HTTPS origin")
                received = 0
                while block := response.read(1024 * 1024):
                    received += len(block)
                    if received > item["bytes"]:
                        raise RuntimeError("Downloaded archive exceeds its pinned byte count")
                    output.write(block)
        if temporary.stat().st_size != item["bytes"] or digest(temporary) != item["sha256"]:
            raise RuntimeError(f"Downloaded archive failed integrity check: {item['filename']}")
        temporary.replace(target)
        print(f"Verified {item['filename']}", flush=True)
        return target
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


class LimitedReader:
    def __init__(self, stream):
        self.stream = stream
        self.bytes_read = 0

    def read(self, size=-1):
        if size < 0:
            size = 1024 * 1024
        block = self.stream.read(size)
        self.bytes_read += len(block)
        if self.bytes_read > MAX_TAR_BYTES:
            raise RuntimeError("Decompressed package exceeds the tar byte limit")
        return block


def stdlib_zstd_available():
    try:
        from compression import zstd  # noqa: F401 - availability only
        return True
    except ImportError:
        return False


def select_decompressor(explicit, cache, offline):
    if explicit or stdlib_zstd_available():
        return explicit
    if sys.platform == "win32" and platform.machine().lower() in ("amd64", "x86_64"):
        path = Path(__file__).with_name("shell-zstd-bootstrap.py")
        spec = importlib.util.spec_from_file_location("stormtty_shell_zstd_bootstrap", path)
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        return str(helper.provision(cache, offline))
    raise RuntimeError("This host needs Python 3.14+ compression.zstd or --zstd /absolute/trusted/zstd; "
                       "automatic pinned build-tool provisioning supports Windows x64")


@contextmanager
def open_package(archive: Path, zstd: str | None):
    process = None
    stream = None
    try:
        if zstd:
            executable = Path(zstd).resolve(strict=True)
            if not executable.is_file():
                raise RuntimeError("--zstd must name an existing trusted executable")
            process = subprocess.Popen([str(executable), "-d", "-c", "--", str(archive)],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            stream = process.stdout
        else:
            try:
                from compression import zstd as stdlib_zstd
            except ImportError as error:
                raise RuntimeError("Use Python 3.14+ with compression.zstd, or pass --zstd /absolute/trusted/zstd") from error
            stream = stdlib_zstd.open(archive, "rb")
        bounded = LimitedReader(stream)
        with tarfile.open(fileobj=bounded, mode="r|") as bundle:
            yield bundle
        # Consume the trailer so decompressor failure is not mistaken for success.
        while bounded.read(1024 * 1024):
            pass
        if process and process.wait(timeout=30) != 0:
            raise RuntimeError(f"Zstandard decompression failed: {archive.name}")
    finally:
        if stream is not None:
            stream.close()
        if process:
            if process.poll() is None:
                process.kill()
            process.wait()
            process.stderr.close()


def extract_package(bundle: tarfile.TarFile, destination: Path, package: str,
                    records: dict[str, dict], budget: dict[str, int]) -> None:
    """Write regular files ourselves; never apply tar ownership/links/install hooks."""
    for member in bundle:
        budget["entries"] += 1
        if budget["entries"] > MAX_FILES:
            raise RuntimeError("Package set exceeds entry limit")
        relative = portable_path(member.name)
        if not (member.isdir() or member.isfile() or member.islnk()) or member.issparse():
            raise RuntimeError(f"Unsupported archive entry: {member.name}")
        if member.isfile() and relative.parts[0].startswith("."):
            if len(relative.parts) != 1 or member.name not in METADATA:
                raise RuntimeError(f"Unknown package metadata: {member.name}")
            relative = PurePosixPath(".stormtty-packages", package, member.name)
        elif relative.parts[0] not in ("usr", "etc"):
            raise RuntimeError(f"Unexpected runtime payload root: {member.name}")
        name = relative.as_posix()
        key = name.casefold()
        previous = records.get(key)
        if previous and (previous["path"] != name or previous["directory"] != member.isdir()):
            raise RuntimeError(f"Case/type collision in package set: {member.name}")
        # Record implicit parent directories too, avoiding Windows case aliases.
        for index in range(1, len(relative.parts)):
            parent_name = PurePosixPath(*relative.parts[:index]).as_posix()
            parent_key = parent_name.casefold()
            parent_record = records.get(parent_key)
            if parent_record and (parent_record["path"] != parent_name or not parent_record["directory"]):
                raise RuntimeError(f"Parent case/type collision: {member.name}")
            records.setdefault(parent_key, {"path": parent_name, "directory": True})
        target = destination.joinpath(*relative.parts)
        if member.isdir():
            target.mkdir(parents=True, exist_ok=True)
            records.setdefault(key, {"path": name, "directory": True})
            continue
        if member.islnk():
            source_name = portable_path(member.linkname).as_posix()
            source_record = records.get(source_name.casefold())
            if (not source_record or source_record["path"] != source_name
                    or source_record["directory"] or source_record.get("package") != package):
                raise RuntimeError(f"Hardlink target is not an earlier verified same-package file: {member.name}")
            if previous or target.exists():
                raise RuntimeError(f"Duplicate hardlink path: {member.name}")
            budget["bytes"] += source_record["bytes"]
            if budget["bytes"] > MAX_PAYLOAD_BYTES:
                raise RuntimeError("Hardlinks exceed payload byte limit")
            target.parent.mkdir(parents=True, exist_ok=True)
            # Copy bytes, NOT os.link: portable on Windows without link privileges.
            shutil.copyfile(destination / source_name, target)
            if os.name != "nt":
                target.chmod(0o755 if member.mode & 0o111 else 0o644)
            records[key] = {"path": name, "directory": False, "bytes": source_record["bytes"],
                            "sha256": source_record["sha256"], "package": package,
                            "archive_hardlink_materialized_from": source_name}
            continue
        if member.size < 0 or member.size > MAX_MEMBER_BYTES:
            raise RuntimeError(f"File exceeds member byte limit: {member.name}")
        budget["bytes"] += member.size
        if budget["bytes"] > MAX_PAYLOAD_BYTES:
            raise RuntimeError("Package set exceeds payload byte limit")
        target.parent.mkdir(parents=True, exist_ok=True)
        source = bundle.extractfile(member)
        if source is None:
            raise RuntimeError("Regular file has no readable data")
        # Reject even identical file collisions: this lock has no shared payload files.
        if previous or target.exists():
            raise RuntimeError(f"Duplicate file in package set: {member.name}")
        h = hashlib.sha256()
        remaining = member.size
        with source, target.open("xb") as output:
            while remaining:
                block = source.read(min(1024 * 1024, remaining))
                if not block:
                    raise RuntimeError(f"Truncated tar member: {member.name}")
                h.update(block)
                output.write(block)
                remaining -= len(block)
        if os.name != "nt":
            target.chmod(0o755 if member.mode & 0o111 else 0o644)
        records[key] = {"path": name, "directory": False, "bytes": member.size,
                        "sha256": h.hexdigest(), "package": package}


def stage_runtime_config(root: Path, records: dict[str, dict]) -> None:
    # Add only app-owned files. Never replace or reclassify upstream payloads.
    parent = records.get("etc")
    if parent and (parent["path"] != "etc" or not parent["directory"]):
        raise RuntimeError("Runtime configuration collides with package etc directory")
    for source_name, name in (("shell-runtime.fstab", "etc/fstab"),
                              ("shell-runtime.nsswitch.conf", "etc/nsswitch.conf")):
        source = Path(__file__).with_name(source_name)
        target = root / name
        if name.casefold() in records or target.exists() or target.is_symlink():
            raise RuntimeError(f"Runtime configuration collides with package payload: {name}")
        target.parent.mkdir(parents=True, exist_ok=True)
        records.setdefault("etc", {"path": "etc", "directory": True})
        shutil.copyfile(source, target)
        records[name.casefold()] = {"path": name, "directory": False,
            "bytes": source.stat().st_size, "sha256": digest(source), "package": "stormtty-original-config"}


def write_notice_packet(destination: Path, lock: dict, lock_hash: str, sources: bool,
                        records: dict[str, dict]) -> None:
    notices = destination / "notices"
    notices.mkdir()
    text = ["StormTTY Zsh private runtime - QUALIFICATION ONLY", "",
            "This is a collection of independent upstream programs, not a renamed Zsh fork.",
            "All upstream files/notices are retained. No package install script was executed.",
            "The pinned runtime has passed native Windows qualification for build-default use.",
            "Public distribution still requires the reviewed notice overlay and matching source delivery.",
            "Package license tags below are not a substitute for the reviewed per-component packet.",
            "", "Required release work:",
            "- Preserve Zsh's actual permissive license; its _darcs completion is GPL-2.0-or-later.",
            "- Preserve runtime COPYING/CYGWIN_LICENSE, LGPL text, GCC runtime exception and",
            "  all other component-specific notices. Some binary packages omit common license texts.",
            "- Publish matching complete source, recipes and modifications beside each binary",
            "  release (GPLv3 section 6(d) is the intended route); upstream links alone are not enough.",
            "- No Ghostty GPL shell-integration scripts have been copied into this bundle.", "",
            "Exact package/source inventory:"]
    source_map = {s["filename"]: s for s in lock["sources"]}
    for package in lock["packages"]:
        source = source_map[package["source"]]
        text += [f"{package['name']} {package['version']}", f"  Declared license: {package['license']}",
                 f"  Binary: {package['url']}", f"  Binary SHA-256: {package['sha256']}",
                 f"  Source: {source['url']}", f"  Source SHA-256: {source['sha256']}", ""]
    (notices / "THIRD-PARTY-NOTICES.txt").write_text("\n".join(text), encoding="utf-8")
    (notices / "source-manifest.json").write_text(json.dumps(lock["sources"], indent=2) + "\n", encoding="utf-8")
    (notices / "shell-bootstrap.lock.json").write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")
    manifest = {"schema": 1, "bundle_id": lock["bundle_id"], "platform": lock["platform"],
                "qualification_only": lock["qualification_only"],
                "default_activation_allowed": lock["default_activation_allowed"],
                "native_qualification": lock["native_qualification"],
                "public_redistribution_ready": False, "public_source_availability_verified": False,
                "release_notices_complete": False, "source_archives_verified_in_cache": sources,
                "lock_sha256": lock_hash, "shell": "usr/bin/zsh.exe", "managed_zdotdir": "stormtty/zsh",
                "files": sorted((x for x in records.values() if not x["directory"]), key=lambda x: x["path"])}
    (destination / "stormtty-shell-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def stage(args) -> Path:
    lock = read_lock(args.lock)
    destination = args.destination.absolute()
    if destination.exists() or destination.is_symlink():
        raise RuntimeError(f"Destination must not exist: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    guard = cache / "shell-bootstrap.active"
    try:
        fd = os.open(guard, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError as error:
        raise RuntimeError(f"Staging already active: {guard}; verify no process is active before removing a stale guard") from error
    try:
        with os.fdopen(fd, "w") as f:
            f.write(f"pid={os.getpid()}\n")
        zstd = select_decompressor(args.zstd, cache, args.offline)
        archives = [(package, download(package, cache, args.offline)) for package in lock["packages"]]
        if args.sources:
            for source in lock["sources"]:
                download(source, cache, args.offline)
        with tempfile.TemporaryDirectory(prefix=".stormtty-shell-stage-", dir=destination.parent) as temporary:
            root = Path(temporary) / "runtime"
            root.mkdir()
            records: dict[str, dict] = {}
            budget = {"entries": 0, "bytes": 0}
            for package, archive in archives:
                print(f"Extracting {package['name']} {package['version']}", flush=True)
                with open_package(archive, zstd) as bundle:
                    extract_package(bundle, root, package["name"], records, budget)
            for name in ("usr/bin/zsh.exe", "usr/bin/msys-2.0.dll", "usr/bin/sleep.exe"):
                if not (root / name).is_file():
                    raise RuntimeError(f"Missing required runtime file: {name}")
            preset = Path(__file__).with_name("shell-preset.zsh")
            preset_target = root / "stormtty/zsh/.zshrc"
            preset_target.parent.mkdir(parents=True)
            shutil.copyfile(preset, preset_target)
            records["stormtty/zsh/.zshrc"] = {"path": "stormtty/zsh/.zshrc", "directory": False,
                "bytes": preset.stat().st_size, "sha256": digest(preset), "package": "stormtty-original-preset"}
            stage_runtime_config(root, records)
            write_notice_packet(root, lock, digest(args.lock), args.sources, records)
            if destination.exists() or destination.is_symlink():
                raise RuntimeError("Destination appeared while staging; refusing overwrite")
            root.rename(destination)
        print(f"Staged {destination / 'usr/bin/zsh.exe'}", flush=True)
        print("Build-default qualified. Public distribution requires the notice overlay and matching source delivery.", flush=True)
        return destination
    finally:
        guard.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, required=True, help="fresh private runtime directory; never overwritten")
    parser.add_argument("--cache", type=Path, required=True, help="workspace-owned archive cache")
    parser.add_argument("--lock", type=Path, default=LOCK)
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--sources", action="store_true", help="also download/verify complete source archives into cache")
    parser.add_argument("--zstd", help="explicit trusted decompressor override; Windows x64 auto-provisions when needed")
    try:
        stage(parser.parse_args())
        return 0
    except (OSError, ValueError, RuntimeError, tarfile.TarError) as error:
        print(f"shell-bootstrap: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
