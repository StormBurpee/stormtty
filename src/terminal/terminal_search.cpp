#include "terminal_search.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace stormtty {
namespace {
void checked(GhosttyResult result, const char* operation) {
    if (result != GHOSTTY_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (Ghostty result " +
                                 std::to_string(result) + ")");
}

bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned count{};
        std::uint32_t scalar{}, minimum{};
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1;
            scalar = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2;
            scalar = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3;
            scalar = first & 7;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (count > text.size() - i) return false;
        while (count--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
    }
    return true;
}
} // namespace

void* TerminalSearch::Allocator::allocate(
    void* context, std::size_t length, std::uint8_t alignment, std::uintptr_t) noexcept {
    auto& self = *static_cast<Allocator*>(context);
    if (length > self.limit - self.used) {
        self.limited = true;
        return nullptr;
    }
    if (alignment >= std::numeric_limits<std::size_t>::digits - 1) {
        self.failed = true;
        return nullptr;
    }
    const auto bytes = std::max<std::size_t>(alignof(std::max_align_t), std::size_t{1} << alignment);
    void* memory = ::operator new(length, std::align_val_t(bytes), std::nothrow);
    if (!memory) {
        self.failed = true;
        return nullptr;
    }
    self.used += length;
    self.peak = std::max(self.peak, self.used);
    return memory;
}

// Always fall back to allocate/copy/free so peak live bytes are accounted
// accurately, including shrinks (no retained larger physical allocation).
bool TerminalSearch::Allocator::resize(
    void*, void*, std::size_t, std::uint8_t, std::size_t, std::uintptr_t) noexcept {
    return false;
}

void* TerminalSearch::Allocator::remap(
    void*, void*, std::size_t, std::uint8_t, std::size_t, std::uintptr_t) noexcept {
    return nullptr;
}

void TerminalSearch::Allocator::release(
    void* context, void* memory, std::size_t length, std::uint8_t alignment, std::uintptr_t) noexcept {
    auto& self = *static_cast<Allocator*>(context);
    self.used -= length;
    const auto bytes = std::max<std::size_t>(alignof(std::max_align_t), std::size_t{1} << alignment);
    ::operator delete(memory, std::align_val_t(bytes));
}

const GhosttyAllocatorVtable TerminalSearch::Allocator::vtable{allocate, resize, remap, release};

TerminalSearch::~TerminalSearch() = default;

TerminalSearch::SearchHandle::~SearchHandle() {
    if (value) ghostty_search_free(value);
}

void TerminalSearch::reset_handle() noexcept {
    if (handle_) ghostty_search_free(handle_);
    handle_.value = {};
}

void TerminalSearch::initialize(GhosttyTerminal terminal, std::uint64_t revision) {
    publish(terminal, revision, SearchStatus::idle);
}

SearchSnapshotPtr TerminalSearch::publish(GhosttyTerminal terminal, std::uint64_t revision,
                                         SearchStatus status, std::string message,
                                         std::vector<SearchMatch> matches) {
    auto value = std::make_shared<SearchSnapshot>();
    value->token = {request_id_, revision, generation_};
    value->update_id = ++update_;
    value->work_units = work_;
    value->status = status;
    value->query = options_.query;
    value->message = std::move(message);
    value->matches_found = count_;
    GhosttyTerminalScreen screen{};
    checked(ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen), "search screen");
    value->alternate_screen = screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
    value->matches = std::move(matches);
    value->selected_index = selected_;
    result_ = std::move(value);
    return result_;
}

SearchSnapshotPtr TerminalSearch::stop(GhosttyTerminal terminal, std::uint64_t revision,
                                      SearchStatus status, std::string message) {
    reset_handle();
    building_.clear();
    materializing_ = false;
    selected_.reset();
    return publish(terminal, revision, status, std::move(message));
}

bool TerminalSearch::allocation_ok(GhosttyTerminal terminal, std::uint64_t revision) {
    if (memory_.limited) {
        stop(terminal, revision, SearchStatus::limited,
             "Search memory budget reached; narrow the query or raise its budget.");
        return false;
    }
    if (memory_.failed) {
        stop(terminal, revision, SearchStatus::error,
             "Search allocation failed; no complete results were published.");
        return false;
    }
    return true;
}

