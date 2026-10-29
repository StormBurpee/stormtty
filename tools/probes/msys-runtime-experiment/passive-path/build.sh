#!/usr/bin/env bash
# Experimental native MSYS build ONLY. Never installs a package or DLL.
set -euo pipefail
[[ $# == 3 ]] || { echo 'usage: passive-path/build.sh VERIFIED_SOURCE_ARCHIVE NEW_OUTPUT_DIR stock|refined' >&2; exit 2; }
archive=$(realpath "$1")
output=$(realpath -m "$2")
variant=$3
diagnostic=$(cd -- "$(dirname -- "$0")" && pwd)
here=$(dirname "$diagnostic")
[[ $variant == stock || $variant == refined ]] || exit 2
[[ ${MSYSTEM:-} == MSYS && $(uname -m) == x86_64 ]] || { echo 'Native MSYS x86_64 build shell required' >&2; exit 2; }
[[ ! -e $output ]] || { echo 'Output must not exist' >&2; exit 2; }
echo '4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204  '"$archive" | sha256sum -c -
echo '1afc3483da5dcc8848f336619f858c0ffcfbef608d1446464e6566135b2aff66  '"$here/0006-refined-prefix.patch" | sha256sum -c -
mkdir -p "$output/packages" "$output/provenance"
df -k "$output" | tee "$output/provenance/disk-before.txt"
available=$(df -Pk "$output" | awk 'NR==2 {print $4}')
(( available >= 6 * 1024 * 1024 )) || { echo 'Need at least 6 GiB free before runtime build' >&2; exit 1; }
# Preserve original source archive and experimental patch, including upstream licenses.
cp -- "$archive" "$output/provenance/msys2-runtime-3.6.11-4.src.tar.zst"
cp -- "$here/0006-refined-prefix.patch" "$output/provenance/"
cp -- "$here/0000-utils-const-compat.patch" "$output/provenance/"
pacman -Q > "$output/provenance/pacman-before.txt"
find /var/cache/pacman/pkg -maxdepth 1 -type f -name '*.pkg.tar.*' -exec sha256sum {} + > "$output/provenance/cached-toolchain-packages.sha256.txt"
{ uname -a; gcc --version; g++ --version; make --version; makepkg --version; } > "$output/provenance/system-toolchain.txt"
# Match the original GCC15 runtime build's GNU++17 dialect explicitly. GCC16
# defaults to GNU++20; the official recipe overwrites CXXFLAGS, but not CXX.
# This applies identically to stock and refined, without relaxing -Werror.
[[ -n ${EXPERIMENT_TOOLCHAIN:-} ]]
export PATH="$EXPERIMENT_TOOLCHAIN/wrappers:$PATH"
export CC=stormtty-gcc-15
[[ $($CC -dumpfullversion) == 15.3.0 ]]
[[ $(stormtty-gxx-15 -dumpfullversion) == 15.3.0 ]]
cp "$EXPERIMENT_TOOLCHAIN/provenance/compiler-packages.txt" "$output/provenance/"
cxx_command=(stormtty-gxx-15 -std=gnu++17 -DSTORMTTY_PASSIVE_CONSOLE_TRACE=1)
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
[[ $(cat msys2-runtime.commit) == 00eb8435b1c498e9211c7a7cee01584d76bd5f21 ]]
# Pin build/output locations rather than inheriting a runner's makepkg overrides.
config="$output/provenance/makepkg.conf"
cp /etc/makepkg.conf "$config"
{ printf '\nBUILDDIR=\nPKGDEST=%q\nSRCDEST=%q\nMAKEFLAGS=%q\n' \
    "$output/packages" "$output/msys2-runtime" '-j2 -k'; } >> "$config"
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
if [[ $variant == refined ]]; then
  git apply --check "$here/0006-refined-prefix.patch"
  git apply "$here/0006-refined-prefix.patch"
fi
# Reject source drift before a full native build. Only the exact intended
# stock or refined-retirement source satisfies these identities.
header_expected=c8e4a76e2a7aa5963763c62319e5f51ab08445be2f7ea6e3c1c314de91223650
console_expected=535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33
if [[ $variant == refined ]]; then
  header_expected=ef5f8cd8db11a874807d37661a87ac0a65a9e5bad04c6f37039bd9b814a5f3d4
  console_expected=bc68a642dca059ed76b6ae79b21db85e34ff3376f98a5efe4bb2b0136b79f0ae
fi
printf '%s  winsup/cygwin/fhandler/console.cc\n' "$console_expected" | sha256sum -c -
echo 'e65b607b331586533fd495dc228361b01b10f4ec85682abb2342b5c29867a36f  winsup/cygwin/fhandler/termios.cc' | sha256sum -c -
printf '%s  winsup/cygwin/local_includes/fhandler.h\n' "$header_expected" | sha256sum -c -
# Separate observation patch only; the stock/refined behavior above is unchanged.
python3 "$diagnostic/verify.py" "$variant" winsup/cygwin/fhandler/console.cc before
git apply --check "$diagnostic/$variant.patch"
git apply "$diagnostic/$variant.patch"
python3 "$diagnostic/verify.py" "$variant" winsup/cygwin/fhandler/console.cc after
cp "$diagnostic/$variant.patch" "$output/provenance/passive.patch"
cp "$diagnostic/recorder.hpp" "$output/provenance/stormtty-passive-path.h"
cp "$diagnostic/identities.json" "$output/provenance/passive-identities.json"
for file in fhandler/console.cc fhandler/termios.cc local_includes/fhandler.h; do
  cp "winsup/cygwin/$file" "$output/provenance/$(basename "$file").after"
done
cp winsup/cygwin/fhandler/console.cc "$output/provenance/console-after.cc"
git diff --binary > "$output/provenance/all-packaging-and-experimental.patch"
# git diff omits the new untracked header; retain it explicitly, including source.
git diff --no-index -- /dev/null winsup/cygwin/local_includes/stormtty-passive-path.h >> "$output/provenance/all-packaging-and-experimental.patch" || [[ $? == 1 ]]
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
# Preserve the ACTUAL runtime translation unit before deleting generated trees.
# Standalone smoke compilation has different runtime-internal include context.
mapfile -t console_objects < <(find "$output/msys2-runtime/src" -type f -path '*/fhandler/console.o')
[[ ${#console_objects[@]} == 1 ]]
objdump -dr "${console_objects[0]}" > "$output/provenance/console-native-disassembly.txt"
objdump -t "${console_objects[0]}" > "$output/provenance/console-native-symbols.txt"
sha256sum "${console_objects[0]}" > "$output/provenance/console-native-object-sha256.txt"
# Standalone native recorder object is supplementary, never runtime-TU proof.
"${cxx_command[@]}" -O2 -Wall -Wextra -Werror -c "$diagnostic/recorder-smoke.cpp" -o "$output/recorder-smoke.o"
objdump -dr "$output/recorder-smoke.o" > "$output/provenance/recorder-native-disassembly.txt"
objdump -t "$output/recorder-smoke.o" > "$output/provenance/recorder-native-symbols.txt"
python3 "$diagnostic/review-evidence.py" "$output/provenance"
cp -- "$dll" "$output/experimental-msys-2.0.dll"
sha256sum "$output/experimental-msys-2.0.dll" "$output/provenance/0006-refined-prefix.patch" > "$output/provenance/hashes.txt"
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
echo "PASSIVE_DIAGNOSTIC_BUILD_COMPLETE=$output"
