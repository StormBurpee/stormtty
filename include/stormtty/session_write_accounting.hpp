#pragma once
#include <cstdint>
namespace stormtty {
// Provenance only: no raw input is retained, inspected, rewritten or logged.
enum class SessionWriteOrigin { key, text, paste, mouse, focus, protocol };
struct SessionWriteAccounting {
    std::uint64_t key_bytes{},text_bytes{},paste_bytes{},mouse_bytes{},focus_bytes{},protocol_bytes{};
    void accepted(SessionWriteOrigin origin,std::uint64_t bytes) noexcept {
        switch(origin) {
        case SessionWriteOrigin::key:key_bytes+=bytes;break;
        case SessionWriteOrigin::text:text_bytes+=bytes;break;
        case SessionWriteOrigin::paste:paste_bytes+=bytes;break;
        case SessionWriteOrigin::mouse:mouse_bytes+=bytes;break;
        case SessionWriteOrigin::focus:focus_bytes+=bytes;break;
        case SessionWriteOrigin::protocol:protocol_bytes+=bytes;break;
        }
    }
    std::uint64_t frontend() const noexcept {return key_bytes+text_bytes+paste_bytes+mouse_bytes;}
    std::uint64_t total() const noexcept {return frontend()+focus_bytes+protocol_bytes;}
};
}