void TerminalSearch::begin(GhosttyTerminal terminal, std::uint64_t revision,
                           std::uint64_t request_id, SearchOptions options) {
    reset_handle();
    building_.clear();
    materializing_ = false;
    request_id_ = request_id;
    ++generation_;
    fed_revision_ = 0;
    work_ = 0;
    count_ = 0;
    selected_.reset();
    if (options.query.size() > 4096) {
        options_ = {};
        publish(terminal, revision, SearchStatus::error, "Search query exceeds the 4096-byte limit.");
        return;
    }
    options_ = std::move(options);
    const auto& config = options_;
    if (!request_id || config.query.size() > 4096 || !valid_utf8(config.query) ||
        !config.max_matches || config.max_matches > 4096 ||
        config.memory_budget_bytes < 64u * 1024u || config.memory_budget_bytes > 64u * 1024u * 1024u ||
        !config.max_work_units_per_step || config.max_work_units_per_step > 16 ||
        !config.max_total_work_units || config.max_total_work_units > 1000000) {
        publish(terminal, revision, SearchStatus::error,
                "Invalid UTF-8 query or search budget (query <=4096 bytes, matches 1..4096, "
                "memory 64KiB..64MiB, step 1..16, total 1..1000000).");
        return;
    }
    if (config.query.empty()) {
        publish(terminal, revision, SearchStatus::idle);
        return;
    }
    memory_.limit = config.memory_budget_bytes;
    memory_.limited = memory_.failed = false;
    memory_.peak = 0;
    const auto created = ghostty_search_new(&memory_.abi, &handle_.value, terminal);
    if (!allocation_ok(terminal, revision)) return;
    if (created != GHOSTTY_SUCCESS) {
        stop(terminal, revision, SearchStatus::error,
             "Could not create engine search.");
        return;
    }
    GhosttyString needle{reinterpret_cast<const std::uint8_t*>(config.query.data()), config.query.size()};
    const auto set = ghostty_search_set(handle_, GHOSTTY_SEARCH_OPT_NEEDLE, &needle);
    if (!allocation_ok(terminal, revision)) return;
    if (set != GHOSTTY_SUCCESS) {
        stop(terminal, revision, SearchStatus::error,
             "Could not set engine search query.");
        return;
    }
    publish(terminal, revision, SearchStatus::running);
}

void TerminalSearch::cancel(GhosttyTerminal terminal, std::uint64_t revision,
                            std::uint64_t request_id) {
    request_id_ = request_id;
    ++generation_;
    count_ = 0;
    stop(terminal, revision, SearchStatus::cancelled, {});
}

bool TerminalSearch::needs_work(std::uint64_t revision) const {
    return handle_ && (result_->status == SearchStatus::running || fed_revision_ != revision);
}

