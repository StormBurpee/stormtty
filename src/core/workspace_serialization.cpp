#include "stormtty/workspace.hpp"
#include "text.hpp"

#include <iomanip>
#include <locale>
#include <sstream>

namespace stormtty {
namespace {
bool read_quoted(std::istream& input, std::string& output) {
    output.clear();
    input >> std::ws;
    char ch{};
    if (!input.get(ch) || ch != '"') return false;
    while (input.get(ch)) {
        if (ch == '"') return true;
        if (ch == '\\') {
            if (!input.get(ch) || (ch != '\\' && ch != '"')) return false;
        }
        output += ch;
    }
    return false;
}
}
std::string serialize_workspace(const Workspace& workspace) {
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "STORMTTY-WORKSPACE 1\nnext " << workspace.state().next_id << "\nactive " << workspace.active_tab().value << '\n';
    for (const auto& t : workspace.state().tabs)
        out << "tab " << t.id.value << ' ' << t.root.value << ' ' << t.active_pane.value << ' '
            << (t.zoomed_pane ? t.zoomed_pane->value : 0) << ' ' << std::quoted(t.title) << '\n';
    for (const auto& [id, p] : workspace.state().panes)
        out << "pane " << id.value << ' ' << std::quoted(p.profile) << ' ' << std::quoted(p.title) << '\n';
    out << std::setprecision(17);
    for (const auto& [id, n] : workspace.state().nodes) {
        if (n.is_leaf()) out << "leaf " << id.value << ' ' << n.pane.value << '\n';
        else out << "split " << id.value << ' ' << (n.axis == Axis::horizontal ? 'H' : 'V') << ' '
                 << n.ratio << ' ' << n.first.value << ' ' << n.second.value << '\n';
    }
    out << "end\n"; return out.str();
}
Result<Workspace> deserialize_workspace(std::string_view text) {
    if (text.size() > 4 * 1024 * 1024) return {{}, "Workspace exceeds the 4 MiB limit"};
    if (!detail::valid_document_text(text)) return {{}, "Workspace contains invalid UTF-8 or control characters"};
    std::istringstream input{std::string(text)}; input.imbue(std::locale::classic());
    std::string line; std::size_t line_number{};
    auto fail = [&](std::string message) -> Result<Workspace> {
        return {{}, "Workspace line " + std::to_string(line_number) + ": " + message};
    };
    if (!std::getline(input, line)) return fail("Missing version header");
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != "STORMTTY-WORKSPACE 1") return fail("Unsupported workspace format/version");
    WorkspaceState state; bool have_next{}, have_active{}, have_end{};
    while (std::getline(input, line)) {
        ++line_number;
        if (line.size() > 16384) return fail("Record exceeds the 16 KiB limit");
        std::istringstream row(line); row.imbue(std::locale::classic()); std::string kind;
        if (!(row >> kind)) continue;
        if (have_end) return fail("Unexpected record after end");
        if (kind == "next") {
            if (have_next || !(row >> state.next_id)) return fail("Invalid or duplicate next ID");
            have_next = true;
        } else if (kind == "active") {
            if (have_active || !(row >> state.active_tab.value)) return fail("Invalid or duplicate active tab");
            have_active = true;
        } else if (kind == "tab") {
            Tab t; std::uint64_t zoom{};
            if (!(row >> t.id.value >> t.root.value >> t.active_pane.value >> zoom) || !read_quoted(row, t.title)) return fail("Invalid tab record");
            if (zoom) t.zoomed_pane = PaneId{zoom};
            if (state.tabs.size() >= Workspace::maximum_tabs) return fail("Too many tabs");
            state.tabs.push_back(std::move(t));
        } else if (kind == "pane") {
            Pane p;
            if (!(row >> p.id.value) || !read_quoted(row, p.profile) || !read_quoted(row, p.title)) return fail("Invalid pane record");
            if (state.panes.size() >= Workspace::maximum_panes || !state.panes.emplace(p.id, p).second) return fail("Too many panes or duplicate pane ID");
        } else if (kind == "leaf" || kind == "split") {
            Node n;
            if (!(row >> n.id.value)) return fail("Invalid node ID");
            if (kind == "leaf") { if (!(row >> n.pane.value)) return fail("Invalid leaf record"); }
            else {
                char axis{};
                if (!(row >> axis >> n.ratio >> n.first.value >> n.second.value) || (axis != 'H' && axis != 'V')) return fail("Invalid split record");
                n.axis = axis == 'H' ? Axis::horizontal : Axis::vertical;
            }
            if (state.nodes.size() >= Workspace::maximum_panes * 2 || !state.nodes.emplace(n.id, n).second) return fail("Too many nodes or duplicate node ID");
        } else if (kind == "end") have_end = true;
        else return fail("Unknown record type: " + kind);
        row >> std::ws;
        if (!row.eof()) return fail("Unexpected trailing fields");
    }
    if (!have_next || !have_active || !have_end) return fail("Missing required metadata or end marker");
    return Workspace::from_state(std::move(state));
}
} // namespace stormtty
