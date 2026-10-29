#pragma once
// Qualification-only ASCII protocol. '-' means deletion; empty hex means set
// to empty. Never dereference a disengaged ProcessEnvironmentEntry value.
#include "stormtty/process.hpp"
#include <ostream>
namespace stormtty::qualification {
inline void write_environment_override(std::ostream& output, const ProcessEnvironmentEntry& entry) {
    constexpr char hex[]="0123456789abcdef";
    output << entry.name << '=';
    if (!entry.value) output << '-';
    else for (const unsigned char byte:*entry.value) output << hex[byte>>4] << hex[byte&15];
    output << '\n';
}
}
