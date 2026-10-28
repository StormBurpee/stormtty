#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
good=$'[GNUPG:] GOODSIG FA11531AA0AA7F57 Vendor\n[GNUPG:] VALIDSIG 5F944B027F7FE2091985AA2EFA11531AA0AA7F57 date\n[GNUPG:] TRUST_FULLY 0 pgp\n'
check() {
  local expected=$1 evidence=$2 result=0
  printf '%s' "$evidence" | awk -f "$here/signature-status.awk" || result=$?
  [[ $result == "$expected" ]] || { echo 'Signature status parser test failed' >&2; exit 1; }
}
check 0 "$good"
check 0 "${good/TRUST_FULLY/TRUST_ULTIMATE}"
check 1 "${good/5F944B/0F944B}"
check 1 "${good/GOODSIG FA11531AA0AA7F57/GOODSIG 0000000000000000}"
check 1 "${good/TRUST_FULLY/TRUST_UNDEFINED}"
check 1 "${good/TRUST_FULLY/TRUST_NEVER}"
check 1 "${good/TRUST_FULLY/TRUST_MARGINAL}"
check 1 "${good/GOODSIG/IGNORED}"
check 1 "${good/VALIDSIG/IGNORED}"
check 1 "${good/TRUST_FULLY/IGNORED}"
check 1 "$good$good"
check 1 ''
for status in BADSIG ERRSIG EXPSIG EXPKEYSIG REVKEYSIG KEYEXPIRED SIGEXPIRED FAILURE ERROR NO_PUBKEY NODATA BADARMOR; do
  check 1 "$good"$'\n'"[GNUPG:] $status rejected"
done
echo '24 signature status parser tests passed'
