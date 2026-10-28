#!/usr/bin/env python3
"""Cache the exact official Zstandard Windows x64 build tool, never install it.

Python 3.12+ stdlib only. No PATH lookup, global installation or app payload.
The official HTTPS release ZIP and selected files are independently hash-pinned.
Upstream does not publish a detached signature for this Windows ZIP.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import tempfile
import urllib.parse
import urllib.request
import zipfile

LOCK = Path(__file__).with_name("shell-zstd-bootstrap.lock.json")
ALLOWED_HOSTS = {"github.com", "release-assets.githubusercontent.com"}


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def safe_path(name):
    parts = name.rstrip("/").split("/")
    if (not name or len(name) > 512 or "\\" in name or ":" in name
            or any(ord(c) < 32 for c in name)
            or any(p in ("", ".", "..") or p.endswith((" ", "."))
                   or re.fullmatch(r"(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?", p)
                   for p in parts)):
        raise RuntimeError(f"Unsafe tool archive path: {name!r}")
    return parts


def regular(path):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
        raise RuntimeError(f"Expected a regular, non-reparse tool file: {path}")
    return info


def directory(path):
    for item in (path, *path.parents):
        info = item.lstat()
        if not stat.S_ISDIR(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise RuntimeError(f"Expected a real, non-reparse cache directory: {item}")


def checked(path, item):
    size = regular(path).st_size
    actual = digest(path)
    if size != item["bytes"] or actual != item["sha256"]:
        raise RuntimeError(f"Tool integrity check failed: {path}; expected SHA-256 {item['sha256']}, "
                           f"bytes {item['bytes']}; actual SHA-256 {actual}, bytes {size}; "
                           "remove explicitly before retrying")
    return path


def notice_bytes(path, item):
    """Recovery is only for pinned notice texts, never an executable/archive."""
    size = regular(path).st_size
    if size <= 2 * item["bytes"] + 3:
        raw = path.read_bytes()
        without_bom = raw.removeprefix(b"\xef\xbb\xbf")
        for candidate in (raw, raw.replace(b"\r\n", b"\n"), without_bom,
                          without_bom.replace(b"\r\n", b"\n")):
            if len(candidate) == item["bytes"] and hashlib.sha256(candidate).hexdigest() == item["sha256"]:
                return candidate
        actual = hashlib.sha256(raw).hexdigest()
        size = len(raw)
    else:
        actual = digest(path)
    raise RuntimeError(f"Tool notice integrity check failed: {path}; expected SHA-256 {item['sha256']}, "
                       f"bytes {item['bytes']}; actual SHA-256 {actual}, bytes {size}")


def cache_notice(source, target, item):
    data = notice_bytes(source, item)
    if target.exists() or target.is_symlink():
        regular(target)
        if target.read_bytes() == data:
            return
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".zstd-notice-", dir=target.parent, delete=False) as output:
            temporary = Path(output.name)
            output.write(data)
        checked(temporary, item)
        temporary.replace(target)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def read_lock(path=LOCK):
    lock = json.loads(path.read_text(encoding="utf-8"))
    if lock.get("schema") != 1 or lock.get("platform") != "windows-x86_64":
        raise RuntimeError("Unsupported decompressor lock")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", lock["version"]):
        raise RuntimeError("Invalid decompressor version")
    filename = f"zstd-v{lock['version']}-win64.zip"
    archive = lock["archive"]
    if (archive["filename"] != filename or archive["url"] !=
            f"https://github.com/facebook/zstd/releases/download/v{lock['version']}/{filename}"):
        raise RuntimeError("Decompressor origin must be the exact official release asset")
    if {x["target"] for x in lock["members"]} != {"zstd.exe", "README.md"} or len(lock["members"]) != 2:
        raise RuntimeError("Unexpected decompressor extraction set")
    for item in [archive, *lock["members"], *lock["notices"]]:
        if (not re.fullmatch(r"[0-9a-f]{64}", item["sha256"])
                or type(item["bytes"]) is not int or not 0 < item["bytes"] <= 8 * 1024 * 1024):
            raise RuntimeError("Invalid decompressor size/hash pin")
    for item in lock["members"]:
        if item["member"] != filename[:-4] + "/" + item["target"]:
            raise RuntimeError("Unexpected decompressor ZIP member")
    if {x["target"] for x in lock["notices"]} != {"LICENSE", "COPYING"} or len(lock["notices"]) != 2:
        raise RuntimeError("Missing decompressor license texts")
    for item in lock["notices"]:
        if item["source"] != "shell-notices/texts/zstd/" + item["target"]:
            raise RuntimeError("Unexpected decompressor license source")
    return lock


def https_origin(url):
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https" or parsed.netloc not in ALLOWED_HOSTS or parsed.username:
        raise RuntimeError("Decompressor download left allowed official HTTPS origins")


class OfficialRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        https_origin(newurl)
        return super().redirect_request(request, fp, code, msg, headers, newurl)


def download(item, cache, offline):
    target = cache / item["filename"]
    if target.exists() or target.is_symlink():
        return checked(target, item)
    if offline:
        raise RuntimeError(f"Offline decompressor archive missing: {target}")
    https_origin(item["url"])
    opener = urllib.request.build_opener(OfficialRedirect())
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".zstd-download-", suffix=".part", dir=cache, delete=False) as output:
            temporary = Path(output.name)
            request = urllib.request.Request(item["url"], headers={"User-Agent": "StormTTY-pinned-build-bootstrap"})
            with opener.open(request, timeout=120) as response:
                https_origin(response.geturl())
                received = 0
                while block := response.read(1024 * 1024):
                    received += len(block)
                    if received > item["bytes"]:
                        raise RuntimeError("Decompressor ZIP exceeds its pinned byte count")
                    output.write(block)
        checked(temporary, item)
        temporary.replace(target)
        print(f"Verified build tool {item['filename']}", file=sys.stderr, flush=True)
        return target
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def extract(archive, destination, lock):
    """Inspect every path; write only two hash-locked ordinary files ourselves."""
    with zipfile.ZipFile(archive) as bundle:
        entries = bundle.infolist()
        if len(entries) > 128 or sum(x.file_size for x in entries) > 20 * 1024 * 1024:
            raise RuntimeError("Decompressor ZIP exceeds extraction limits")
        seen = set()
        by_name = {}
        for entry in entries:
            # ZipInfo normalizes os.sep on Windows and truncates NULs. Validate
            # the original central-directory name before trusting that alias.
            safe_path(entry.orig_filename)
            safe_path(entry.filename)
            key = entry.filename.rstrip("/").casefold()
            kind = stat.S_IFMT(entry.external_attr >> 16)
            if (key in seen or entry.flag_bits & 1 or entry.file_size < 0
                    or entry.file_size > 8 * 1024 * 1024
                    or kind not in (0, stat.S_IFREG, stat.S_IFDIR)
                    or (kind == stat.S_IFDIR) != entry.is_dir() and kind != 0):
                raise RuntimeError(f"Unsafe/duplicate decompressor ZIP entry: {entry.filename}")
            seen.add(key)
            by_name[entry.filename] = entry
        for item in lock["members"]:
            entry = by_name.get(item["member"])
            if entry is None or entry.is_dir() or entry.file_size != item["bytes"]:
                raise RuntimeError("Pinned decompressor ZIP member missing or wrong size")
            target = destination / item["target"]
            with bundle.open(entry) as source, target.open("xb") as output:
                shutil.copyfileobj(source, output, 1024 * 1024)
            checked(target, item)


def provision(cache, offline=False, lock_path=LOCK):
    lock = read_lock(lock_path)
    cache = cache.absolute()
    cache.mkdir(parents=True, exist_ok=True)
    directory(cache)
    tools = cache / "tools"
    tools.mkdir(exist_ok=True)
    directory(tools)
    destination = tools / lock["archive"]["filename"][:-4]
    guard = tools / ".zstd-bootstrap.active"
    try:
        fd = os.open(guard, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError as error:
        raise RuntimeError(f"Decompressor staging already active: {guard}; check process before removing stale guard") from error
    try:
        with os.fdopen(fd, "w") as output:
            output.write(f"pid={os.getpid()}\n")
        archive = download(lock["archive"], cache, offline)
        if destination.exists() or destination.is_symlink():
            directory(destination)
            for item in lock["members"]:
                checked(destination / item["target"], item)
            for item in lock["notices"]:
                path = destination / item["target"]
                cache_notice(path, path, item)
            if {p.name for p in destination.iterdir()} != {x["target"] for x in lock["members"] + lock["notices"]}:
                raise RuntimeError("Unexpected file in private decompressor directory")
            return destination / "zstd.exe"
        with tempfile.TemporaryDirectory(prefix=".zstd-stage-", dir=tools) as temp:
            fresh = Path(temp) / "tool"
            fresh.mkdir()
            extract(archive, fresh, lock)
            for item in lock["notices"]:
                source = Path(__file__).parent / item["source"]
                cache_notice(source, fresh / item["target"], item)
            fresh.rename(destination)
        return destination / "zstd.exe"
    finally:
        guard.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--offline", action="store_true")
    try:
        args = parser.parse_args()
        print(provision(args.cache, args.offline))
        return 0
    except (OSError, ValueError, KeyError, RuntimeError, zipfile.BadZipFile) as error:
        print(f"shell-zstd-bootstrap: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
