#!/usr/bin/env python3
"""Build-only, pinned ConPTY acquisition. No execution/downloads by the app.

Windows trust verification is mandatory for staging/install verification. Portable
unit tests exercise only archive handling; there is deliberately no trust bypass.
"""
from __future__ import annotations
import argparse
import base64
import ctypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

TOOLS = Path(__file__).resolve().parent
LOCK = TOOLS / "conpty-bootstrap.lock.json"
OWNER = "stormtty-private-conpty-v1"
MAX_ENTRY = 2 * 1024 * 1024


def checked_path(path: Path) -> Path:
    """Reject symlink/reparse ancestry before creating or consuming local files."""
    if not path.is_absolute():
        raise RuntimeError("ConPTY paths must be absolute")
    if ".." in path.parts:
        raise RuntimeError("ConPTY paths must not contain parent traversal")
    if os.name == "nt" and (str(path).startswith("\\\\") or not path.drive):
        raise RuntimeError("ConPTY requires a local drive path, not UNC/device paths")
    for part in [path, *path.parents]:
        try:
            info = part.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise RuntimeError(f"Linked/reparse path rejected: {part}")
    return path


def checked_bytes(path: Path, expected: dict) -> bytes:
    checked_path(path)
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_size != expected["size"]:
        raise RuntimeError(f"Missing/nonregular/wrong-sized pinned file: {path}")
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected["sha256"]:
        raise RuntimeError(f"SHA256 mismatch: {path}")
    return data


def require_native_amd64() -> Path:
    if os.name != "nt":
        raise RuntimeError("ConPTY staging requires native AMD64 Windows trust verification; cross-build with STORMTTY_BUNDLE_CONPTY=OFF")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    kernel.IsWow64Process2.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ushort), ctypes.POINTER(ctypes.c_ushort)]
    process, native = ctypes.c_ushort(), ctypes.c_ushort()
    if not kernel.IsWow64Process2(kernel.GetCurrentProcess(), ctypes.byref(process), ctypes.byref(native)) or native.value != 0x8664:
        raise RuntimeError("Bundled ConPTY preview supports native AMD64 Windows only; ARM64/emulation is not qualified")
    buffer = ctypes.create_unicode_buffer(32768)
    count = kernel.GetWindowsDirectoryW(buffer, len(buffer))
    if not count or count >= len(buffer):
        raise RuntimeError("Could not resolve Windows directory for built-in trust verification")
    powershell = Path(buffer.value) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    checked_path(powershell)
    if not powershell.is_file():
        raise RuntimeError("Built-in Windows PowerShell unavailable; cannot verify Microsoft Authenticode")
    return powershell


def verify_authenticode(root: Path, powershell: Path) -> None:
    quoted = ",".join("'" + str(root / n).replace("'", "''") + "'" for n in ("conpty.dll", "OpenConsole.exe"))
    # GitHub's pwsh host can export PS7 module paths into this PS5.1 child.
    # Never autoload a same-named user/PS7 module into the system trust verifier.
    # This is child-only module discovery, not an execution-policy change.
    environment = {key: value for key, value in os.environ.items() if key.upper() != "PSMODULEPATH"}
    environment["PSModulePath"] = str(powershell.parent / "Modules")
    script = ("$ErrorActionPreference='Stop'; "
              "Import-Module ($PSHOME + '\\Modules\\Microsoft.PowerShell.Security\\Microsoft.PowerShell.Security.psd1') -ErrorAction Stop; "
              "foreach ($p in @(" + quoted + ")) { ") + r"""
$s=Microsoft.PowerShell.Security\Get-AuthenticodeSignature -LiteralPath $p
if ($s.Status -ne 'Valid' -or !$s.SignerCertificate -or $s.SignerCertificate.Subject -notmatch '(?:^|,\s*)O=Microsoft Corporation(?:,|$)') {
  throw "Microsoft Authenticode validation failed: $p ($($s.Status))"
}
Write-Output "$p signer=$($s.SignerCertificate.Subject) thumbprint=$($s.SignerCertificate.Thumbprint)"
}"""
    encoded = base64.b64encode(script.encode("utf-16-le")).decode("ascii")
    subprocess.run([str(powershell), "-NoLogo", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded], check=True, timeout=120, env=environment)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise RuntimeError("Unexpected redirect from pinned NuGet download URL")


