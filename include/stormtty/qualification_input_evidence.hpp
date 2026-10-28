#pragma once
#ifndef STORMTTY_QUALIFICATION_INPUT_EVIDENCE
#error "Input evidence belongs only to the isolated synthetic-fixture qualification build"
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <ostream>
#include <string_view>
#include <vector>

namespace stormtty::qualification {
// No callbacks or I/O on the writer. Fixed storage; only an explicit test-driven
// snapshot allocates. Own with shared_ptr through ProcessOptions for worker life.
// Records describe completed Windows WriteFile calls, NOT shell consumption.
class InputEvidence final {
public:
    static constexpr std::size_t record_capacity = 256;
    static constexpr std::size_t bytes_per_record = 1024;
    struct Record {
        std::uint64_t sequence{}, message{}, offset{};
        std::uint32_t requested{}, reported{}, accepted{}, error{};
        bool success{};
        std::size_t stored{};
        std::array<unsigned char, bytes_per_record> bytes{};
    };
    struct Snapshot {
        std::uint64_t completed{}, overwritten{}, byte_truncated_records{}, after{}, missing_since_after{};
        std::vector<Record> records;
    };
    // Called once immediately after each WriteFile on the single writer worker.
    // On failure reported is diagnostic only: it is NOT counted as accepted.
    void completed_write(std::uint64_t message, std::uint64_t offset,
                         std::string_view requested, bool success,
                         std::uint32_t reported, std::uint32_t error) {
        std::lock_guard lock(mutex_);
        auto& r = records_[next_];
        r.sequence = ++completed_; r.message = message; r.offset = offset;
        r.requested = static_cast<std::uint32_t>(requested.size());
        r.reported = reported; r.success = success; r.error = error;
        r.accepted = success ? reported : 0;
        r.stored = std::min(requested.size(), bytes_per_record);
        std::copy_n(requested.begin(), r.stored, r.bytes.begin());
        if (r.stored < requested.size()) ++byte_truncated_records_;
        next_ = (next_ + 1) % record_capacity;
        if (size_ < record_capacity) ++size_; else ++overwritten_;
    }
    Snapshot snapshot(std::uint64_t after = 0) const {
        std::lock_guard lock(mutex_);
        Snapshot result{completed_, overwritten_, byte_truncated_records_, after, 0, {}};
        result.records.reserve(size_);
        const auto first = (next_ + record_capacity - size_) % record_capacity;
        if (size_ && records_[first].sequence > after + 1)
            result.missing_since_after = records_[first].sequence - after - 1;
        for (std::size_t i = 0; i < size_; ++i) {
            const auto& record = records_[(first + i) % record_capacity];
            if (record.sequence > after) result.records.push_back(record);
        }
        return result;
    }
    static void dump(std::ostream& out, const Snapshot& snapshot, std::string_view label) {
        out << "PTY_INPUT_EVIDENCE label=" << label << " completed=" << snapshot.completed
            << " overwritten=" << snapshot.overwritten << " byte_truncated_records="
            << snapshot.byte_truncated_records << " after=" << snapshot.after << " missing_since_after=" << snapshot.missing_since_after
            << " retained=" << snapshot.records.size() << '\n';
        constexpr char hex[] = "0123456789abcdef";
        for (const auto& r : snapshot.records) {
            out << "PTY_WRITE seq=" << r.sequence << " message=" << r.message << " offset=" << r.offset
                << " requested=" << r.requested << " success=" << r.success << " reported=" << r.reported
                << " accepted=" << r.accepted << " error=" << r.error
                << " requested_hex_truncated=" << (r.stored < r.requested)
                << " accepted_hex_truncated=" << (r.stored < r.accepted) << " requested_hex=";
            for (std::size_t i = 0; i < r.stored; ++i) out << hex[r.bytes[i] >> 4] << hex[r.bytes[i] & 15];
            out << " accepted_hex=";
            for (std::size_t i = 0; i < std::min<std::size_t>(r.stored, r.accepted); ++i)
                out << hex[r.bytes[i] >> 4] << hex[r.bytes[i] & 15];
            out << '\n';
        }
        out << "END_PTY_INPUT_EVIDENCE\n";
    }
private:
    mutable std::mutex mutex_;
    std::array<Record, record_capacity> records_{};
    std::size_t next_{}, size_{};
    std::uint64_t completed_{}, overwritten_{}, byte_truncated_records_{};
};
} // namespace stormtty::qualification
