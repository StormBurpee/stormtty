#!/usr/bin/env python3
"""Package the supplied artwork; no drawing, cropping, or background changes.

Maintainer-only dependency: Pillow (recorded generation version in README).
Normal native builds consume the checked-in ICO/ICNS and do not run this script.
"""

from __future__ import annotations

import argparse
import hashlib
import io
from pathlib import Path
import struct

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
BRANDING = ROOT / "assets" / "branding"
SOURCE = BRANDING / "stormtty-original.png"
SOURCE_SHA256 = "982cb7679f2d63155d541ccc3984befa12c44eb19576144045c660e96468678a"
WINDOWS_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
MAC_SIZES = (16, 32, 128, 256, 512)
# 16/32 @1x use the long-supported RGB+alpha-mask chunks (below). The PNG
# chunks identify point size AND scale, not merely the backing pixel count.
MAC_PNG_TYPES = (
    (b"ic11", 16, 2), (b"ic12", 32, 2),
    (b"ic07", 128, 1), (b"ic13", 128, 2),
    (b"ic08", 256, 1), (b"ic14", 256, 2),
    (b"ic09", 512, 1), (b"ic10", 512, 2),
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def png_bytes(image: Image.Image) -> bytes:
    stream = io.BytesIO()
    image.save(stream, format="PNG", optimize=False, compress_level=9)
    return stream.getvalue()


def icns_chunk(kind: bytes, payload: bytes) -> bytes:
    return kind + struct.pack(">I", len(payload) + 8) + payload


def legacy_rgb(image: Image.Image) -> bytes:
    # ICNS channel-wise RLE supports literal packets of 1..128 bytes. Literal
    # packets are deterministic, small at 16/32px, and preserve RGB exactly.
    result = bytearray()
    for channel in image.convert("RGB").split():
        pixels = channel.tobytes()
        for offset in range(0, len(pixels), 128):
            packet = pixels[offset:offset + 128]
            result.append(len(packet) - 1)
            result.extend(packet)
    return bytes(result)


def make_icns(frames: dict[int, Image.Image]) -> bytes:
    entries = []
    for size, rgb_type, alpha_type in ((16, b"is32", b"s8mk"), (32, b"il32", b"l8mk")):
        entries.append(icns_chunk(rgb_type, legacy_rgb(frames[size])))
        entries.append(icns_chunk(alpha_type, frames[size].getchannel("A").tobytes()))
    for kind, points, scale in MAC_PNG_TYPES:
        entries.append(icns_chunk(kind, png_bytes(frames[points * scale])))
    toc = icns_chunk(b"TOC ", b"".join(entry[:8] for entry in entries))
    return icns_chunk(b"icns", toc + b"".join(entries))


def verify_rgba(actual: Image.Image, expected: Image.Image, label: str) -> None:
    actual.load()
    require(actual.size == expected.size, f"{label}: wrong dimensions")
    require(actual.mode == "RGBA", f"{label}: alpha channel missing")
    require(actual.tobytes() == expected.tobytes(), f"{label}: pixels or alpha changed")
    alpha_min, alpha_max = actual.getchannel("A").getextrema()
    require(alpha_min == 0 and alpha_max == 255, f"{label}: transparency range lost")


def verify_ico(data: bytes, frames: dict[int, Image.Image]) -> None:
    require(struct.unpack_from("<HHH", data) == (0, 1, len(WINDOWS_SIZES)), "ICO: invalid header")
    next_offset = 6 + 16 * len(WINDOWS_SIZES)
    for index, size in enumerate(WINDOWS_SIZES):
        w, h, colors, reserved, planes, bits, length, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * index)
        require((w or 256, h or 256) == (size, size), "ICO: wrong directory dimensions")
        require((colors, reserved, planes, bits) == (0, 0, 0, 32), "ICO: expected 32-bit RGBA PNG")
        require(offset == next_offset and length > 0 and offset + length <= len(data), "ICO: invalid frame bounds")
        with Image.open(io.BytesIO(data[offset:offset + length])) as frame:
            verify_rgba(frame, frames[size], f"ICO {size}px")
        next_offset += length
    require(next_offset == len(data), "ICO: trailing or missing data")
    with Image.open(io.BytesIO(data)) as icon:
        require(icon.ico.sizes() == {(n, n) for n in WINDOWS_SIZES}, "ICO: unreadable frame directory")
        for size in WINDOWS_SIZES:
            verify_rgba(icon.ico.getimage((size, size)), frames[size], f"ICO decoded {size}px")


