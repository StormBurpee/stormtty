# Locked bundled-shell distribution packet

Reviewed against the exact `zsh-msys2-x86_64-20261028-r1` package set on 2026-10-28. This packet preserves upstream terms; it is not a legal opinion or a change to StormTTY's first-party licensing.

## Completed, concrete work

- `components.json` maps **all 17 binary packages**, exact versions and binary SHA-256 values to the applicable notice directories and source archives. Coarse package tags are not substituted for actual library terms.
- `assets.json` identifies **79 exact-text notice assets** by hash and provenance: upstream copyright/license/exception files, original authors lists, the GPL-licensed `_darcs` completion, runtime/Newlib notices, and documentation copying statements. Cygwin's supplemental `SOURCE-NOTICES.txt` retains 484 distinct complete copyright/license comment blocks from the pinned source tree, with their source paths. The untouched full sources retain every source notice, including material beyond this supplemental index.
- The complete original runtime remains intact. Original `usr/share/licenses`, documentation, command copyright output and source headers are not removed or replaced by this packet.
- The source emitter includes **19 exact source archives**, not just the 17 package-source archives initially identified. Every archive is checked against its fixed byte count and SHA-256. Each original source archive retains its `PKGBUILD`, distribution patches and upstream source material; the runtime's archive contains the actual source repository, not merely a fetch recipe.
- Source delivery also includes the original binary package `.BUILDINFO`, `.PKGINFO`, `.MTREE` and `.INSTALL` as inert build evidence, plus the actual StormTTY staging/preset/notice scripts. Nothing is executed while assembling the packet.

## Static dependencies that were explicitly closed

| Binary | Evidence | Additional corresponding source |
|---|---|---|
| `dumper.exe` in msys2-runtime | `winsup/utils/Makefile.am` links `-lbfd`; configure adds `-liberty`; original runtime `.BUILDINFO` records binutils 2.47-1 | `binutils-2.47-1.src.tar.zst`, SHA-256 `2cf54a4208a6683ca2793fb14eb6ca4b3ddec9474e60738c90050bb2ee359276` |
| `bash.exe` / `sh.exe` | Original `PKGBUILD` uses `--enable-static-link --with-installed-readline`; `.BUILDINFO` records libreadline-devel 8.3.006-1 | `readline-8.3.006-1.src.tar.zst`, SHA-256 `9ddb3eb989f6ad7d696ce5a2d92322b33a20056b293e3c4fc4f6cc568a320640` |

The matching ncurses, libiconv and gettext sources used in the Bash build are already in the original set. General-purpose compiler/toolchain source is not automatically part of application Corresponding Source; GCC runtime license/exception notices are preserved. This inventory does not claim an independently reproduced, bit-identical build.

## Important component distinctions retained

- **Zsh:** its own permissive license, not MIT by assertion. The shipped `_darcs` completion separately grants GPL-2.0-or-later; its exact source and GPL text are included. A GPL-name completion list in `_composer` is not treated as a license grant.
- **Cygwin/MSYS2:** LGPLv3+ DLL with the Cygwin linking exception; GPLv3+ utilities; Newlib and BSD-family source notices preserved. The pinned runtime source tag resolves to `867eb7748e6e5a0e8430ce9f46af27b4b1e8f204`.
- **GCC libraries:** GPLv3+ with Runtime Library Exception 3.1 for the applicable runtimes; LGPL2.1+ libquadmath notices retained separately. The exception is not described as making the runtime libraries public domain.
- **libintl:** the actual `gettext-runtime/intl/COPYING.LIB` is LGPL2.1, despite the binary package's coarse `GPL` tag. Both its actual library notices and the upstream package COPYING are preserved.
- **PCRE2/PCRE:** preserve full PCRE terms and the separately licensed JIT material; PCRE2's exception is not omitted.
- **Less/Zstandard/GMP:** retain the supplied license alternatives rather than deleting one based on package metadata. Documentation terms are independent of code terms.
- **GMP manual:** GFDL1.3+ with specified front/back-cover texts, retained verbatim in the original Info files and this notice packet. Other included Info manuals' copying statements and GFDL license nodes are retained. The manuals are not edited or rebranded.

## Exact upstream inconsistency (not an invented blanket blocker)

At the pinned Cygwin source revision, `winsup/utils/dumper.cc` explicitly grants **GPL version 3 or later**, but its prose references `COPYING.dumper`, which contains **GPL version 2**. The packet preserves both original texts and also includes GPLv3. The component mapping follows the operative source-file grant, GPLv3-or-later, while documenting the stale/conflicting license-file reference. No permission to disregard either notice is fabricated. This is the specific upstream ambiguity to retain in any legal review; it is not evidence that StormTTY's unrelated native application must be relicensed.

## Generate the concrete deliverables

```powershell
python tools/shell-notices-tests.py
python tools/shell-notices.py --runtime .deps/stormtty-shell `
  --cache .cache/shell-bootstrap --output .artifacts/shell-distribution `
  --source-packet
```

Use `--offline` with the verified source cache. The **fresh output directory** contains:

- `runtime-notices/`: reviewable notice overlay, all exact texts, component/provenance mapping and source manifest. Merge this under the runtime's notices directory without removing upstream notices. It supersedes the staging-only placeholder summary, not the original upstream files.
- `corresponding-source.zip`: deterministic, stored ZIP containing all 19 source archives, build evidence, notices and packaging/preset source.
- `SOURCE-README.txt`: reconstruction instructions and distribution scope.
- `packet-manifest.json`: source ZIP hash/size and local-generation facts.

The runtime itself and its qualification/default flags are **not mutated**. `--source-packet` is explicit because the source payload is approximately 323 MiB. No source archive is fetched from a moving `latest` URL.

## Remaining publication duties — bounded and actionable

1. The publisher must include the notice overlay with the exact binary package and actually publish the matching source ZIP alongside that binary download, without additional charge, with a clear direct source link. The intended GPLv3 route is section 6(d), not a vague written offer or links to moving upstream repositories. Keep matching source available for every distributed binary version.
2. Preserve applicable license rights in any product EULA/distribution terms, including users' rights to modify/replace the separately distributed LGPL libraries. Do not impose a signature/hash restriction that prevents lawful replacement while claiming those rights remain available. No EULA or locked-down hardware installation policy is authored here.
3. Native Windows/default activation remains a separate evidence gate owned by the parent task. A generated notice/source packet does not assert native correctness or publication.

For this exact locked set, the identified missing license texts and the two concrete static-link source gaps have been supplied. There is no open-ended “find some licenses later” task left in this packet. Future package changes require regenerating/reviewing the mapping and checking static dependencies again.
