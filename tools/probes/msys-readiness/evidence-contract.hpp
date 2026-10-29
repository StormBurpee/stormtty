#pragma once
#include "stormtty/qualification_input_evidence.hpp"
#include <string_view>

namespace stormtty::readiness {
// Only standalone terminal replies are excluded from writer attribution.
// Child bytes are never filtered by this helper or by the fixture.
inline bool cursor_reply(std::string_view value) {
    if (!value.starts_with("\x1b[") || !value.ends_with("R")) return false;
    value.remove_prefix(2); value.remove_suffix(1);
    const auto separator = value.find(';');
    if (separator == std::string_view::npos) return false;
    const auto number = [](std::string_view part) {
        return !part.empty() && part.size() <= 5 && part.front() != '0' &&
            part.find_first_not_of("0123456789") == std::string_view::npos;
    };
    return number(value.substr(0, separator)) && number(value.substr(separator + 1));
}

inline bool intact_authored_writes(const qualification::InputEvidence::Snapshot& snapshot,
                                  std::string_view expected) {
    if (expected.empty() || snapshot.missing_since_after || snapshot.overwritten ||
        snapshot.byte_truncated_records) return false;
    unsigned authored{};
    for (const auto& record : snapshot.records) {
        if (!record.success || record.error || record.offset || record.message == 0 ||
            record.accepted != record.requested || record.reported != record.accepted ||
            record.stored < record.accepted) return false;
        const std::string_view bytes(reinterpret_cast<const char*>(record.bytes.data()), record.accepted);
        if (cursor_reply(bytes)) continue;
        const auto wanted = authored == 0 ? expected.substr(0, expected.size() - 1) : expected.substr(expected.size() - 1);
        if (authored >= 2 || bytes != wanted) return false;
        ++authored;
    }
    return authored == 2;
}
} // namespace stormtty::readiness
