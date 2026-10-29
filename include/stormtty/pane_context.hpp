#pragma once
#include "stormtty/agent_display_text.hpp"
#include "stormtty/shell_context.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace stormtty {
enum class PaneDirectorySource { unavailable, observed, configured };
struct PaneContextInput {
    // Existing host-resolved title, including user/application/OSC titles. This
    // helper never guesses provenance or replaces a nonempty title with cwd.
    std::string_view title, profile_label, observed_directory, configured_directory;
    const ShellContext* shell_context{}; // Borrowed only during this call.
    bool integrated_shell{}, retained_exited{};
};
struct PaneContextPresentation {
    std::string primary, directory, head, repository_root, configured_directory;
    std::string detail; // Complete, read-only display record; never action authority.
    PaneDirectorySource directory_source{PaneDirectorySource::unavailable};
    bool detached{}, retained_exited{};
    bool operator==(const PaneContextPresentation&) const = default;
};
// Same visible control/bidi/invalid UTF-8 policy as other untrusted UI text;
// additionally escape LF because every input field is single-line chrome.
inline std::string pane_context_text(std::string_view raw) {
    std::string out;
    std::size_t offset=0;
    while(offset<raw.size()) {
        auto token=agent_display_detail::next(raw,offset);
        out+=token.newline?"[U+000A]":token.text;
    }
    return out;
}
inline PaneContextPresentation make_pane_context(const PaneContextInput& input) {
    PaneContextPresentation out;
    out.primary=pane_context_text(!input.title.empty()?input.title:
        !input.profile_label.empty()?input.profile_label:std::string_view{"Terminal"});
    out.configured_directory=pane_context_text(input.configured_directory);
    out.retained_exited=input.retained_exited;
    if(input.shell_context) {
        out.directory=pane_context_text(input.shell_context->display_directory);
        out.repository_root=pane_context_text(input.shell_context->repository_root);
        if(input.shell_context->git!=GitHeadKind::none) out.head=pane_context_text(input.shell_context->head);
        out.detached=input.shell_context->git==GitHeadKind::detached;
        if(!out.directory.empty()) out.directory_source=PaneDirectorySource::observed;
    } else if(!input.integrated_shell) {
        // Integrated-shell clears must not revive an older raw PWD snapshot.
        if(!input.observed_directory.empty()) {
            out.directory=pane_context_text(input.observed_directory);
            out.directory_source=PaneDirectorySource::observed;
        } else if(!out.configured_directory.empty()) {
            out.directory=out.configured_directory;
            out.directory_source=PaneDirectorySource::configured;
        }
    }
    out.detail="Title: "+out.primary;
    const auto profile=pane_context_text(input.profile_label);
    if(!profile.empty()) out.detail+="\nProfile: "+profile;
    out.detail+="\nObserved directory: "+(out.directory_source==PaneDirectorySource::observed?out.directory:"Unavailable");
    if(!out.configured_directory.empty()) out.detail+="\nConfigured start folder: "+out.configured_directory;
    if(!out.repository_root.empty()) out.detail+="\nRepository: "+out.repository_root;
    if(!out.head.empty()) out.detail+=std::string(out.detached?"\nDetached HEAD: ":"\nBranch: ")+out.head;
    if(out.retained_exited) out.detail+="\nState: Exited (retained output)";
    return out;
}
// Native hosts choose density using measured available space, not UTF-8 byte
// counts: drop HEAD first, then secondary entirely, then ellipsize primary.
// Keep the entire directory (not only basename) to distinguish sibling roots.
// No time input/state: context changes replace text directly, never animate it.
enum class PaneContextDensity { title_only, directory, directory_and_head };
inline std::string pane_context_secondary(const PaneContextPresentation& value,
        PaneContextDensity density=PaneContextDensity::directory_and_head) {
    if(density==PaneContextDensity::title_only) return {};
    std::string out=value.directory;
    if(value.directory_source==PaneDirectorySource::configured&&!out.empty()) out="Start: "+out;
    if(density==PaneContextDensity::directory_and_head&&!value.head.empty()) {
        if(!out.empty()) out+=" · ";
        out+=(value.detached?"HEAD ":"")+value.head;
    }
    return out;
}
// Native hosts provide their single-layout, shaping-aware line wrapper. Cache
// by full text and geometry: filtering, paging and repaints create no layouts.
// No byte slicing, scalar-width approximation or grapheme segmentation here.
class PaneContextLineCache {
public:
    template<class Wrap>
    const std::vector<std::string>& lines(std::string_view text,double width,double scale,Wrap wrap) {
        if(!valid_ || text_!=text || width_!=width || scale_!=scale) {
            auto next=wrap(text,width,scale);
            text_=text;width_=width;scale_=scale;lines_=std::move(next);valid_=true;
        }
        return lines_;
    }
private:
    std::string text_;
    double width_{},scale_{};
    bool valid_{};
    std::vector<std::string> lines_;
};

}
