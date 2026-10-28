#!/usr/bin/env bash
# Experimental native MSYS build ONLY. Never installs a package or DLL.
set -euo pipefail
[[ $# == 3 ]] || { echo 'usage: build-trace.sh VERIFIED_SOURCE_ARCHIVE NEW_OUTPUT_DIR control|instrumented' >&2; exit 2; }
archive=$(realpath "$1")
output=$(realpath -m "$2")
variant=$3
here=$(cd -- "$(dirname -- "$0")" && pwd)
[[ $variant == control || $variant == instrumented ]] || exit 2
[[ ${MSYSTEM:-} == MSYS && $(uname -m) == x86_64 ]] || { echo 'Native MSYS x86_64 build shell required' >&2; exit 2; }
[[ ! -e $output ]] || { echo 'Output must not exist' >&2; exit 2; }
echo '4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204  '"$archive" | sha256sum -c -
mkdir -p "$output/packages" "$output/provenance"
df -k "$output" | tee "$output/provenance/disk-before.txt"
available=$(df -Pk "$output" | awk 'NR==2 {print $4}')
(( available >= 6 * 1024 * 1024 )) || { echo 'Need at least 6 GiB free before runtime build' >&2; exit 1; }
# Preserve original source archive and experimental patch, including upstream licenses.
cp -- "$archive" "$output/provenance/msys2-runtime-3.6.11-4.src.tar.zst"
cp -- "$here/0002-resize-observation-trace.patch" "$output/provenance/"
cp -- "$here/0000-utils-const-compat.patch" "$output/provenance/"
pacman -Q > "$output/provenance/pacman-before.txt"
find /var/cache/pacman/pkg -maxdepth 1 -type f -name '*.pkg.tar.*' -exec sha256sum {} + > "$output/provenance/cached-toolchain-packages.sha256.txt"
{ uname -a; gcc --version; g++ --version; make --version; makepkg --version; } > "$output/provenance/system-toolchain.txt"
# Match the original GCC15 runtime build's GNU++17 dialect explicitly. GCC16
# defaults to GNU++20; the official recipe overwrites CXXFLAGS, but not CXX.
# This applies identically to control and instrumented, without relaxing -Werror.
[[ -n ${EXPERIMENT_TOOLCHAIN:-} ]]
export PATH="$EXPERIMENT_TOOLCHAIN/wrappers:$PATH"
export CC=stormtty-gcc-15
[[ $($CC -dumpfullversion) == 15.3.0 ]]
[[ $(stormtty-gxx-15 -dumpfullversion) == 15.3.0 ]]
cp "$EXPERIMENT_TOOLCHAIN/provenance/compiler-packages.txt" "$output/provenance/"
cxx_command=(stormtty-gxx-15 -std=gnu++17)
if [[ $variant == instrumented ]]; then
  cxx_command+=(-DSTORMTTY_MSYS_RESIZE_TRACE=1)
fi
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
if [[ $variant == instrumented ]]; then
  git apply --check "$here/0002-resize-observation-trace.patch"
  git apply "$here/0002-resize-observation-trace.patch"
fi
for file in fhandler/console.cc fhandler/termios.cc local_includes/fhandler.h; do
  cp "winsup/cygwin/$file" "$output/provenance/$(basename "$file").after"
done
cp winsup/cygwin/fhandler/console.cc "$output/provenance/console-after.cc"
git diff --binary > "$output/provenance/all-packaging-and-experimental.patch"
cd ../..
makepkg --config "$config" --noextract --holdver --noconfirm
find src -name Makefile -exec grep -H '^CXX = ' {} \; > "$output/provenance/configured-cxx.txt"
grep -F -- '-std=gnu++17' "$output/provenance/configured-cxx.txt" > /dev/null
pacman -Q > "$output/provenance/pacman-after.txt"
cmp "$output/provenance/pacman-before.txt" "$output/provenance/pacman-after.txt"
# Build output stays separate from the running build environment's DLL.
dll="$output/msys2-runtime/src/dest/usr/bin/msys-2.0.dll"
[[ -f $dll ]]
cp -- "$dll" "$output/experimental-msys-2.0.dll"
sha256sum "$output/experimental-msys-2.0.dll" "$output/provenance/0002-resize-observation-trace.patch" > "$output/provenance/hashes.txt"
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
echo "EXPERIMENTAL_BUILD_COMPLETE=$output"
