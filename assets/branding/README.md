# Provisional StormTTY application icon

The project owner supplied `image(20261028-041028).png` on 2026-10-28 for
StormTTY's application/taskbar icon. `stormtty-original.png` is a byte-for-byte
copy of that 1254 × 1254 RGBA PNG, not a redraw or a cleaned-up derivative.
The supplied transparency, entire canvas, aspect ratio, colours, and artwork
are retained; the packaged variants only apply proportional Lanczos resizing
and lossless format encoding. In particular, the supplied edge details are
not cropped, erased, sharpened, or composited onto a new background.

This packaging does not grant or change any artwork, application, or font
license. Artwork rights remain with the original rights holder.

## Checked-in build inputs

| File | Contents |
| --- | --- |
| `stormtty-original.png` | Unmodified supplied source; SHA256 `982cb7679f2d63155d541ccc3984befa12c44eb19576144045c660e96468678a` |
| `stormtty.ico` | Nine 32-bit RGBA PNG frames: 16, 20, 24, 32, 40, 48, 64, 128 and 256 px |
| `stormtty.icns` | Ten point/scale variants: 16, 32, 128, 256 and 512 pt, each at 1× and 2× (up to 1024 px) |
| `SHA256SUMS` | Source and packaged artifact SHA256 checksums |

The ICNS stores 16/32 pt at 1× as legacy RGB plus separate 8-bit alpha masks
(`is32`/`s8mk`, `il32`/`l8mk`). The remaining eight variants use the standard
PNG chunk types, with explicit point-size/Retina identities and a table of
contents. This avoids relying on PNG-encoded legacy small-icon chunks.

## Maintainer regeneration and verification

Normal CMake builds use the checked-in files and require **no Python or Pillow**.
The packaging script was run with Python 3.12, Pillow 12.3.0 and zlib 1.3.2.
For byte-identical regeneration use the same Pillow/zlib versions:

```sh
python3 tools/package-icons.py
python3 tools/package-icons.py --check
cd assets/branding && sha256sum -c SHA256SUMS
```

`--check` does not modify files. It checks source provenance, ICO directory
entries/bounds, ICNS chunk lengths/table of contents, every required size and
scale, alpha-channel extrema, and decoded RGBA pixel equality against the
expected source resizes. It then checks byte-for-byte regeneration and hashes.
An intentional future logo replacement must also update the source hash and
provenance in the script and this file.

Optional Apple-named iconset export for native inspection (not a build step):

```sh
python3 tools/package-icons.py --iconset /path/to/stormtty.iconset
# On macOS, optionally compare an Apple-tool-built container:
iconutil --convert icns --output /path/to/stormtty-apple.icns /path/to/stormtty.iconset
```

## Native integration

- Windows: configure `src/app/windows_resources.rc.in` with CMake `@ONLY` and
  compile the generated `.rc` into each executable that needs the icon. Its
  single `RT_GROUP_ICON` resource is `IDI_STORMTTY` (101), shared through
  `src/app/windows_resources.h`. Load that module resource for the main
  window's large/small icons and StormTTY-owned alert windows; embedding an
  Explorer icon alone does not set every window icon. Do not substitute a
  stock Windows icon.
- macOS: include `stormtty.icns` as a source with
  `MACOSX_PACKAGE_LOCATION Resources`, and set the application target's
  `MACOSX_BUNDLE_ICON_FILE` to `stormtty.icns`. This produces the bundle resource
  and `CFBundleIconFile` entry used by Finder/Dock and the application icon.
- The Unix-host Zig 0.17 Windows GNU gate can use a workspace-cache wrapper
  around `zig rc`. Its RC compile rule must explicitly request COFF output:
  `<CMAKE_RC_COMPILER> <DEFINES> <INCLUDES> <FLAGS> /:output-format coff
  /:target x86_64 /fo <OBJECT> -- <SOURCE>`. Zig accepts the MSVC RC flags, not
  windres command syntax. Native MSVC uses its regular `rc.exe` compiler.

## Evidence and remaining native checks

On 2026-10-28, all nine ICO frames and all ten ICNS variants passed structural
and exact decoded RGBA verification. A miniature CMake/Zig 0.17 Windows GNU
build compiled the resource to COFF, linked an x86-64 PE executable, and its
resource directory contained nine `RT_ICON` entries plus `RT_GROUP_ICON` 101.

That is packaging and cross-link evidence, **not native runtime/UI evidence**.
Windows still needs native MSVC compilation and visual checks of Explorer,
the taskbar, Alt+Tab, main-window icons, and the app-owned startup/error UI at
multiple display scales. macOS still needs native bundle/Finder/Dock checks
and validation of the exported iconset with Apple's `iconutil`.

Format references: [Apple iconset names and sizes](https://developer.apple.com/library/archive/documentation/Xcode/Reference/xcode_ref-Asset_Catalog_Format/IconSetType.html),
[Microsoft ICON resources](https://learn.microsoft.com/en-us/windows/win32/menurc/icon-resource),
[Pillow ICNS decoder/container definitions](https://pillow.readthedocs.io/en/stable/_modules/PIL/IcnsImagePlugin.html).
