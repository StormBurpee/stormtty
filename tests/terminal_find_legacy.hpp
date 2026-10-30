#pragma once

#include "stormtty/session.hpp"
#include "stormtty/terminal_find.hpp"

// Pre-extraction characterization of the search state in NativeWindow at
// babd2c4. Intentionally independent of the production controller. Host-owned
// capture/IME/invalidation between these operations is not simulated here.
namespace stormtty::test {

class LegacyTerminalFind {
public:
    enum class Update { rejected, unchanged, changed };

    void activate(SessionId session) {
        session_ = session;
        query_.select_all();
    }

    void reset(bool clear_error) {
        session_ = {};
        request_ = 0;
        initial_.cancel();
        result_.reset();
        if (clear_error) {
            error_.clear();
        }
    }

    void submit(Session& actor) {
        error_.clear();
        result_.reset();
        (void)actor.clear_selection();
        const auto request = query_.text().empty()
            ? actor.cancel_search()
            : actor.search({query_.text()});
        request_ = request.request_id;
        initial_.begin(query_.text().empty() ? 0 : request.request_id);
        if (request.result != SessionSendResult::accepted) {
            error_ = "Search unavailable";
        }
    }

    Update adopt(SearchSnapshotPtr result) {
        if (!result || !current_find_result(*result, request_, query_.text())) {
            return Update::rejected;
        }
        if (result_ == result) {
            return Update::unchanged;
        }
        result_ = std::move(result);
        return Update::changed;
    }

    std::optional<SearchToken> take_initial_selection() {
        return result_ ? initial_.take(*result_) : std::nullopt;
    }

    void navigate(Session* actor, bool previous) {
        initial_.cancel();
        if (!session_ || !actor) {
            return;
        }
        const auto result = actor->search_snapshot();
        if (result && current_find_result(*result, request_, query_.text()) &&
            result->status == SearchStatus::complete && !result->matches.empty()) {
            (void)actor->navigate_search(result->token,
                previous ? SearchDirection::backward : SearchDirection::forward, true);
        }
    }

    SessionId session() const { return session_; }
    TerminalFindQuery& query() { return query_; }
    const TerminalFindQuery& query() const { return query_; }
    std::uint64_t request() const { return request_; }
    const SearchSnapshotPtr& result() const { return result_; }
    const std::string& error() const { return error_; }
    const TerminalFindInitialSelection& initial_selection() const { return initial_; }

private:
    SessionId session_;
    TerminalFindQuery query_;
    SearchSnapshotPtr result_;
    TerminalFindInitialSelection initial_;
    std::uint64_t request_{};
    std::string error_;
};

} // namespace stormtty::test
