#!/usr/bin/env bash
# Experimental native MSYS build ONLY. Never installs a package or DLL.
set -euo pipefail
[[ $# == 3 ]] || { echo 'usage: combined-runtime/build-review/build.sh VERIFIED_SOURCE_ARCHIVE NEW_OUTPUT_DIR copied|combined' >&2; exit 2; }
archive=$(realpath "$1")
output=$(realpath -m "$2")
variant=$3
# Reviewed source-tag commit epoch, not the MSYS packaging marker.
export SOURCE_DATE_EPOCH=1790890720
diagnostic=$(cd -- "$(dirname -- "$0")" && pwd)
combined=$(dirname "$diagnostic")
here=$(cd "$combined/.." && pwd)
behavior="$here/partial-read"
[[ $combined == /c/stormtty-build-review/source/tools/probes/msys-runtime-experiment/combined-runtime ]]
[[ $output == /c/stormtty-build-review/"$variant"-build ]]
[[ ${EXPERIMENT_TOOLCHAIN:-} == /c/stormtty-build-review/gcc15 ]]
[[ $variant == copied || $variant == combined ]] || exit 2
[[ ${MSYSTEM:-} == MSYS && $(uname -m) == x86_64 ]] || { echo 'Native MSYS x86_64 build shell required' >&2; exit 2; }
[[ ! -e $output ]] || { echo 'Output must not exist' >&2; exit 2; }
echo '4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204  '"$archive" | sha256sum -c -
echo '2a827b8e656975d8f34511152a664783fe48c1f6199ecd5506825af92bd7b508  '"$behavior/0007-preserve-copied-on-signal.patch" | sha256sum -c -
mkdir -p "$output/packages" "$output/provenance"
python3 "$diagnostic/source-manifest.py" "$output/provenance/source-before.json"
echo 'a5b1d57c84088ac9330df9a30c64993d5aa2ce5b8adf242732c737807818a7f6  '"$here/0000-utils-const-compat.patch" | sha256sum -c -
printf 'SOURCE_DATE_EPOCH=%s\n' "${SOURCE_DATE_EPOCH-<unset>}" > "$output/provenance/build-time-environment.txt"
df -k "$output" | tee "$output/provenance/disk-before.txt"
available=$(df -Pk "$output" | awk 'NR==2 {print $4}')
(( available >= 6 * 1024 * 1024 )) || { echo 'Need at least 6 GiB free before runtime build' >&2; exit 1; }
# Preserve original source archive and experimental patch, including upstream licenses.
cp -- "$archive" "$output/provenance/msys2-runtime-3.6.11-4.src.tar.zst"
cp -- "$behavior/0007-preserve-copied-on-signal.patch" "$output/provenance/"
cp -- "$here/0000-utils-const-compat.patch" "$output/provenance/"
cp -- "$combined/PLAN.md" "$output/provenance/combined-plan.md"
cp -- "$here/0006-refined-prefix.patch" "$output/provenance/"
pacman -Q > "$output/provenance/pacman-before.txt"
find /var/cache/pacman/pkg -maxdepth 1 -type f -name '*.pkg.tar.*' -exec sha256sum {} + > "$output/provenance/cached-toolchain-packages.sha256.txt"
{ uname -a; gcc --version; g++ --version; make --version; makepkg --version; } > "$output/provenance/system-toolchain.txt"
# Match the original GCC15 runtime build's GNU++17 dialect explicitly. GCC16
# defaults to GNU++20; the official recipe overwrites CXXFLAGS, but not CXX.
# This applies identically to copied and combined, without relaxing -Werror.
[[ -n ${EXPERIMENT_TOOLCHAIN:-} ]]
export PATH="$EXPERIMENT_TOOLCHAIN/wrappers:$PATH"
export CC=stormtty-gcc-15
[[ $($CC -dumpfullversion) == 15.3.0 ]]
[[ $(stormtty-gxx-15 -dumpfullversion) == 15.3.0 ]]
tool_snapshot() {
  {
    for tool in gcc g++ ld as ar ranlib objcopy objdump x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ x86_64-w64-mingw32-ld x86_64-w64-mingw32-as x86_64-w64-mingw32-ar x86_64-w64-mingw32-ranlib; do
      realpath "$(command -v "$tool")"
    done
    for driver in "$CC" x86_64-w64-mingw32-gcc; do
    for tool in cc1 cc1plus collect2 ld as; do
      located=$("$driver" -print-prog-name="$tool")
      [[ $located == */* ]] || located=$(command -v "$located")
      realpath "$located"
    done
    done
    find "$EXPERIMENT_TOOLCHAIN/prefix" -type f \( -name '*.exe' -o -name '*.dll' \)
    find "$EXPERIMENT_TOOLCHAIN/wrappers" -type f
  } | LC_ALL=C sort -u | while IFS= read -r file; do sha256sum "$file"; done
}
tool_snapshot > "$output/provenance/tool-binaries-before.txt"
selected_ld=$("$CC" -print-prog-name=ld)
[[ $selected_ld == */* ]] || selected_ld=$(command -v "$selected_ld")
"$selected_ld" --help > "$output/provenance/reader-linker-help.txt"
grep -F -- '--no-insert-timestamp' "$output/provenance/reader-linker-help.txt" > /dev/null
cp "$EXPERIMENT_TOOLCHAIN/provenance/compiler-packages.txt" "$output/provenance/"
cxx_command=(stormtty-gxx-15 -std=gnu++17 -DSTORMTTY_COMBINED_CONSOLE_TRACE=1)
export CXX="${cxx_command[*]}"
{ "$CC" --version; "${cxx_command[@]}" --version; } > "$output/provenance/selected-toolchain.txt"
cplusplus=$("${cxx_command[@]}" -dM -E -x c++ /dev/null | awk '$2 == "__cplusplus" {print $3}')
[[ $cplusplus == 201703L ]]
printf 'CXX=%s\n__cplusplus=%s\n' "$CXX" "$cplusplus" > "$output/provenance/compiler-mode.txt"
printf 'int main() { volatile int value = 0; ++value; --value; return value; }\n' |
  "${cxx_command[@]}" -Wall -Wextra -Werror -x c++ -fsyntax-only -
command -v makeinfo > "$output/provenance/makeinfo.txt"
makeinfo --version >> "$output/provenance/makeinfo.txt"
tar --zstd --no-same-owner -xf "$archive" -C "$output"
cd "$output/msys2-runtime"
echo '3fc783146515f8882e2c1ec57c3d9029294141acf7a8edf68244bb386214d84a  PKGBUILD' | sha256sum -c -
cp PKGBUILD "$output/provenance/PKGBUILD"
[[ $(git --git-dir=msys2-runtime rev-parse refs/tags/cygwin-3.6.11) == 9408d4c6c8273c19d19b5a788e73155fff2e4c01 ]]
[[ $(git --git-dir=msys2-runtime rev-parse 'refs/tags/cygwin-3.6.11^{commit}') == 867eb7748e6e5a0e8430ce9f46af27b4b1e8f204 ]]
[[ $(git --git-dir=msys2-runtime show -s --format=%ct 867eb7748e6e5a0e8430ce9f46af27b4b1e8f204) == "$SOURCE_DATE_EPOCH" ]]
git --git-dir=msys2-runtime show -s --format='%H%n%ct%n%cI' 867eb7748e6e5a0e8430ce9f46af27b4b1e8f204 > "$output/provenance/source-epoch.txt"
[[ $(cat msys2-runtime.commit) == 00eb8435b1c498e9211c7a7cee01584d76bd5f21 ]]
# Pin build/output locations rather than inheriting a runner's makepkg overrides.
config="$output/provenance/makepkg.conf"
cp /etc/makepkg.conf "$config"
{ printf '\nBUILDDIR=\nPKGDEST=%q\nSRCDEST=%q\nMAKEFLAGS=%q\n' \
    "$output/packages" "$output/msys2-runtime" '-j2 -k'; } >> "$config"
printf 'export SOURCE_DATE_EPOCH=%q\n' "$SOURCE_DATE_EPOCH" >> "$config"
printf 'export CC=%q\n' "$CC" >> "$config"
printf 'export CXX=%q\n' "$CXX" >> "$config"
# --holdver uses the verified archive's bundled Git source, not a remote update.
# Dependencies must be installed beforehand in the isolated build environment.
# No --syncdeps, --install, --skipinteg, or --nocheck.
makepkg --config "$config" --nobuild --holdver --noconfirm
cd src/msys2-runtime
[[ $(git hash-object winsup/cygwin/fhandler/console.cc) == a7cdbed500c6235e21f41fe9d0be5075a8c471f1 ]]
for file in fhandler/console.cc fhandler/termios.cc local_includes/fhandler.h; do
  cp "winsup/cygwin/$file" "$output/provenance/$(basename "$file").before"
done
cp winsup/cygwin/fhandler/console.cc "$output/provenance/console-before.cc"
# Common utility-only compatibility, NOT the experimental runtime behavior patch.
[[ $(git hash-object winsup/utils/mingw/cygcheck.cc) == 22c7d181aede363995405698ec11673fa5b15ac8 ]]
[[ $(git hash-object winsup/utils/mingw/dump_setup.cc) == 050679a0d32758b52c1b73f4e3c9a143e2a72264 ]]
[[ $(git hash-object winsup/utils/mingw/strace.cc) == a6b2e5d5488c1726ec2bb713a30eac409f6ee938 ]]
cp winsup/utils/mingw/strace.cc "$output/provenance/strace-before.cc"
cp winsup/utils/mingw/cygcheck.cc "$output/provenance/cygcheck-before.cc"
cp winsup/utils/mingw/dump_setup.cc "$output/provenance/dump_setup-before.cc"
git apply --check "$here/0000-utils-const-compat.patch"
git apply "$here/0000-utils-const-compat.patch"
cp winsup/utils/mingw/strace.cc "$output/provenance/strace-after.cc"
cp winsup/utils/mingw/cygcheck.cc "$output/provenance/cygcheck-after.cc"
cp winsup/utils/mingw/dump_setup.cc "$output/provenance/dump_setup-after.cc"
git apply --check "$behavior/0007-preserve-copied-on-signal.patch"
git apply "$behavior/0007-preserve-copied-on-signal.patch"
if [[ $variant == combined ]]; then
  echo '1afc3483da5dcc8848f336619f858c0ffcfbef608d1446464e6566135b2aff66  '"$here/0006-refined-prefix.patch" | sha256sum -c -
  git apply --check "$here/0006-refined-prefix.patch"
  git apply "$here/0006-refined-prefix.patch"
fi
# Reject source drift before a full native build. Only the exact intended
# copied or combined source satisfies these identities.
# Pre-observer source means behavior A/B already applied, matching linked-gate.
python3 "$diagnostic/verify-source.py" "$variant" winsup/cygwin before
cp winsup/cygwin/fhandler/console.cc "$output/provenance/console.cc.before"
git apply --check "$combined/$variant.patch"
git apply "$combined/$variant.patch"
python3 "$diagnostic/verify-source.py" "$variant" winsup/cygwin after
cp "$combined/$variant.patch" "$output/provenance/diagnostic.patch"
cp "$combined/observer.hpp" "$output/provenance/stormtty-combined-path.h"
cp "$combined/identities.json" "$output/provenance/diagnostic-identities.json"
for file in fhandler/console.cc fhandler/termios.cc local_includes/fhandler.h; do
  cp "winsup/cygwin/$file" "$output/provenance/$(basename "$file").after"
done
cp winsup/cygwin/fhandler/console.cc "$output/provenance/console-after.cc"
git diff --binary > "$output/provenance/all-packaging-and-experimental.patch"
# git diff omits the new untracked header; retain it explicitly, including source.
git diff --no-index -- /dev/null winsup/cygwin/local_includes/stormtty-combined-path.h >> "$output/provenance/all-packaging-and-experimental.patch" || [[ $? == 1 ]]
cd ../..
makepkg --config "$config" --noextract --holdver --noconfirm
find src -name Makefile -exec grep -H '^CXX = ' {} \; > "$output/provenance/configured-cxx.txt"
grep -F -- '-std=gnu++17' "$output/provenance/configured-cxx.txt" > /dev/null
pacman -Q > "$output/provenance/pacman-after.txt"
cmp "$output/provenance/pacman-before.txt" "$output/provenance/pacman-after.txt"
# Build output stays separate from the running build environment's DLL.
dll="$output/msys2-runtime/src/dest/usr/bin/msys-2.0.dll"
[[ -f $dll ]]
objdump -p "$dll" > "$output/provenance/diagnostic-dll-pe.txt"
objdump -s -j .gnu_debuglink "$dll" > "$output/provenance/dll-debuglink.txt"
mapfile -t version_sources < <(find "$output/msys2-runtime/src" -type f -path '*/cygwin/version.cc')
[[ ${#version_sources[@]} == 1 ]]
cp "${version_sources[0]}" "$output/provenance/generated-version.cc"
# Preserve the ACTUAL runtime translation unit before deleting generated trees.
# Standalone smoke compilation has different runtime-internal include context.
mapfile -t console_objects < <(find "$output/msys2-runtime/src" -type f -path '*/fhandler/console.o')
[[ ${#console_objects[@]} == 1 ]]
objdump -dr "${console_objects[0]}" > "$output/provenance/console-native-disassembly.txt"
objdump -t "${console_objects[0]}" > "$output/provenance/console-native-symbols.txt"
sha256sum "${console_objects[0]}" > "$output/provenance/console-native-object-sha256.txt"
# Compile only. The reader resolves v2 exports dynamically, so compilation
# uses the trusted installed import library, never loads the experimental DLL.
"$CC" -std=gnu11 -O2 -Wall -Wextra -Werror \
  -c "$combined/native/reader.c" -o "$output/reader.o"
"$CC" -Wl,--no-insert-timestamp "$output/reader.o" -o "$output/reader.exe"
objdump -dr "$output/reader.o" > "$output/provenance/reader-object-disassembly.txt"
objdump -t "$output/reader.o" > "$output/provenance/reader-object-symbols.txt"
objdump -p "$output/reader.exe" > "$output/provenance/reader-pe.txt"
objdump -d "$output/reader.exe" > "$output/provenance/reader-linked-disassembly.txt"
sha256sum "$output/reader.o" "$output/reader.exe" > "$output/provenance/reader-hashes.txt"
cp -- "$dll" "$output/experimental-msys-2.0.dll"
sha256sum "$output/experimental-msys-2.0.dll" "$output/provenance/0007-preserve-copied-on-signal.patch" > "$output/provenance/hashes.txt"
printf '%s\n' "$variant" > "$output/provenance/variant.txt"
sha256sum "$output"/packages/*.pkg.tar.* > "$output/provenance/package-hashes.txt"
# Delete only this invocation's generated source/build tree after securing the
# copied DLL, full original archive, recipe, all applied changes and inventories.
# Candidate packages and logs remain private/ephemeral; no system cleanup.
cmp "$archive" "$output/provenance/msys2-runtime-3.6.11-4.src.tar.zst"
[[ -f "$output/provenance/all-packaging-and-experimental.patch" && -s "$output/experimental-msys-2.0.dll" ]]
cd "$output"
rm -rf -- "$output/msys2-runtime"
df -k "$output" | tee "$output/provenance/disk-after.txt"
tool_snapshot > "$output/provenance/tool-binaries-after.txt"
cmp "$output/provenance/tool-binaries-before.txt" "$output/provenance/tool-binaries-after.txt"
python3 "$diagnostic/source-manifest.py" "$output/provenance/source-before.json" --check
python3 "$diagnostic/inspect-built.py" "$output" "$variant"
echo "COMBINED_BUILD_ONLY_COMPLETE=$output"
