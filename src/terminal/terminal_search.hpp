#pragma once

#include "stormtty/terminal.hpp"
#include <ghostty/vt.h>

namespace stormtty {

// Private, actor-confined incremental search owner. The caller owns the terminal
// and revision and must check thread/callback failures before entering this class.
// Terminal handles are borrowed synchronously; the engine search must be destroyed
// before its terminal. Neither GridRefs nor selections escape a call.
class TerminalSearch final {
public:
    struct SelectionChoice {
        SearchApplyResult result;
        std::size_t index{};
        SearchMatch match{};
    };

    TerminalSearch() = default;
    ~TerminalSearch();
    TerminalSearch(const TerminalSearch&) = delete;
    TerminalSearch& operator=(const TerminalSearch&) = delete;
    TerminalSearch(TerminalSearch&&) = delete;
    TerminalSearch& operator=(TerminalSearch&&) = delete;

    void initialize(GhosttyTerminal terminal, std::uint64_t revision);
    void begin(GhosttyTerminal terminal, std::uint64_t revision,
               std::uint64_t request_id, SearchOptions options);
    void cancel(GhosttyTerminal terminal, std::uint64_t revision,
                std::uint64_t request_id);
    bool needs_work(std::uint64_t revision) const;
    SearchSnapshotPtr step(GhosttyTerminal terminal, std::uint64_t revision);
    SearchSnapshotPtr snapshot() const { return result_; }

    // Choices contain only owned coordinates. TerminalCore resolves and applies
    // them without an intervening terminal mutation, then records the outcome.
    SelectionChoice choose(SearchToken token, std::uint64_t revision,
                           std::size_t index) const;
    SelectionChoice choose(SearchToken token, std::uint64_t revision,
                           SearchDirection direction, bool wrap) const;
    SearchApplyResult record_action(SearchApplyResult result,
                                   std::optional<std::size_t> applied_index = {});

private:
    // Ghostty may swallow allocation failures. Sticky flags prevent a partial
    // engine result from being published as a successful complete search.
    struct Allocator {
        std::size_t limit{}, used{}, peak{};
        bool limited{}, failed{};
        static void* allocate(void*, std::size_t, std::uint8_t, std::uintptr_t) noexcept;
        static bool resize(void*, void*, std::size_t, std::uint8_t,
                           std::size_t, std::uintptr_t) noexcept;
        static void* remap(void*, void*, std::size_t, std::uint8_t,
                           std::size_t, std::uintptr_t) noexcept;
        static void release(void*, void*, std::size_t, std::uint8_t, std::uintptr_t) noexcept;
        static const GhosttyAllocatorVtable vtable;
        GhosttyAllocator abi{this, &vtable};
    };

    struct SearchHandle {
        GhosttySearch value{};
        ~SearchHandle();
        operator GhosttySearch() const noexcept { return value; }
    };

    void reset_handle() noexcept;
    SearchSnapshotPtr publish(GhosttyTerminal terminal, std::uint64_t revision,
                              SearchStatus status, std::string message = {},
                              std::vector<SearchMatch> matches = {});
    SearchSnapshotPtr stop(GhosttyTerminal terminal, std::uint64_t revision,
                           SearchStatus status, std::string message);
    bool allocation_ok(GhosttyTerminal terminal, std::uint64_t revision);

    std::uint64_t request_id_{}, fed_revision_{}, work_{}, update_{}, generation_{};
    Allocator memory_; // Stable address; outlives the engine handle using its ABI.
    SearchHandle handle_;
    SearchOptions options_;
    SearchSnapshotPtr result_;
    std::vector<SearchMatch> building_;
    std::size_t count_{};
    bool materializing_{};
    std::optional<std::size_t> selected_;
};

} // namespace stormtty
