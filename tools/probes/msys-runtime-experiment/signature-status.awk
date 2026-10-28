# GnuPG status-fd protocol; only one expected, fully trusted signature accepted.
$1 == "[GNUPG:]" {
  if ($2 == "GOODSIG") { good++; if ($3 != "FA11531AA0AA7F57") bad=1 }
  if ($2 == "VALIDSIG") {
    valid++
    if ($3 != "5F944B027F7FE2091985AA2EFA11531AA0AA7F57") bad=1
  }
  if ($2 ~ /^TRUST_(FULLY|ULTIMATE)$/) trusted++
  if ($2 ~ /^(BADSIG|ERRSIG|EXPSIG|EXPKEYSIG|REVKEYSIG|KEYEXPIRED|SIGEXPIRED|FAILURE|ERROR|NO_PUBKEY|NODATA|BADARMOR)$/) bad=1
  if ($2 ~ /^TRUST_(UNDEFINED|NEVER|MARGINAL)$/) bad=1
}
END { exit !(good == 1 && valid == 1 && trusted == 1 && !bad) }
