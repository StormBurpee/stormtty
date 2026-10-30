#include "stormtty/terminal_find_controller.hpp"
#include "stormtty/session.hpp"

namespace stormtty {

void TerminalFindController::activate(SessionId session) {
    session_ = session;
    query_.select_all();
}

void TerminalFindController::reset(ResetReason reason) {
    session_ = {};
    request_ = 0;
    initial_.cancel();
    result_.reset();
    if (reason == ResetReason::explicit_close) {
        error_.clear();
    }
}

void TerminalFindController::submit(Session& actor) {
    error_.clear();
    result_.reset();
    // Replacing a query, including a no-match or empty query, must remove the
    // old match highlight before the new search/cancellation is queued.
    (void)actor.clear_selection();
    const auto submission = query_.text().empty()
        ? actor.cancel_search()
        : actor.search({query_.text()});
    request_ = submission.request_id;
    initial_.begin(query_.text().empty() ? 0 : submission.request_id);
    if (submission.result != SessionSendResult::accepted) {
        error_ = "Search unavailable";
    }
}

TerminalFindController::Update TerminalFindController::adopt(SearchSnapshotPtr result) {
    if (!result || !current_find_result(*result, request_, query_.text())) {
        return Update::rejected;
    }
    if (result_ == result) {
        return Update::unchanged;
    }
    result_ = std::move(result);
    return Update::changed;
}

std::optional<SearchToken> TerminalFindController::take_initial_selection() {
    return result_ ? initial_.take(*result_) : std::nullopt;
}

void TerminalFindController::begin_navigation() {
    initial_.cancel();
}

void TerminalFindController::navigate(Session& actor, bool previous) {
    const auto result = actor.search_snapshot();
    if (result && current_find_result(*result, request_, query_.text()) &&
        result->status == SearchStatus::complete && !result->matches.empty()) {
        (void)actor.navigate_search(result->token,
            previous ? SearchDirection::backward : SearchDirection::forward, true);
    }
}

SessionId TerminalFindController::session() const {
    return session_;
}

TerminalFindQuery& TerminalFindController::query() {
    return query_;
}

const TerminalFindQuery& TerminalFindController::query() const {
    return query_;
}

std::uint64_t TerminalFindController::request() const {
    return request_;
}

const SearchSnapshotPtr& TerminalFindController::result() const {
    return result_;
}

const std::string& TerminalFindController::error() const {
    return error_;
}

const TerminalFindInitialSelection& TerminalFindController::initial_selection() const {
    return initial_;
}

} // namespace stormtty
