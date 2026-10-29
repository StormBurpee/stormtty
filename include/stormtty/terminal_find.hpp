#pragma once
#include "stormtty/terminal.hpp"
#include "stormtty/workspace.hpp"
#include <algorithm>
#include <string>
#include <string_view>

namespace stormtty {
inline bool terminal_find_fits(Rect pane, bool split) {
    return pane.width>=180 && pane.height>=(split?140:112);
}
struct TerminalFindLayout { Rect strip, query, status, previous, next, close; };
inline TerminalFindLayout layout_terminal_find(Rect pane, double header=0) {
    const double w=std::max(0.0,pane.width), h=std::max(0.0,pane.height-header);
    const bool compact=w<440;
    const double height=std::min(h,compact?72.0:42.0), pad=std::min(8.0,w/2);
    TerminalFindLayout out; out.strip={pane.x,pane.y+header,w,height};
    const double button=std::min(28.0,std::max(0.0,(w-2*pad)/3));
    const double controls=3*button, y=out.strip.y+(compact?38:7);
    out.close={pane.x+w-pad-button,y,button,std::min(28.0,std::max(0.0,height-(compact?38:7)))};
    out.next={out.close.x-button,y,button,out.close.height}; out.previous={out.next.x-button,y,button,out.close.height};
    out.query={pane.x+pad,out.strip.y+7,std::max(0.0,w-2*pad-(compact?0:controls+108)),std::min(28.0,std::max(0.0,height-7))};
    out.status={compact?pane.x+pad:out.query.x+out.query.width+8,y,compact?std::max(0.0,w-2*pad-controls):92,out.close.height};
    return out;
}
// Query editing is UTF-8 scalar aware, bounded, and single-line. No regex or
// Unicode case-fold controls are offered: those are not backend capabilities.
class TerminalFindQuery {
public:
    static constexpr std::size_t limit=4096;
    const std::string& text() const { return text_; }
    std::size_t caret() const { return caret_; }
    bool selected() const { return all_; }
    void select_all() { all_=!text_.empty(); caret_=text_.size(); }
    bool insert(std::string_view text) {
        if(text.empty() || text.size()+(all_?0:text_.size())>limit) return false;
        for(std::size_t i=0;i<text.size();) {
            const auto c=static_cast<unsigned char>(text[i]);
            if(c<32 || c==127) return false;
            unsigned count=c<0x80?1:c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:0;
            if(!count || i+count>text.size()) return false;
            for(unsigned j=1;j<count;++j) if((static_cast<unsigned char>(text[i+j])&0xc0)!=0x80)return false;
            if(count==3 && ((c==0xe0 && static_cast<unsigned char>(text[i+1])<0xa0)||(c==0xed && static_cast<unsigned char>(text[i+1])>=0xa0)))return false;
            if(count==4 && ((c==0xf0 && static_cast<unsigned char>(text[i+1])<0x90)||(c==0xf4 && static_cast<unsigned char>(text[i+1])>=0x90)))return false;
            i+=count;
        }
        if(all_) {text_.clear();caret_=0;all_=false;}
        text_.insert(caret_,text);caret_+=text.size();return true;
    }
    bool erase(bool backward) {
        if(all_) {text_.clear();caret_=0;all_=false;return true;}
        if(backward && caret_) {const auto old=caret_;left();text_.erase(caret_,old-caret_);return true;}
        if(!backward && caret_<text_.size()) {const auto old=caret_;right();const auto end=caret_;caret_=old;text_.erase(old,end-old);return true;}
        return false;
    }
    void left() {all_=false;if(caret_){--caret_;while(caret_ && (static_cast<unsigned char>(text_[caret_])&0xc0)==0x80)--caret_;}}
    void right() {all_=false;if(caret_<text_.size()){++caret_;while(caret_<text_.size() && (static_cast<unsigned char>(text_[caret_])&0xc0)==0x80)++caret_;}}
    void home() {all_=false;caret_=0;} void end(){all_=false;caret_=text_.size();}
private:
    std::string text_; std::size_t caret_{}; bool all_{};
};
// Native measurement callback keeps the caret in view without byte-width
// estimates or splitting a UTF-8 scalar. Logarithmic measurements, bounded text.
template<class Measure>
std::pair<std::string,double> terminal_find_visible(const TerminalFindQuery& query, double width, Measure measure) {
    const auto& text=query.text();const auto caret=query.caret();
    std::vector<std::size_t> boundaries{0};
    for(std::size_t i=1;i<=caret;++i)if(i==text.size() || (static_cast<unsigned char>(text[i])&0xc0)!=0x80)boundaries.push_back(i);
    std::size_t low=0,high=boundaries.size()-1;
    while(low<high){const auto mid=(low+high)/2;if(measure(std::string_view(text).substr(boundaries[mid],caret-boundaries[mid]))>std::max(0.0,width))low=mid+1;else high=mid;}
    const auto start=boundaries[low];
    return {text.substr(start),measure(std::string_view(text).substr(start,caret-start))};
}
inline bool current_find_result(const SearchSnapshot& result, std::uint64_t request, std::string_view query) {
    return request && result.token.request_id==request && result.query==query;
}
inline bool initial_find_selection_needed(const SearchSnapshot& result, std::uint64_t request, const std::optional<SearchToken>& attempted) {
    return result.token.request_id==request && result.status==SearchStatus::complete && !result.matches.empty() &&
        !result.selected_index && (!attempted || *attempted!=result.token);
}
// Initial query intent survives transient empty/repaint and accepted-then-
// invalidated results. Only user navigation, replacement or dismissal ends it.
// Every result token is submitted at most once; no timer or polling retry.
class TerminalFindInitialSelection {
public:
    void begin(std::uint64_t request) { request_=request;attempted_.reset(); }
    void cancel() { begin(0); }
    std::uint64_t request() const { return request_; }
    const std::optional<SearchToken>& attempted() const { return attempted_; }
    std::optional<SearchToken> take(const SearchSnapshot& result) {
        if(!request_ || !initial_find_selection_needed(result,request_,attempted_))return {};
        attempted_=result.token;return attempted_;
    }
private:
    std::uint64_t request_{};
    std::optional<SearchToken> attempted_;
};
inline std::string terminal_find_status(const SearchSnapshot* result, std::uint64_t request, std::string_view query) {
    if(query.empty())return "";
    if(!result || !current_find_result(*result,request,query))return "Searching…";
    switch(result->status) {
    case SearchStatus::complete:
        if(result->matches.empty())return "No matches";
        return (result->selected_index && *result->selected_index<result->matches.size()?std::to_string(result->matches.size()-*result->selected_index):"–")+" / "+std::to_string(result->matches.size());
    case SearchStatus::limited:return "Limit reached";
    case SearchStatus::error:return "Search failed";
    case SearchStatus::cancelled:return "Cancelled";
    default:return "Searching…";
    }
}
} // namespace stormtty
