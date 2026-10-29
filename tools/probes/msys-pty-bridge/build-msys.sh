#!/usr/bin/env bash
# Compile with an ALREADY verified official/private MSYS compiler. No install,
# download, runtime replacement, package update or mutation of the caller's HOME.
set -euo pipefail
[[ $# == 2 && ${MSYSTEM:-} == MSYS && $(uname -m) == x86_64 ]] || exit 2
compiler=$(realpath "$1")
output=$(realpath -m "$2")
[[ -x $compiler && ! -e $output ]] || exit 2
here=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$output"
{ "$compiler" --version; "$compiler" -dumpmachine; uname -a; } > "$output/toolchain.txt"
printf '#ifndef __CYGWIN__\n#error MSYS POSIX runtime compiler required, not MinGW\n#endif\nint main(void){return 0;}\n' |
  "$compiler" -x c - -o "$output/compiler-check.exe"
for name in broker fixture; do
  "$compiler" -std=c11 -O2 -static-libgcc -Wall -Wextra -Werror "$here/$name.c" -o "$output/$name.exe"
done
# Preserve evidence; ldd here inspects only the just-built trusted fixture files.
ldd "$output/broker.exe" "$output/fixture.exe" > "$output/dependencies.txt"
grep -qi 'msys-2.0.dll' "$output/dependencies.txt"
! grep -qi 'cygwin1.dll' "$output/dependencies.txt"
sha256sum "$here"/*.c "$output"/*.exe > "$output/hashes.txt"
echo 'COMPILED_ONLY: Windows runtime qualification has not run.'
