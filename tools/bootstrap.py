#!/usr/bin/env python3
"""Build exact-pinned libghostty-vt without changing its source or integrity pins.

Python 3.10+, Git, and Zig 0.17.0 are required. --install-zig explicitly opts
into installing a checksum-verified official Zig archive under .deps/tools.
Native Windows also requires an MSVC developer shell and Windows SDK; macOS
requires Xcode Command Line Tools. This does not install those SDKs.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
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
import urllib.request
import zipfile


ROOT = Path(__file__).resolve().parents[1]
LOCK = Path(__file__).with_name("bootstrap.lock.json")


@contextmanager
def cache_lock(cache: Path):
    """Do not concurrently rewrite package archives or run expensive builds."""
    path = cache / "bootstrap.active"
    try:
        descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError:
        raise RuntimeError(f"Bootstrap already active: {path}. If stale, verify the process has stopped before removing it") from None
    try:
        with os.fdopen(descriptor, "w") as stream:
            stream.write(f"pid={os.getpid()}\n")
        yield
    finally:
        path.unlink(missing_ok=True)


def run(command: list[str], *, cwd: Path | None = None,
        env: dict[str, str] | None = None, capture: bool = False) -> str:
    print("+ " + subprocess.list2cmdline(command), flush=True)
    result = subprocess.run(command, cwd=cwd, env=env, check=True, text=True,
                            stdout=subprocess.PIPE if capture else None)
    return result.stdout.strip() if capture else ""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def download(url: str, destination: Path, expected: str, offline: bool) -> None:
    """Never use partial bytes or silently replace an unexpected existing file."""
    if destination.is_file():
        if sha256(destination) != expected:
            raise RuntimeError(f"SHA256 mismatch: {destination}; remove it explicitly before retrying")
        return
    if offline:
        raise RuntimeError(f"Offline input missing: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_suffix(destination.suffix + ".part")
    try:
        if shutil.which("curl"):
            run(["curl", "--fail", "--location", "--proto", "=https", "--tlsv1.2",
                 "--retry", "2", "--connect-timeout", "20", "--max-time", "300",
                 "--output", str(partial), url])
        else:
            with urllib.request.urlopen(url, timeout=300) as response, partial.open("wb") as out:
                shutil.copyfileobj(response, out)
        if sha256(partial) != expected:
            raise RuntimeError(f"SHA256 mismatch downloading {url}")
        partial.replace(destination)
    finally:
        partial.unlink(missing_ok=True)


def host() -> tuple[str, str]:
    machine = platform.machine().lower()
    arch = {"amd64": "x86_64", "x86_64": "x86_64", "arm64": "aarch64",
            "aarch64": "aarch64"}.get(machine)
    system = {"Windows": "windows", "Darwin": "macos", "Linux": "linux"}.get(platform.system())
    if not arch or not system:
        raise RuntimeError(f"Unsupported bootstrap host: {platform.system()} {machine}")
    return arch, system


def extract_tool(archive: Path, destination: Path) -> None:
    # Archives are official/checksum-verified, but still reject traversal and
    # links rather than trusting an archiver's platform-dependent defaults.
    def safe(name: str) -> None:
        path = PurePosixPath(name.replace("\\", "/"))
        if path.is_absolute() or ".." in path.parts or ":" in name:
            raise RuntimeError(f"Unsafe archive path: {name}")

    if archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as bundle:
            for member in bundle.infolist():
                safe(member.filename)
                if (member.external_attr >> 16) & 0o170000 == 0o120000:
                    raise RuntimeError("Tool archive contains a symbolic link")
            bundle.extractall(destination)
    else:
        with tarfile.open(archive) as bundle:
            for member in bundle.getmembers():
                safe(member.name)
                if not (member.isfile() or member.isdir()):
                    raise RuntimeError(f"Unsupported tool archive entry: {member.name}")
            if hasattr(tarfile, "data_filter"):
                bundle.extractall(destination, filter="data")
            else:
                bundle.extractall(destination)


def resolve_zig(args: argparse.Namespace, lock: dict, cache: Path) -> Path:
    candidate = args.zig or os.environ.get("ZIG") or shutil.which("zig")
    if not candidate and args.install_zig:
        arch, system = host()
        pin = lock["zig"]["archives"][f"{arch}-{system}"]
        archive = cache / "archives" / pin["url"].rsplit("/", 1)[1]
        download(pin["url"], archive, pin["sha256"], args.offline)
        tool_root = ROOT / ".deps" / "tools"
        executable = tool_root / f"zig-{arch}-{system}-{lock['zig']['version']}" / (
            "zig.exe" if system == "windows" else "zig")
        if not executable.exists():
            tool_root.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix="zig-install-", dir=tool_root) as directory:
                unpacked = Path(directory)
                extract_tool(archive, unpacked)
                (unpacked / executable.parent.name).replace(executable.parent)
        candidate = str(executable)
    if not candidate:
        raise RuntimeError("Zig missing; provide --zig PATH or opt into --install-zig")
    path = Path(shutil.which(candidate) or candidate).expanduser().resolve()
    actual = run([str(path), "version"], capture=True)
    if actual != lock["zig"]["version"]:
        raise RuntimeError(f"Expected Zig {lock['zig']['version']}; found {actual}")
    return path


def resolve_source(args: argparse.Namespace, lock: dict) -> Path:
    source = (args.source or ROOT / ".deps" / "ghostty-source").resolve()
    pin = lock["ghostty"]
    if not source.exists():
        if args.offline:
            raise RuntimeError(f"Offline source missing: {source}")
        source.mkdir(parents=True)
        run(["git", "init", str(source)])
        run(["git", "-C", str(source), "remote", "add", "origin", pin["repository"]])
        run(["git", "-C", str(source), "fetch", "--depth", "1", "origin", pin["commit"]])
        run(["git", "-C", str(source), "checkout", "--detach", "FETCH_HEAD"])
    actual = run(["git", "-C", str(source), "rev-parse", "HEAD"], capture=True)
    if actual != pin["commit"]:
        raise RuntimeError(f"Ghostty must be pinned to {pin['commit']}; found {actual}")
    if run(["git", "-C", str(source), "status", "--porcelain", "--untracked-files=all"], capture=True):
        raise RuntimeError(f"Refusing modified Ghostty source: {source}")
    return source


def prepare_dependencies(lock: dict, zig: Path, cache: Path,
                         env: dict[str, str], offline: bool) -> None:
    # In 0.17.0 --save activates local package storage; --pkg-dir alone does
    # not. Without --save, tarball fetch can recompress a double directory
    # prefix and leave temporary source trees. Save ONLY to a throwaway
    # manifest, never to Ghostty's manifest. The normal Zig content-hash
    # calculation and correctly rooted recompression remain unchanged.
    with tempfile.TemporaryDirectory(prefix="verify-", dir=cache) as directory:
        verification = Path(directory)
        (verification / "build.zig").touch()
        manifest = verification / "build.zig.zon"
        for pin in lock["dependencies"]:
            archive = cache / "archives" / pin["url"].rsplit("/", 1)[1]
            download(pin["url"], archive, pin["archive_sha256"], offline)
            manifest.unlink(missing_ok=True)
            run([str(zig), "fetch", "--save=verified", str(archive)], cwd=verification, env=env)
            hashes = re.findall(r'\.hash\s*=\s*"([^"]+)"', manifest.read_text())
            if hashes != [pin["zig_hash"]]:
                raise RuntimeError(f"Zig package hash mismatch for {pin['name']}: {hashes}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig", help="Existing exact-version Zig executable")
    parser.add_argument("--install-zig", action="store_true", help="Allow verified official Zig installation")
    parser.add_argument("--source", type=Path, help="Existing clean exact-pinned Ghostty checkout")
    parser.add_argument("--cache", type=Path, default=ROOT / ".cache" / "bootstrap")
    parser.add_argument("--prefix", type=Path, help="Install prefix (default .deps/ghostty-vt/TARGET)")
    parser.add_argument("--target", help="Zig triple; default native host (MSVC on Windows)")
    parser.add_argument("--optimize", choices=["Debug", "ReleaseSafe", "ReleaseFast", "ReleaseSmall"], default="ReleaseSafe")
    parser.add_argument("--jobs", type=int, default=2, help="Bound Zig job count (default 2)")
    parser.add_argument("--no-simd", action="store_true", help="Diagnostic baseline only; not the shipping configuration")
    parser.add_argument("--offline", action="store_true", help="Require already-downloaded source and archives")
    parser.add_argument("--fetch-only", action="store_true", help="Verify/cache needed dependencies without compiling the library")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    lock = json.loads(LOCK.read_text())
    cache = args.cache.expanduser().resolve()
    cache.mkdir(parents=True, exist_ok=True)
    arch, system = host()
    target = args.target or f"{arch}-{system}" + ({"windows": "-msvc", "linux": "-gnu"}.get(system, ""))
    if any(char in target for char in "/\\:") or target in {".", ".."}:
        parser.error("--target must be a Zig target triple, not a path")
    if "windows-msvc" in target and system != "windows":
        raise RuntimeError("MSVC requires a native Windows SDK/developer environment; GNU cross-compilation is compile-only")
    if "macos" in target and system != "macos":
        raise RuntimeError("macOS verification requires an Apple SDK/macOS host; this bootstrap does not redistribute SDKs")
    temporary = cache / "tmp"
    temporary.mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(temporary), TMP=str(temporary), TEMP=str(temporary),
               ZIG_GLOBAL_CACHE_DIR=str(cache / "zig-global"),
               ZIG_LOCAL_PKG_DIR=str(cache / "zig-packages"),
               ZIG_LOCAL_CACHE_DIR=str(cache / "build" / target))
    # Tool discovery is lightweight. Zig's build/fetch cache never uses $HOME.
    with cache_lock(cache):
        zig = resolve_zig(args, lock, cache)
        source = resolve_source(args, lock)
        prepare_dependencies(lock, zig, cache, env, args.offline)
        prefix = (args.prefix or ROOT / ".deps" / "ghostty-vt" / target).resolve()
        flags = ["-Demit-lib-vt", "-Demit-exe=false", "-Demit-macos-app=false",
                 "-Demit-xcframework=false", "-Demit-docs=false",
                 f"-Dtarget={target}", "-Dcpu=baseline",
                 f"-Doptimize={args.optimize}", f"-Dsimd={'false' if args.no_simd else 'true'}", f"-j{args.jobs}"]
        command = [str(zig), "build", *flags, "--prefix", str(prefix)]
        run([*command, "--fetch=needed"] if args.fetch_only else command, cwd=source, env=env)
        if args.fetch_only:
            print("Pinned dependency tree verified and cached; compilation not requested.")
            return 0
        metadata = {"schema": 1, "ghostty_commit": lock["ghostty"]["commit"],
                    "zig_version": lock["zig"]["version"], "target": target,
                    "host": f"{arch}-{system}", "optimize": args.optimize, "simd": not args.no_simd,
                    "python": platform.python_version(), "flags": flags,
                    "runtime_verified": False, "artifacts": {}}
        for directory in (prefix / "lib", prefix / "bin"):
            if directory.exists():
                for artifact in sorted(directory.iterdir()):
                    if artifact.is_file() and not artifact.is_symlink():
                        metadata["artifacts"][artifact.relative_to(prefix).as_posix()] = sha256(artifact)
        (prefix / "stormtty-build.json").write_text(json.dumps(metadata, indent=2) + "\n")
        notices = prefix / "share" / "licenses" / "ghostty"
        notices.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / "LICENSE", notices / "LICENSE")
        shutil.copyfile(LOCK, prefix / "stormtty-bootstrap.lock.json")
        print(f"Built {target}; native runtime/UI tests remain separate gates.\n"
              f"Configure StormTTY with -DSTORMTTY_GHOSTTY_ROOT={prefix}")
        return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"bootstrap: {error}", file=sys.stderr)
        raise SystemExit(1)
