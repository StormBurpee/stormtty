# Deterministic metadata control proposal and review scope

This records the separate review proposal, not authorization to build or execute.
Root subsequently authorized source implementation of items 1, 2 and 4 only; see
`build-review/README.md` for the resulting configuration. Item 3 remains proposed
and unimplemented. The package recipe and runtime/source/compiler pins remain
unchanged. Exact matching
DLL, reader and controller hashes across two builds has not been demonstrated.
No normalization of binary hashes, binary export, default change, or fixture launch
is proposed as a substitute for that evidence.

## Why a later exact rebuild may differ

The pinned runtime's `winsup/cygwin/scripts/mkvers.sh` uses the wall-clock date when
`SOURCE_DATE_EPOCH` is unset. `winsup/cygwin/Makefile.am` enables deterministic
archives and zero PE timestamps conditionally on that variable. The pinned
PKGBUILD retains `-ggdb`, then adds a debuglink whose CRC can reflect debug-file
path changes. The reader link has no explicit timestamp control. The controller
link has no reproducibility control. The hosted Windows image and its MSVC tools
can also change; a version label alone does not establish identical tool bytes.

## Candidate changes for a separately reviewed diff

1. Choose one explicit epoch from the retained source commit's timestamp, verify
   that timestamp from the retained Git object, and record the numeric value in
   the reviewed build configuration. Export it identically for A and B. Record
   the value inside the makepkg build environment as well as in the wrapper.
   Do not infer or replace it at execution time.
2. Use fixed private source/build/toolchain path names, identical across repeated
   builds of each variant. Keep A and B's canonical layout symmetric. Capture all
   compiler/linker executable hashes and relevant configuration before/after.
   A changed tool hash requires fresh review, not an accepted equivalent version.
3. If canonical paths alone do not stabilize debug information, propose narrowly
   scoped debug-prefix mappings in a separate explicit diff. Review both emitted
   debug paths and runtime strings before accepting them; do not silently alter
   macro-expanded `__FILE__` or behavior-bearing source. Preserve debug evidence.
4. Make the reader's supported PE timestamp control explicit after confirming it
   against the pinned linker. Separately assess MSVC's reproducible-link option
   for the controller with the actual toolchain. Keep optimization, warnings,
   observer definitions, behavior patches, imports and handler source unchanged.
   No post-link timestamp stripping or binary rewriting is an acceptable shortcut.
5. Retain existing exact source/toolchain/import gates and independent linked
   review. Record the proposed controls as part of source identity so an older
   approval cannot authorize binaries produced with the new configuration.

These are candidate controls, not a promise of reproducibility. Source implementation
of items 1, 2 and 4 changes wrapper metadata controls, not the pinned package recipe. Exact compiler pinning
for the controller may require an additional toolchain decision by root.

## Evidence required before considering a rebuild-based execution gate

Run two separately authorized clean build-only jobs with the identical reviewed
configuration. Both compile A, B, reader and controller; neither launches them.
Export only bounded source/text/disassembly/import evidence and full SHA256 hashes.
Require exact equality for each corresponding DLL, reader and controller (A is
not expected to equal B), plus source/toolchain/configuration identity. Repeat
builds must not reuse the first build's objects or binaries. Capture debug metadata
and generated version sources needed to explain mismatches without exporting
private binaries or archives.

Any mismatch leaves execution blocked. Diagnose the differing metadata/code from
cold evidence and return a new explicit proposal; do not normalize, weaken, or
substitute reviewed hashes. Only after exact matching and fresh independent linked
review may root consider a separately authorized execution job that rebuilds and
checks those same full identities before any launch. All 17 pending matrix cases,
full native adjudication and uninstrumented application qualification remain
separate requirements even if the 15-case stage eventually succeeds.
