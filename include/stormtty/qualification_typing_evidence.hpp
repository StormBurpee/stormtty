#pragma once
#ifndef STORMTTY_QUALIFICATION_INPUT_EVIDENCE
#error "Typing evidence is restricted to isolated synthetic qualification builds"
#endif
#include "stormtty/frame.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <ostream>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace stormtty::qualification {
// Fixed storage, no observer callbacks/file I/O on transport or actor threads.
// Overflow never alters transport: evidence becomes explicitly inconclusive.
class TypingEvidence final {
public:
    static constexpr std::size_t record_capacity=2048, byte_capacity=1024*1024;
    enum class Kind { read, write, enqueue, feed, command, publication, marker, prelude_feed, wake };
    enum class Phase { prelude, typing, frozen };
    struct Record {
        Kind kind{};
        std::uint64_t ordinal{}, us{}, sequence{}, offset{}, size{}, auxiliary{}, next{}, next_command{}, queued{}, generation{}, row_hash{};
        std::uint32_t row{}, column{}, reported{}, error{};
        bool success{}, visible{}, alternate{};
        std::size_t bytes_start{}, bytes_size{};
    };
    struct Snapshot { bool overflow{}; Phase phase{}; std::vector<Record> records; std::vector<unsigned char> bytes; };
    static std::uint64_t fingerprint(const RenderFrame& f) {
        std::uint64_t h=14695981039346656037ULL;
        auto mix=[&](unsigned char c){ h=(h^c)*1099511628211ULL; };
        if (f.cursor.row<f.lines.size() && f.lines[f.cursor.row])
            for (const auto& c:f.lines[f.cursor.row]->cells) {
                for (const unsigned char b:c.grapheme) mix(b);
                mix(0); mix(c.span);
                for (const auto color:{c.foreground,c.background,c.underline_color}) {
                    mix(color.r); mix(color.g); mix(color.b); mix(color.a);
                }
                mix(c.style.bold); mix(c.style.italic); mix(c.style.faint); mix(c.style.blink);
                mix(c.style.inverse); mix(c.style.invisible); mix(c.style.strikethrough);
                mix(c.style.overline); mix(c.style.underline);
            }
        return h;
    }
    void read(std::string_view bytes) {
        std::lock_guard lock(mutex_);
        Record r; r.kind=Kind::read; r.offset=read_bytes_; r.size=bytes.size(); r.success=true;
        if (phase_==Phase::frozen) return;
        read_bytes_+=bytes.size();
        if (phase_==Phase::prelude && prelude_read_) {
            if(bytes.size()>byte_capacity-byte_count_) {overflow_=true;return;}
            for(const unsigned char b:bytes) bytes_[byte_count_++]=b;
            auto& first=records_[*prelude_read_];first.size+=bytes.size();first.bytes_size+=bytes.size();
        } else { const auto index=record_count_;append(r,bytes);if(phase_==Phase::prelude&&record_count_>index)prelude_read_=index; }
    }
    void write(std::uint64_t message,std::uint64_t offset,std::string_view bytes,bool success,std::uint32_t reported,std::uint32_t error) {
        std::lock_guard lock(mutex_);
        if(phase_!=Phase::typing)return;
        Record r; r.kind=Kind::write; r.sequence=message; r.offset=offset; r.size=bytes.size();
        r.success=success; r.reported=reported; r.error=error; append(r,bytes);
    }
    void event(Kind kind,std::uint64_t sequence,std::uint64_t offset,std::uint64_t size) {
        std::lock_guard lock(mutex_);
        if(phase_==Phase::frozen)return;
        if(phase_==Phase::prelude && kind!=Kind::command && kind!=Kind::feed)return;
        Record r; r.kind=kind; r.sequence=sequence; r.offset=offset; r.size=size;
        if(phase_==Phase::prelude && kind==Kind::feed) {
            r.kind=Kind::prelude_feed;
            if(record_count_ && records_[record_count_-1].kind==Kind::prelude_feed) {
                auto& last=records_[record_count_-1];
                if(last.offset+last.size!=offset||last.sequence>=sequence){overflow_=true;return;}
                last.sequence=sequence;last.size+=size;return;
            }
        }
        append(r,{});
    }
    // Called only by the Session actor, in the same global order as input/output.
    void phase(bool arm,std::uint64_t sequence,std::uint64_t fed) {
        std::lock_guard lock(mutex_);
        if((arm&&phase_!=Phase::prelude)||(!arm&&phase_!=Phase::typing)){overflow_=true;return;}
        Record r;r.kind=Kind::marker;r.sequence=sequence;r.offset=fed;r.size=arm?1:2;r.auxiliary=read_bytes_;
        append(r,{});phase_=arm?Phase::typing:Phase::frozen;
    }
    Phase phase() const {std::lock_guard lock(mutex_);return phase_;}
    void publication(const RenderFrame& f,std::uint64_t fed_sequence,std::uint64_t fed_bytes,std::uint64_t command_sequence,std::uint64_t next,std::size_t queued,std::uint64_t next_command=0) {
        std::lock_guard lock(mutex_);if(phase_!=Phase::typing)return;
        Record r; r.kind=Kind::publication; r.sequence=fed_sequence; r.offset=fed_bytes;
        r.auxiliary=command_sequence; r.next=next; r.queued=queued; r.generation=f.generation;
        r.next_command=next_command;
        r.row=f.cursor.row; r.column=f.cursor.column; r.visible=f.cursor.visible; r.alternate=f.alternate_screen;
        r.row_hash=fingerprint(f);
        append(r,{});
    }
    void wake(std::uint64_t sequence,std::chrono::steady_clock::time_point deadline,
              std::chrono::steady_clock::time_point begin,std::chrono::steady_clock::time_point returned,
              std::chrono::steady_clock::time_point reacquired,std::uint32_t reason,bool pending,bool high_resolution) {
        std::lock_guard lock(mutex_);if(phase_!=Phase::typing)return;
        auto us=[&](auto t){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t-start_).count());};
        Record r;r.kind=Kind::wake;r.sequence=sequence;r.offset=us(deadline);r.size=us(begin);
        r.auxiliary=us(returned);r.next=us(reacquired);r.reported=reason;r.success=pending;r.alternate=high_resolution;
        append(r,{});
    }
    Snapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return {overflow_,phase_,{records_.begin(),records_.begin()+record_count_},{bytes_.begin(),bytes_.begin()+byte_count_}};
    }
    static void dump(std::ostream& out,const Snapshot& s) {
        out<<"TYPING_TRACE version=2 overflow="<<s.overflow<<" phase="<<static_cast<unsigned>(s.phase)<<" records="<<s.records.size()<<" bytes="<<s.bytes.size()<<'\n';
        constexpr std::array names{"read","write","enqueue","feed","command","publication","marker","prelude_feed","wake"};
        constexpr char hex[]="0123456789abcdef";
        for (const auto& r:s.records) {
            out<<"TYPING_EVENT kind="<<names[static_cast<unsigned>(r.kind)]<<" ordinal="<<r.ordinal<<" us="<<r.us
                <<" sequence="<<r.sequence<<" offset="<<r.offset<<" size="<<r.size<<" auxiliary="<<r.auxiliary
                <<" next="<<r.next<<" next_command="<<r.next_command<<" queued="<<r.queued<<" generation="<<r.generation
                <<" row="<<r.row<<" column="<<r.column<<" row_hash="<<r.row_hash<<" visible="<<r.visible<<" alternate="<<r.alternate
                <<" success="<<r.success<<" reported="<<r.reported<<" error="<<r.error<<" hex=";
            for (std::size_t i=0;i<r.bytes_size;++i) { const auto b=s.bytes[r.bytes_start+i]; out<<hex[b>>4]<<hex[b&15]; }
            out<<'\n';
        }
        out<<"END_TYPING_TRACE\n";
    }
private:
    void append(Record r,std::string_view bytes) {
        if (record_count_==record_capacity || bytes.size()>byte_capacity-byte_count_) { overflow_=true; return; }
        r.ordinal=record_count_+1;
        r.us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start_).count();
        r.bytes_start=byte_count_; r.bytes_size=bytes.size();
        for (const unsigned char b:bytes) bytes_[byte_count_++]=b;
        records_[record_count_++]=r;
    }
    const std::chrono::steady_clock::time_point start_=std::chrono::steady_clock::now();
    mutable std::mutex mutex_;
    std::array<Record,record_capacity> records_{};
    std::array<unsigned char,byte_capacity> bytes_{};
    std::size_t record_count_{},byte_count_{};
    std::uint64_t read_bytes_{};
    bool overflow_{};
    Phase phase_{Phase::prelude};
    std::optional<std::size_t> prelude_read_;
};
}