def acquire(cache: Path, lock: dict, offline: bool) -> Path:
    checked_path(cache)
    cache.mkdir(parents=True, exist_ok=True)
    package = cache / f"{lock['package']}.{lock['version']}.nupkg"
    if not package.exists():
        if offline:
            raise RuntimeError(f"Pinned ConPTY archive absent from offline cache: {package}")
        fd, temp_name = tempfile.mkstemp(prefix=".conpty-download-", dir=cache)
        temp = Path(temp_name)
        try:
            opener = urllib.request.build_opener(NoRedirect)
            with os.fdopen(fd, "wb") as out, opener.open(lock["url"], timeout=60) as response:
                if response.geturl() != lock["url"]:
                    raise RuntimeError("Unexpected NuGet download origin")
                remaining = lock["size"]
                while True:
                    chunk = response.read(min(65536, remaining + 1))
                    if not chunk:
                        break
                    remaining -= len(chunk)
                    if remaining < 0:
                        raise RuntimeError("Oversized pinned ConPTY download")
                    out.write(chunk)
            checked_bytes(temp, lock)
            checked_path(package)
            os.replace(temp, package)
        finally:
            temp.unlink(missing_ok=True)
    data = checked_bytes(package, lock)
    if base64.b64encode(hashlib.sha512(data).digest()).decode("ascii") != lock["catalog_sha512_base64"]:
        raise RuntimeError("Official catalog SHA512 mismatch")
    return package


def extract(package: Path, destination: Path, lock: dict) -> None:
    """Only pinned named payloads, never extractall or archive-provided paths."""
    with zipfile.ZipFile(package) as archive:
        entries = archive.infolist()
        if len(entries) > 64:
            raise RuntimeError("Too many entries in ConPTY archive")
        names = set()
        for entry in entries:
            name = entry.filename
            path = PurePosixPath(name)
            if name in names or path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name:
                raise RuntimeError(f"Duplicate/unsafe archive entry: {name}")
            names.add(name)
            kind = stat.S_IFMT(entry.external_attr >> 16)
            if kind not in (0, stat.S_IFREG, stat.S_IFDIR) or entry.file_size > MAX_ENTRY:
                raise RuntimeError(f"Nonregular/oversized archive entry: {name}")
        for name, expected in lock["files"].items():
            if name not in names:
                raise RuntimeError(f"Missing pinned archive entry: {name}")
            entry = archive.getinfo(name)
            if entry.file_size != expected["size"]:
                raise RuntimeError(f"Wrong pinned entry size: {name}")
            with archive.open(entry) as source:
                data = source.read(MAX_ENTRY + 1)
            if len(data) != expected["size"] or hashlib.sha256(data).hexdigest() != expected["sha256"]:
                raise RuntimeError(f"Pinned entry hash mismatch: {name}")
            target = destination / expected["destination"]
            checked_path(target)
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open("xb") as out:
                out.write(data)


def manifest_bytes(lock: dict) -> bytes:
    return (json.dumps({"owner": OWNER, "provenance": lock}, indent=2) + "\n").encode("utf-8")


def verify_layout(root: Path, lock: dict, powershell: Path) -> None:
    checked_path(root)
    for expected in lock["files"].values():
        checked_bytes(root / expected["destination"], expected)
    for name, expected in lock["notices"].items():
        checked_bytes(root / "notices" / name, expected)
    manifest = root / "notices/provenance.json"
    expected_manifest = manifest_bytes(lock)
    checked_bytes(manifest, {"size": len(expected_manifest), "sha256": hashlib.sha256(expected_manifest).hexdigest()})
    expected_names = {v["destination"] for v in lock["files"].values()} | {"notices/" + n for n in lock["notices"]} | {"notices/provenance.json"}
    for path in root.rglob("*"):
        checked_path(path)
        if not path.is_dir() and path.relative_to(root).as_posix() not in expected_names:
            raise RuntimeError(f"Unexpected file in private ConPTY runtime: {path}")
    verify_authenticode(root, powershell)


