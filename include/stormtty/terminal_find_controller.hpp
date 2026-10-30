#pragma once

#include "stormtty/terminal_find.hpp"

namespace stormtty {

class Session;

// UI-thread search lifecycle, not an input router or actor owner. The host binds
// a stable ID and retains that actor for each synchronous Session borrow. Query
// editing is the existing bounded editor; the host decides when to submit it.
class TerminalFindController final {
public:
    enum class Update { rejected, unchanged, changed };
    enum class ResetReason { explicit_close, focus_loss };

    void activate(SessionId session);
    void reset(ResetReason reason);
    void submit(Session& actor);

    // Keep adoption and selection separate: the host invalidates chrome and
    // records diagnostics after adoption, before submitting initial selection.
    Update adopt(SearchSnapshotPtr result);
    std::optional<SearchToken> take_initial_selection();

    // Begin before actor lookup: explicit intent cancels automatic selection
    // even when no actor remains. Dispatch then reads a fresh owned snapshot.
    void begin_navigation();
    void navigate(Session& actor, bool previous);

    SessionId session() const;
    TerminalFindQuery& query();
    const TerminalFindQuery& query() const;
    std::uint64_t request() const;
    const SearchSnapshotPtr& result() const;
    const std::string& error() const;
    const TerminalFindInitialSelection& initial_selection() const;

private:
    SessionId session_;
    TerminalFindQuery query_;
    SearchSnapshotPtr result_;
    TerminalFindInitialSelection initial_;
    std::uint64_t request_{};
    std::string error_;
};

} // namespace stormtty