def verify_icns(data: bytes, frames: dict[int, Image.Image]) -> None:
    require(data[:4] == b"icns" and struct.unpack_from(">I", data, 4)[0] == len(data), "ICNS: invalid header")
    offset, chunks = 8, []
    while offset < len(data):
        require(offset + 8 <= len(data), "ICNS: truncated chunk header")
        kind, length = struct.unpack_from(">4sI", data, offset)
        require(length >= 8 and offset + length <= len(data), "ICNS: invalid chunk bounds")
        chunks.append((kind, length, data[offset + 8:offset + length]))
        offset += length
    expected_types = [b"TOC ", b"is32", b"s8mk", b"il32", b"l8mk"] + [kind for kind, _, _ in MAC_PNG_TYPES]
    require([kind for kind, _, _ in chunks] == expected_types, "ICNS: missing or duplicate variant")
    require(chunks[0][2] == b"".join(kind + struct.pack(">I", length) for kind, length, _ in chunks[1:]), "ICNS: invalid table of contents")
    with Image.open(io.BytesIO(data)) as icon:
        expected = {(size, size, scale) for size in MAC_SIZES for scale in (1, 2)}
        require(set(icon.info["sizes"]) == expected, "ICNS: unreadable point/scale variants")
        for size, _, scale in sorted(expected):
            frame = icon.icns.getimage((size, size, scale))
            verify_rgba(frame, frames[size * scale], f"ICNS {size}pt @{scale}x")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Verify checked-in resources without writing them")
    parser.add_argument("--iconset", type=Path, help="Also export the ten Apple-named PNGs into this .iconset directory")
    args = parser.parse_args()
    require(not (args.check and args.iconset), "--check cannot be combined with --iconset")
    require(hashlib.sha256(SOURCE.read_bytes()).hexdigest() == SOURCE_SHA256, "Original artwork differs from the supplied source; review provenance before replacing it")
    with Image.open(SOURCE) as source:
        require(source.size == (1254, 1254) and source.mode == "RGBA", "Unexpected original artwork format")
        # Preserve the entire square canvas, aspect, colour and alpha. No crop,
        # trim, sharpening, cleanup, redrawing, compositing, or added background.
        sizes = set(WINDOWS_SIZES) | {n * scale for n in MAC_SIZES for scale in (1, 2)}
        frames = {size: source.resize((size, size), Image.Resampling.LANCZOS) for size in sorted(sizes)}
    stream = io.BytesIO()
    frames[256].save(stream, format="ICO", sizes=[(n, n) for n in WINDOWS_SIZES],
                     append_images=[frames[n] for n in WINDOWS_SIZES if n != 256])
    outputs = {"stormtty.ico": stream.getvalue(), "stormtty.icns": make_icns(frames)}
    verify_ico(outputs["stormtty.ico"], frames)
    verify_icns(outputs["stormtty.icns"], frames)
    checksums = f"{SOURCE_SHA256}  stormtty-original.png\n"
    for name, data in outputs.items():
        destination = BRANDING / name
        if args.check:
            require(destination.read_bytes() == data, f"{name} differs from deterministic regeneration")
        else:
            destination.write_bytes(data)
        checksums += f"{hashlib.sha256(data).hexdigest()}  {name}\n"
    checksum_path = BRANDING / "SHA256SUMS"
    if args.check:
        require(checksum_path.read_text() == checksums, "SHA256SUMS differs from generated artifacts")
    else:
        checksum_path.write_text(checksums)
    if args.iconset:
        args.iconset.mkdir(parents=True, exist_ok=True)
        for size in MAC_SIZES:
            for scale in (1, 2):
                suffix = "@2x" if scale == 2 else ""
                (args.iconset / f"icon_{size}x{size}{suffix}.png").write_bytes(png_bytes(frames[size * scale]))
    print("Verified original SHA256; all 9 ICO frames and all 10 ICNS point/scale variants retain expected RGBA pixels.")
    print(checksums, end="")


if __name__ == "__main__":
    main()
