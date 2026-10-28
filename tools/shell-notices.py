#!/usr/bin/env python3
"""Emit reviewed notices and exact corresponding-source packet beside a staged runtime.
Does not mutate the runtime, publish anything, or change default/native gates.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import zipfile

TOOLS = Path(__file__).resolve().parent
ASSETS = TOOLS / "shell-notices"
spec = importlib.util.spec_from_file_location("stormtty_shell_bootstrap", TOOLS / "shell-bootstrap.py")
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def checked_asset(path, expected, expected_bytes=None):
    if path.is_symlink() or not path.is_file():
        raise RuntimeError(f"Notice asset integrity failure: {path}")
    size = path.stat().st_size
    actual = bootstrap.digest(path)
    if (expected_bytes is not None and size != expected_bytes) or actual != expected:
        raise RuntimeError(f"Notice asset integrity failure: {path}; expected SHA-256 {expected}, "
                           f"bytes {expected_bytes}; actual SHA-256 {actual}, bytes {size}")
    return path


def notice_bytes(path, expected, expected_bytes):
    """Recover only exact hash-pinned notice text; never modify the checkout."""
    if path.is_symlink() or not path.is_file():
        raise RuntimeError(f"Notice asset must be a regular file: {path}")
    size = path.stat().st_size
    if size <= 2 * expected_bytes + 3:
        raw = path.read_bytes()
        without_bom = raw.removeprefix(b"\xef\xbb\xbf")
        for candidate in (raw, raw.replace(b"\r\n", b"\n"), without_bom,
                          without_bom.replace(b"\r\n", b"\n")):
            if len(candidate) == expected_bytes and hashlib.sha256(candidate).hexdigest() == expected:
                return candidate
        actual = hashlib.sha256(raw).hexdigest()
        size = len(raw)
    else:
        actual = bootstrap.digest(path)
    raise RuntimeError(f"Notice asset integrity failure: {path}; expected SHA-256 {expected}, "
                       f"bytes {expected_bytes}; actual SHA-256 {actual}, bytes {size}")


def source_records(lock):
    records = lock["sources"] + read_json(ASSETS / "source-supplements.lock.json")["sources"]
    names = set()
    for item in records:
        name = item["filename"]
        if len(bootstrap.portable_path(name).parts) != 1 or name in names:
            raise RuntimeError("Unsafe/duplicate source archive filename")
        if item["url"] != "https://repo.msys2.org/msys/sources/" + name:
            raise RuntimeError("Unexpected source archive origin")
        names.add(name)
    return records


def validated_assets(lock):
    index = read_json(ASSETS / "assets.json")
    mapping = read_json(ASSETS / "components.json")
    if index["bundle_id"] != lock["bundle_id"] or mapping["bundle_id"] != lock["bundle_id"]:
        raise RuntimeError("Notice inventory does not match the locked bundle")
    if {p["name"] for p in lock["packages"]} != {p["package"] for p in mapping["components"]}:
        raise RuntimeError("Component mapping is incomplete")
    known = {a["component"] for a in index["assets"]}
    for component in mapping["components"]:
        package = next(p for p in lock["packages"] if p["name"] == component["package"])
        if package["sha256"] != component["binary_sha256"]:
            raise RuntimeError("Component mapping was reviewed for a different binary")
        if any(c not in known for c in component["notice_components"]):
            raise RuntimeError("Component lacks mapped notice texts")
    for asset in index["assets"]:
        bootstrap.portable_path(asset["path"])
        notice_bytes(ASSETS / asset["path"], asset["sha256"], asset["bytes"])
    return index, mapping


def archive_file(archive, name, path, expected=None):
    bootstrap.portable_path(name)
    info = zipfile.ZipInfo(name, date_time=(2026, 10, 28, 0, 0, 0))
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = 0o644 << 16
    h = hashlib.sha256()
    with path.open("rb") as source, archive.open(info, "w", force_zip64=True) as target:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            h.update(block)
            target.write(block)
    if expected and h.hexdigest() != expected:
        raise RuntimeError(f"Input changed while writing source packet: {path}")


def emit(args):
    lock = bootstrap.read_lock(TOOLS / "shell-bootstrap.lock.json")
    index, mapping = validated_assets(lock)
    sources = source_records(lock)
    output = args.output.absolute()
    if output.exists() or output.is_symlink():
        raise RuntimeError("Output must be a fresh directory")
    runtime = args.runtime.resolve(strict=True)
    runtime_manifest = read_json(runtime / "stormtty-shell-manifest.json")
    if runtime_manifest["lock_sha256"] != bootstrap.digest(TOOLS / "shell-bootstrap.lock.json"):
        raise RuntimeError("Runtime was staged from a different lock")
    # Preserve build evidence exactly; do not execute .INSTALL/PKGBUILD files.
    evidence = []
    for record in runtime_manifest["files"]:
        name = record["path"]
        bootstrap.portable_path(name)
        if name.startswith(".stormtty-packages/"):
            path = checked_asset(runtime / name, record["sha256"], record["bytes"])
            evidence.append((name, path, record["sha256"]))
    if not all(any(n.startswith(f".stormtty-packages/{p['name']}/") and n.endswith('/.BUILDINFO')
                   for n, _, _ in evidence) for p in lock["packages"]):
        raise RuntimeError("Runtime lacks original per-package build evidence")
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    source_paths = []
    if args.source_packet:
        for source in sources:
            source_paths.append((source, bootstrap.download(source, cache, args.offline)))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".stormtty-notices-", dir=output.parent) as temp:
        staged = Path(temp) / "packet"
        notices = staged / "runtime-notices"
        notices.mkdir(parents=True)
        for asset in index["assets"]:
            path = notices / asset["path"]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(notice_bytes(ASSETS / asset["path"], asset["sha256"], asset["bytes"]))
        for filename in ("assets.json", "components.json", "source-supplements.lock.json", "REVIEW.md"):
            shutil.copyfile(ASSETS / filename, notices / filename)
        (notices / "source-manifest.json").write_text(json.dumps(sources, indent=2) + "\n", encoding="utf-8")
        text = ["StormTTY bundled Zsh — third-party notices", "",
                "This packet supplements, and must not replace/remove, upstream notices already",
                "present in the intact runtime. StormTTY's first-party licensing is unchanged.",
                "No Ghostty GPL shell-integration script is incorporated.", "",
                "The texts/ directory contains exact upstream copyright, license and exception texts.",
                "assets.json records source provenance and text hashes; components.json maps every",
                "locked binary component to those texts. Documentation copying terms are included.", "",
                "Matching source archive: corresponding-source.zip (when supplied with this packet).",
                "Publish this source archive beside the exact binary release, without extra charge,",
                "and give recipients a clear direct source-download link. Local generation does not",
                "claim that public access has been established. See REVIEW.md for scope and caveats.", ""]
        for c in mapping["components"]:
            text += [f"{c['package']} {c['version']}: {c['license_review']}",
                     "  Notice directories: " + ", ".join("texts/" + n for n in c["notice_components"]),
                     "  " + c["review_notes"], ""]
        (notices / "THIRD-PARTY-NOTICES.txt").write_text("\n".join(text), encoding="utf-8")
        summary = {"schema": 1, "bundle_id": lock["bundle_id"], "binary_components": len(mapping["components"]),
                   "notice_assets": len(index["assets"]), "source_archives": len(sources),
                   "source_packet_emitted": args.source_packet, "runtime_mutated": False,
                   "public_source_availability_verified": False, "native_qualification_changed": False}
        if args.source_packet:
            readme = staged / "SOURCE-README.txt"
            readme.write_text(
                "Corresponding source for the exact StormTTY Zsh runtime identified by shell-bootstrap.lock.json.\n\n"
                "sources/ preserves 19 unmodified MSYS2 source archives, not links to moving branches.\n"
                "Each includes upstream source material, the exact packaging recipe and distribution patches.\n"
                "The msys2-runtime archive contains the repository needed for tag cygwin-3.6.11\n"
                "(commit 867eb7748e6e5a0e8430ce9f46af27b4b1e8f204), not an empty fetch-only recipe.\n"
                "The two extra archives are binutils 2.47-1 for dumper's libbfd/libiberty and\n"
                "readline 8.3.006-1 for Bash's externally installed static Readline.\n\n"
                "build-evidence/ contains original .BUILDINFO/.PKGINFO/.MTREE/.INSTALL data.\n"
                "Use a separate matching MSYS2 build environment and each supplied PKGBUILD to rebuild;\n"
                "the BUILDINFO files identify the original build environment and package versions.\n"
                "Do not execute install/build recipes on the user's machine. No bit-reproducible rebuild\n"
                "claim is made. General-purpose compiler/toolchain code is not reclassified as application\n"
                "source; the GCC Runtime Library Exception and original notices are preserved.\n\n"
                "stormtty-packaging/ preserves the repository layout of the actual staging/preset/notice\n"
                "scripts, pinned build-tool bootstrap, app assembly helper and controlling CMake files.\n"
                "Their inclusion does not change first-party application licensing. Original package\n"
                "license/copyright/exception terms control their respective files, not the aggregate.\n\n"
                "Source distribution is intended to accompany the exact binary download (GPLv3 6(d));\n"
                "the publisher must actually provide accessible source downloads and retain the exact\n"
                "source package for each distributed binary version. This is not a written offer.\n",
                encoding="utf-8")
            archive_path = staged / "corresponding-source.zip"
            with zipfile.ZipFile(archive_path, "x", allowZip64=True) as archive:
                archive_file(archive, "README.txt", readme)
                for source, path in sorted(source_paths, key=lambda x: x[0]["filename"]):
                    archive_file(archive, "sources/" + source["filename"], path, source["sha256"])
                for name, path, checksum in sorted(evidence):
                    archive_file(archive, "build-evidence/" + name, path, checksum)
                recipes = ["tools/" + name for name in (
                    "shell-bootstrap.py", "shell-bootstrap.lock.json", "shell-bootstrap-tests.py",
                    "shell-bootstrap.README.md", "shell-preset.zsh", "shell-runtime.fstab", "shell-runtime.nsswitch.conf", "shell-path-tests.py", "shell-notices.py", "shell-notices-tests.py",
                    "shell-zstd-bootstrap.py", "shell-zstd-bootstrap.lock.json", "shell-zstd-bootstrap-tests.py",
                    "prepare-bundled-shell.py", "stage-developer-shell.py", "stage-developer-shell-tests.py")]
                recipes += ["CMakeLists.txt", "cmake/StormTTYDeveloperShell.cmake"]
                for name in sorted(recipes):
                    archive_file(archive, "stormtty-packaging/" + name, TOOLS.parent / name)
                asset_pins = {a["path"]: a["sha256"] for a in index["assets"]}
                for path in sorted(ASSETS.rglob("*")):
                    if path.is_file():
                        name = path.relative_to(ASSETS).as_posix()
                        # Recipe copies must use the same verified exact notice
                        # bytes as the runtime overlay, not altered checkout text.
                        source = notices / name if name in asset_pins else path
                        archive_file(archive, "stormtty-packaging/tools/shell-notices/" + name,
                                     source, asset_pins.get(name))
                for path in sorted(notices.rglob("*")):
                    if path.is_file():
                        archive_file(archive, "notices/" + path.relative_to(notices).as_posix(), path)
            summary["source_packet_sha256"] = bootstrap.digest(archive_path)
            summary["source_packet_bytes"] = archive_path.stat().st_size
        (staged / "packet-manifest.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        if output.exists():
            raise RuntimeError("Output appeared while assembling; refusing overwrite")
        staged.rename(output)
    print(json.dumps(summary, indent=2))
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-packet", action="store_true")
    parser.add_argument("--offline", action="store_true")
    try:
        emit(parser.parse_args())
        return 0
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print(f"shell-notices: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
