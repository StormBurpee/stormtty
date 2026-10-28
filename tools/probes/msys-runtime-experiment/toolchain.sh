#!/usr/bin/env bash
# Extract verified historical compiler into a NEW private prefix; never install.
set -euo pipefail
[[ $# == 1 && ${MSYSTEM:-} == MSYS ]] || exit 2
root=$(realpath -m "$1")
here=$(cd -- "$(dirname -- "$0")" && pwd)
bash "$here/test-signature-status.sh"
[[ ! -e $root ]] || exit 2
mkdir -p "$root/downloads" "$root/prefix" "$root/wrappers" "$root/provenance"
# The runner's readable public trust store can fail pacman-key's non-root
# wrapper precheck. Clone only public verification state; no system edits,
# secret keys, new signing keys, trust overrides, or signature bypasses.
keyring=$(pacman-conf gpgdir)
# Public-only state; inherit runner scratch ACL rather than Unix chmod on NTFS.
mkdir "$root/keyring"
cp "$keyring/pubring.gpg" "$keyring/trustdb.gpg" "$root/keyring/"
# Some GnuPG installations retain the public keys in a keybox instead.
if [[ -f $keyring/pubring.kbx ]]; then cp "$keyring/pubring.kbx" "$root/keyring/"; fi
printf 'no-auto-key-retrieve\n' > "$root/keyring/gpg.conf"
{ printf 'Public verifier source: %s\n' "$keyring"
  pacman -Q msys2-keyring gnupg pacman
  for file in pubring.gpg trustdb.gpg pubring.kbx; do
    [[ -f $root/keyring/$file ]] || continue
    cmp "$keyring/$file" "$root/keyring/$file"
    sha256sum "$keyring/$file" "$root/keyring/$file"
  done
  sha256sum "$root/keyring/gpg.conf"
} > "$root/provenance/verification-keyring.txt"
cd "$root/downloads"
while read -r hash name directory; do
  url="https://repo.msys2.org/msys/$directory/$name"
  curl --fail --location "$url" -o "$name"
  curl --fail --location "$url.sig" -o "$name.sig"
  echo "$hash  $name" | sha256sum -c -
  # Same full/ultimate trust requirement as pacman-key, with explicit signer,
  # process-success and expiry/revocation/error checks. No wrapper lock override.
  status="$root/provenance/$name.status.txt"
  gpg --homedir "$root/keyring" --batch --status-fd 1 --verify "$name.sig" "$name" > "$status" 2> "$root/provenance/$name.signature.txt"
  awk -f "$here/signature-status.awk" "$status"
  printf '%s  %s\n' "$hash" "$url" >> "$root/provenance/compiler-packages.txt"
  if [[ $directory == x86_64 ]]; then
    # Only usr contents, preserving both packages' licenses. No active runtime DLL.
    tar --zstd --no-same-owner -xf "$name" -C "$root/prefix" usr
    tar --zstd -xOf "$name" .PKGINFO > "$root/provenance/$name.PKGINFO.txt"
    tar --zstd -xOf "$name" .BUILDINFO > "$root/provenance/$name.BUILDINFO.txt"
  fi
done <<'PACKAGES'
f8e7e4bf21bb8f690058384fd6ee27bb3567edad49077e691d1f12f0a42119f1 gcc-15.3.0-1-x86_64.pkg.tar.zst x86_64
0d99a122c453c05ae21ba3dcea910f2e0d93d38ae067c677a112a315b0f3cec5 gcc-libs-15.3.0-1-x86_64.pkg.tar.zst x86_64
110eaa240aaced2ec2d05b1ce1c7d7273b9549c60ac17cb571bd28781766522d gcc-15.3.0-1.src.tar.zst sources
PACKAGES
[[ ! -e "$root/prefix/usr/bin/msys-2.0.dll" ]]
# Private DLL lookup is scoped to compiler children, not makepkg/cross compiler.
for pair in 'gcc gcc' 'gxx g++'; do
  read -r name exe <<< "$pair"
  { printf '#!/usr/bin/env bash\nset -euo pipefail\n'
    printf 'export PATH=%q:"$PATH"\n' "$root/prefix/usr/bin"
    printf 'exec %q "$@"\n' "$root/prefix/usr/bin/$exe"
  } > "$root/wrappers/stormtty-$name-15"
  chmod +x "$root/wrappers/stormtty-$name-15"
done
export PATH="$root/wrappers:$PATH"
[[ $(stormtty-gcc-15 -dumpfullversion) == 15.3.0 ]]
[[ $(stormtty-gxx-15 -dumpfullversion) == 15.3.0 ]]
cc1plus=$(stormtty-gxx-15 -print-prog-name=cc1plus)
[[ $(realpath "$cc1plus") == "$root/prefix/"* && -f $cc1plus ]]
for file in crtbegin.o libgcc.a; do
  located=$(stormtty-gcc-15 -print-file-name="$file")
  [[ $(realpath "$located") == "$root/prefix/"* && -f $located ]]
done
for compiler in stormtty-gcc-15 stormtty-gxx-15; do
  "$compiler" -v 2>&1
  "$compiler" -print-search-dirs
  "$compiler" -print-file-name=crtbegin.o
  "$compiler" -print-file-name=libgcc.a
done > "$root/provenance/compiler-search.txt"
printf '#include <stdio.h>\nint main(void) { return puts("C compiler smoke") < 0; }\n' |
  stormtty-gcc-15 -Wall -Wextra -Werror -x c -o "$root/smoke-c.exe" -
printf '#include <iostream>\n#include <vector>\nstatic_assert(__GNUC__ == 15 && _GLIBCXX_RELEASE == 15, "private compiler and headers required");\nint main() { std::vector<int> v{15}; std::cout << v.at(0) << "\\n"; }\n' |
  stormtty-gxx-15 -std=gnu++17 -Wall -Wextra -Werror -v -x c++ -o "$root/smoke-cxx.exe" - 2> "$root/provenance/compiler-headers-link.txt"
header_found=false
while IFS= read -r path; do
  [[ $path == *include/c++ ]] || continue
  resolved=$(realpath -m "$path")
  if [[ $resolved == "$root/prefix/usr/lib/gcc/x86_64-pc-cygwin/15.3.0/include/c++" && -d $resolved ]]; then
    header_found=true
  fi
done < <(sed -n '/#include <...> search starts here:/,/End of search list./p' "$root/provenance/compiler-headers-link.txt" | sed 's/^ *//')
[[ $header_found == true ]]
# Use the same private compiler DLL set for the smoke executables only.
PATH="$root/prefix/usr/bin:$PATH" "$root/smoke-c.exe"
PATH="$root/prefix/usr/bin:$PATH" "$root/smoke-cxx.exe"
echo 'PRIVATE_GCC15_PREFLIGHT_PASS'
