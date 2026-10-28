#!/usr/bin/env bash
# Experimental native MSYS build ONLY. Never installs a package or DLL.
set -euo pipefail
[[ $# == 3 ]] || { echo 'usage: build.sh VERIFIED_SOURCE_ARCHIVE NEW_OUTPUT_DIR control|patched' >&2; exit 2; }
archive=$(realpath "$1")
output=$(realpath -m "$2")
variant=$3
here=$(cd -- "$(dirname -- "$0")" && pwd)
[[ $variant == control || $variant == patched ]] || exit 2
[[ ${MSYSTEM:-} == MSYS && $(uname -m) == x86_64 ]] || { echo 'Native MSYS x86_64 build shell required' >&2; exit 2; }
[[ ! -e $output ]] || { echo 'Output must not exist' >&2; exit 2; }
echo '4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204  '"$archive" | sha256sum -c -
mkdir -p "$output/packages" "$output/provenance"
df -k "$output" | tee "$output/provenance/disk-before.txt"
available=$(df -Pk "$output" | awk 'NR==2 {print $4}')
(( available >= 6 * 1024 * 1024 )) || { echo 'Need at least 6 GiB free before runtime build' >&2; exit 1; }
# Preserve original source archive and experimental patch, including upstream licenses.
cp -- "$archive" "$output/provenance/msys2-runtime-3.6.11-4.src.tar.zst"
cp -- "$here/0001-retire-resize-before-signal.patch" "$output/provenance/"
pacman -Q > "$output/provenance/pacman-before.txt"
find /var/cache/pacman/pkg -maxdepth 1 -type f -name '*.pkg.tar.*' -exec sha256sum {} + > "$output/provenance/cached-toolchain-packages.sha256.txt"
{ uname -a; gcc --version; g++ --version; make --version; makepkg --version; } > "$output/provenance/toolchain.txt"
# Match the original GCC15 runtime build's GNU++17 dialect explicitly. GCC16
# defaults to GNU++20; the official recipe overwrites CXXFLAGS, but not CXX.
# This applies identically to control and patched, without relaxing -Werror.
cxx_command=(g++ -std=gnu++17)
export CXX="${cxx_command[*]}"
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
{ printf '\nBUILDDIR=\nPKGDEST=%q\nSRCDEST=%q\nMAKEFLAGS=-j2\n' \
    "$output/packages" "$output/msys2-runtime"; } >> "$config"
printf 'export CXX=%q\n' "$CXX" >> "$config"
# --holdver uses the verified archive's bundled Git source, not a remote update.
# Dependencies must be installed beforehand in the isolated build environment.
# No --syncdeps, --install, --skipinteg, or --nocheck.
makepkg --config "$config" --nobuild --holdver --noconfirm
cd src/msys2-runtime
[[ $(git hash-object winsup/cygwin/fhandler/console.cc) == a7cdbed500c6235e21f41fe9d0be5075a8c471f1 ]]
cp winsup/cygwin/fhandler/console.cc "$output/provenance/console-before.cc"
if [[ $variant == patched ]]; then
  git apply --check "$here/0001-retire-resize-before-signal.patch"
  git apply "$here/0001-retire-resize-before-signal.patch"
fi
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
sha256sum "$output/experimental-msys-2.0.dll" "$output/provenance/0001-retire-resize-before-signal.patch" > "$output/provenance/hashes.txt"
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
