#pragma once
#include "stormtty/qualification_input_evidence.hpp"
#include <string>
namespace stormtty::qualification {
inline constexpr std::string_view settings_raw_payload="PASTE_QUAL_0123456789_[x]_end";
// Authored fixture only: never evaluate bytes read from stdin as shell commands.
inline constexpr std::string_view settings_raw_script=R"ZSH(emulate -R zsh
unsetopt MULTIBYTE
zmodload zsh/system || exit 91
stty raw -echo || exit 92
bytes=''
trap 'print -rn -- "$bytes" > "$1"; stty sane' EXIT
printf '\033[?2004hRAW_READY\r\n'
while true; do
  sysread -i 0 -s 1 -t 30 ch || exit 93
  printf -v one '%02x' "'$ch"
  (( ${#bytes} < 2048 )) || exit 94
  bytes+=$one
  if [[ $ch == $'\r' || $ch == $'\n' ]]; then
    print -rn -- "$bytes" > "$1" || exit 95
    exit 0
  fi
done
)ZSH";
inline std::string settings_raw_hex(std::string_view value) {
    constexpr char digits[]="0123456789abcdef";std::string out;
    for(unsigned char c:value){out+=digits[c>>4];out+=digits[c&15];}return out;
}
struct SettingsRawAssessment { std::string verdict="INCONCLUSIVE",writer_hex,child_hex,expected_hex;bool writer_complete{},child_valid{}; };
inline SettingsRawAssessment assess_settings_raw(const InputEvidence::Snapshot& snap,std::string_view child) {
    SettingsRawAssessment out;out.expected_hex=settings_raw_hex("\x1b[200~"+std::string(settings_raw_payload)+"\x1b[201~\r");
    out.child_valid=!child.empty()&&child.size()<=2048&&child.size()%2==0&&child.find_first_not_of("0123456789abcdef")==std::string_view::npos;
    if(out.child_valid)out.child_hex=child;
    out.writer_complete=!snap.missing_since_after&&!snap.byte_truncated_records&&!snap.records.empty();
    for(const auto& r:snap.records){out.writer_complete=out.writer_complete&&r.success&&r.accepted==r.requested&&r.stored>=r.accepted;
        out.writer_hex+=settings_raw_hex(std::string_view(reinterpret_cast<const char*>(r.bytes.data()),std::min<std::size_t>(r.stored,r.accepted)));}
    if(out.writer_complete&&out.child_valid)out.verdict=out.writer_hex==out.expected_hex&&out.child_hex==out.expected_hex?"PASS":"MISMATCH";
    return out;
}
}