SearchSnapshotPtr TerminalSearch::step(GhosttyTerminal terminal, std::uint64_t revision) {
    try {
        if (!needs_work(revision)) return result_;
        if (result_->token.revision != revision) {
            // Content invalidates coordinates, not the request's cumulative budget.
            building_.clear();
            materializing_ = false;
            selected_.reset();
            count_ = 0;
            publish(terminal, revision, SearchStatus::running);
        }
        for (std::uint32_t unit = 0; unit < options_.max_work_units_per_step; ++unit) {
            if (work_ >= options_.max_total_work_units)
                return stop(terminal, revision, SearchStatus::limited,
                            "Search work budget reached; refine or restart the query.");
            ++work_;
            GhosttySearchStatus status{};
            checked(ghostty_search_get(handle_, GHOSTTY_SEARCH_DATA_STATUS, &status), "read search status");
            if (fed_revision_ != revision || status == GHOSTTY_SEARCH_STATUS_FEED_REQUIRED) {
                checked(ghostty_search_feed(handle_), "feed search");
                fed_revision_ = revision;
            } else if (materializing_) {
                // Never retain GhosttySelection or GridRef across this actor call.
                // The whole getter is bounded by max_matches; only 64 results are
                // converted per unit, and any later mutation restarts conversion.
                std::vector<GhosttySelection> selections(count_);
                GhosttySelectionBuffer buffer{selections.data(), selections.size(), 0};
                checked(ghostty_search_get(handle_, GHOSTTY_SEARCH_DATA_MATCHES, &buffer), "read bounded search matches");
                if (buffer.len != count_)
                    return stop(terminal, revision, SearchStatus::error,
                                "Search changed while materializing owned results.");
                const auto end = std::min(count_, building_.size() + 64);
                for (auto index = building_.size(); index < end; ++index) {
                    GhosttyPointCoordinate start{}, finish{};
                    if (ghostty_terminal_point_from_grid_ref(terminal, &selections[index].start, GHOSTTY_POINT_TAG_SCREEN, &start) != GHOSTTY_SUCCESS ||
                        ghostty_terminal_point_from_grid_ref(terminal, &selections[index].end, GHOSTTY_POINT_TAG_SCREEN, &finish) != GHOSTTY_SUCCESS)
                        return stop(terminal, revision, SearchStatus::error,
                                    "Search result was evicted; restart the query.");
                    building_.push_back({{start.x, start.y}, {finish.x, finish.y}});
                }
                if (building_.size() == count_) {
                    materializing_ = false;
                    return publish(terminal, revision, SearchStatus::complete, {}, std::move(building_));
                }
            } else if (status == GHOSTTY_SEARCH_STATUS_RUNNING) {
                checked(ghostty_search_tick(handle_, &status), "tick search");
            }
            if (!allocation_ok(terminal, revision)) return result_;
            checked(ghostty_search_get(handle_, GHOSTTY_SEARCH_DATA_TOTAL_MATCHES, &count_), "count search matches");
            if (count_ > options_.max_matches)
                return stop(terminal, revision, SearchStatus::limited,
                            "Too many matches; refine the query or raise the result limit.");
            checked(ghostty_search_get(handle_, GHOSTTY_SEARCH_DATA_STATUS, &status), "read search progress");
            if (status == GHOSTTY_SEARCH_STATUS_COMPLETE && !materializing_) {
                materializing_ = true;
                building_.clear();
                building_.reserve(count_);
            }
        }
        return publish(terminal, revision, SearchStatus::running);
    } catch (const std::bad_alloc&) {
        // Owned-result allocation failure is not a successful empty search.
        return stop(terminal, revision, SearchStatus::error,
                    "Could not allocate owned search results.");
    } catch (const std::exception& error) {
        return stop(terminal, revision, SearchStatus::error, error.what());
    }
}

TerminalSearch::SelectionChoice TerminalSearch::choose(
    SearchToken token, std::uint64_t revision, std::size_t index) const {
    if (token != result_->token || token.revision != revision)
        return {SearchApplyResult::stale};
    if (result_->status != SearchStatus::complete)
        return {SearchApplyResult::not_ready};
    if (index >= result_->matches.size())
        return {SearchApplyResult::no_match};
    return {SearchApplyResult::applied, index, result_->matches[index]};
}

TerminalSearch::SelectionChoice TerminalSearch::choose(
    SearchToken token, std::uint64_t revision, SearchDirection direction, bool wrap) const {
    if (token != result_->token || token.revision != revision)
        return {SearchApplyResult::stale};
    if (result_->status != SearchStatus::complete)
        return {SearchApplyResult::not_ready};
    const auto count = result_->matches.size();
    if (!count)
        return {SearchApplyResult::no_match};

    std::size_t index = direction == SearchDirection::backward ? 0 : count - 1;
    if (selected_) {
        index = *selected_;
        if (direction == SearchDirection::backward) {
            if (index + 1 < count) ++index;
            else if (wrap) index = 0;
            else return {SearchApplyResult::boundary};
        } else {
            if (index > 0) --index;
            else if (wrap) index = count - 1;
            else return {SearchApplyResult::boundary};
        }
    }
    return {SearchApplyResult::applied, index, result_->matches[index]};
}

SearchApplyResult TerminalSearch::record_action(
    SearchApplyResult result, std::optional<std::size_t> applied_index) {
    // Selection mutation has already succeeded. Preserve this state even if
    // allocating its immutable publication throws, as the Core did previously.
    if (applied_index) selected_ = applied_index;
    auto value = std::make_shared<SearchSnapshot>(*result_);
    value->update_id = ++update_;
    value->last_apply_result = result;
    value->selected_index = selected_;
    result_ = std::move(value);
    return result;
}

} // namespace stormtty