class PublicationRecoveryRequired(RuntimeError):
    """Keep the private staging directory and guard if rollback is obstructed."""


def publish_runtime(stage: Path, destination: Path, receipt: Path, version: str) -> None:
    # Prepare every fallible byte before changing the working runtime. The
    # receipt replacement belongs to this transaction, not to a later step.
    receipt_temp = stage.parent / "receipt"
    receipt_temp.write_text(json.dumps({"owner": OWNER, "version": version}) + "\n", encoding="utf-8")
    old = stage.parent / "previous"
    had_destination = destination.exists()
    if had_destination:
        destination.rename(old)
    published = False
    try:
        stage.rename(destination)
        published = True
        os.replace(receipt_temp, receipt)
    except OSError as error:
        try:
            if published:
                destination.rename(stage)
            if had_destination:
                old.rename(destination)
        except OSError as rollback_error:
            raise PublicationRecoveryRequired(
                f"ConPTY publication failed and rollback was obstructed. Preserve {stage.parent} "
                f"and the active guard for manual recovery; do not delete prior runtime files. "
                f"Publication: {error}; rollback: {rollback_error}") from rollback_error
        raise


def prepare(destination: Path, cache: Path, offline: bool, verify_only: bool = False) -> None:
    powershell = require_native_amd64()  # No non-Windows or unverified staging path.
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    checked_path(destination)
    checked_path(cache)
    if destination == cache or destination in cache.parents or cache in destination.parents:
        raise RuntimeError("Runtime and archive cache must be separate directories")
    for name, expected in lock["notices"].items():
        checked_bytes(TOOLS / "conpty-notices" / name, expected)
    if verify_only:
        verify_layout(destination, lock, powershell)
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    receipt = destination.with_name(destination.name + ".stormtty-conpty-owned.json")
    guard = destination.with_name(destination.name + ".stormtty-conpty-active")
    checked_path(receipt); checked_path(guard)
    try:
        fd = os.open(guard, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError as error:
        raise RuntimeError(f"ConPTY preparation already active: {guard}; after interruption confirm no build runs before removing this guard") from error
    preserve_recovery = False
    try:
        with os.fdopen(fd, "w") as out:
            out.write(f"pid={os.getpid()}\n")
        owned = receipt.is_file() and json.loads(receipt.read_text(encoding="utf-8")).get("owner") == OWNER
        if destination.exists() and not owned:
            raise RuntimeError("Refusing to replace runtime not owned by ConPTY build helper")
        if owned and destination.is_dir():
            try:
                verify_layout(destination, lock, powershell)
                return
            except (OSError, RuntimeError):
                # Missing/hash-damaged outputs can be repaired only from the pin.
                # Authenticode subprocess failures are NOT swallowed/repaired.
                pass
        package = acquire(cache, lock, offline)
        temporary = Path(tempfile.mkdtemp(prefix=".stormtty-conpty-", dir=destination.parent))
        try:
            stage = temporary / "runtime"
            stage.mkdir()
            extract(package, stage, lock)
            for name, expected in lock["notices"].items():
                (stage / "notices" / name).write_bytes(checked_bytes(TOOLS / "conpty-notices" / name, expected))
            (stage / "notices/provenance.json").write_bytes(manifest_bytes(lock))
            verify_layout(stage, lock, powershell)
            publish_runtime(stage, destination, receipt, lock["version"])
        except PublicationRecoveryRequired:
            preserve_recovery = True
            raise
        finally:
            if not preserve_recovery:
                shutil.rmtree(temporary)
    finally:
        if not preserve_recovery:
            guard.unlink(missing_ok=True)



def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--verify-only", action="store_true", help="Verify existing installed/staged layout without downloading or repairing")
    args = parser.parse_args()
    try:
        prepare(args.destination, args.cache, args.offline, args.verify_only)
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        print(f"ConPTY preparation failed: {error}", file=sys.stderr)
        return 1
    print("Verified private ConPTY preview runtime; System remains the default. Compact-alt resize qualification remains unresolved.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
