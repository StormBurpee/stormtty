#include "core_test.hpp"
#include "stormtty/workspace.hpp"
#include <algorithm>
#include <limits>
#include <random>
#include <set>

using namespace stormtty;
namespace {
void basic_model() {
    Workspace w; CHECK(w.validate()); CHECK(!w.active_pane()); CHECK(w.state().tabs.empty());
    CHECK(!w.close_pane({99})); CHECK(!w.toggle_zoom());
    auto tab = w.create_tab({11}); CHECK(tab); const auto t = *tab.value, saved_t = t;
    const auto a = w.active_pane(); CHECK(w.pane(a)->session == SessionId{11});
    auto split = w.split(a, Direction::right, {12}, "default", 0.25); CHECK(split); const auto b = *split.value;
    CHECK(w.state().panes.size() == 2); CHECK(w.state().nodes.size() == 3); CHECK(w.active_pane() == b);
    CHECK(w.pane_order(t) == std::vector<PaneId>({a, b}));
    const auto root = w.tab(t)->root; CHECK(near(w.node(root)->ratio, 0.75));
    auto l = w.layout(t, {0, 0, 801, 400}); CHECK(l); CHECK(!l.constrained);
    CHECK(l.panes.size() == 2); CHECK(l.dividers.size() == 1);
    CHECK(near(l.panes[0].bounds.width, 600)); CHECK(near(l.panes[1].bounds.width, 200));
    CHECK(near(l.panes[1].bounds.x, 601)); CHECK(hit_test(l, {600.5, 200}).kind == HitKind::divider);
    CHECK(hit_test(l, {10, 10}).pane == a); CHECK(hit_test(l, {-1, 10}).kind == HitKind::none);
    CHECK(w.set_zoom(a, true)); CHECK(w.active_pane() == a); CHECK(w.layout(t, {0, 0, 801, 400}).panes.size() == 1);
    CHECK(!w.resize_split(root, 0.6, {0, 0, 801, 400}));
    CHECK(w.focus_direction(Direction::right, {0, 0, 801, 400})); CHECK(w.active_pane() == b);
    CHECK(w.tab(t)->zoomed_pane == b); CHECK(w.toggle_zoom());
    CHECK(w.rename_tab(t, "source review")); CHECK(w.set_pane_title(a, "终端 — work"));
    CHECK(!w.set_pane_title(a, "bad\x1b")); CHECK(!w.set_pane_title(a, std::string("\xc0\xaf", 2)));
    CHECK(w.close_pane(a)); CHECK(w.tab(t)->root != root); CHECK(w.state().nodes.size() == 1);
    CHECK(!w.node(root)); CHECK(w.active_pane() == b); CHECK(!w.tab(t)->zoomed_pane);
    CHECK(w.close_pane(b)); CHECK(w.state().tabs.empty()); CHECK(w.validate());
    auto fresh = w.create_tab(); CHECK(fresh); CHECK(*fresh.value != saved_t);
}
void transactional_and_invariants() {
    Workspace w; CHECK(w.create_tab({1})); auto a = w.active_pane(); CHECK(w.split(a, Direction::right, {2}));
    auto before = w.state(); const auto root = w.tab(w.active_tab())->root;
    CHECK(!w.create_tab({1})); CHECK(w.state() == before);
    CHECK(!w.split(a, Direction::down, {}, "default", 0)); CHECK(w.state() == before);
    CHECK(!w.split(a, static_cast<Direction>(999))); CHECK(w.state() == before);
    CHECK(!w.set_split_ratio(root, std::numeric_limits<double>::quiet_NaN())); CHECK(w.state() == before);
    CHECK(!w.set_split_ratio(root, 1.0)); CHECK(w.state() == before);
    CHECK(!w.move_pane(a, a, Direction::left)); CHECK(w.state() == before);
    CHECK(!w.move_pane(a, {900}, Direction::left)); CHECK(w.state() == before);
    CHECK(!w.reorder_tab(w.active_tab(), 1)); CHECK(w.state() == before);
    CHECK(!w.rename_tab(w.active_tab(), "a\nb")); CHECK(w.state() == before);
    CHECK(!w.bind_session(a, {2})); CHECK(w.state() == before);
    auto bad = before; bad.nodes.at(root).first = root; CHECK(!Workspace::from_state(bad));
    bad = before; bad.next_id = 1; CHECK(!Workspace::from_state(bad));
    bad = before; bad.tabs[0].active_pane = {10000}; CHECK(!Workspace::from_state(bad));
    bad = before; bad.tabs[0].zoomed_pane = a; CHECK(!Workspace::from_state(bad));
    bad = before; bad.nodes.emplace(NodeId{999}, Node{{999}, a, {}, {}, Axis::horizontal, 0.5}); bad.next_id = 1000; CHECK(!Workspace::from_state(bad));
    bad = before; bad.nodes.at(root).ratio = std::numeric_limits<double>::infinity(); CHECK(!Workspace::from_state(bad));
    bad = before; bad.tabs.push_back(bad.tabs[0]); CHECK(!Workspace::from_state(bad));
    bad = before; bad.panes.at(a).id = PaneId{42}; CHECK(!Workspace::from_state(bad));
    CHECK(w.state() == before);
    Workspace deep; CHECK(deep.create_tab()); bool reached = false;
    for (std::size_t i = 0; i < Workspace::maximum_depth + 2; ++i) {
        const auto old = deep.state(); auto result = deep.split(deep.active_pane(), Direction::right);
        if (!result) { CHECK(old == deep.state()); reached = true; break; }
    }
    CHECK(reached); CHECK(deep.validate());
}
void geometry() {
    Workspace w; CHECK(w.create_tab()); const auto a = w.active_pane();
    auto b = w.split(a, Direction::right); CHECK(b); const auto t = w.active_tab(); const auto root = w.tab(t)->root;
    CHECK(w.set_split_ratio(root, 0.12345678912345678)); const auto original = w.state();
    for (const auto scale : {1.0, 1.25, 1.5, 1.75, 2.0, 2.25}) {
        for (double width : {0.0, 1.0, 20.0, 160.0, 330.0, 800.0, 1023.25}) {
            LayoutMetrics m; m.divider_gap = 1 / scale;
            const auto l = w.layout(t, {-20.25, 5.125, width, 500}, m); CHECK(l); CHECK(l.panes.size() == 2);
            CHECK(near(l.panes[0].bounds.width + l.panes[1].bounds.width + l.dividers[0].bounds.width, width));
            CHECK(l.panes[0].bounds.width >= 0); CHECK(l.panes[1].bounds.width >= 0);
            CHECK(near(l.panes[1].bounds.x + l.panes[1].bounds.width, -20.25 + width)); CHECK(w.state() == original);
        }
    }
    CHECK(!w.layout(t, {0, 0, -1, 400})); CHECK(!w.layout(t, {0, 0, std::numeric_limits<double>::quiet_NaN(), 400}));
    CHECK(!w.resize_split(root, 0.5, {0, 0, 100, 200})); CHECK(w.state() == original);
    CHECK(w.resize_split(root, 0.001, {0, 0, 801, 400})); CHECK(near(w.node(root)->ratio, 0.1));
    CHECK(w.resize_split(root, 0.999, {0, 0, 801, 400})); CHECK(near(w.node(root)->ratio, 0.9));
    CHECK(w.resize_split(root, -1.0, {0, 0, 801, 400})); CHECK(near(w.node(root)->ratio, 0.1));
    CHECK(w.resize_split(root, 2.0, {0, 0, 801, 400})); CHECK(near(w.node(root)->ratio, 0.9));
    CHECK(!w.resize_split(root, std::numeric_limits<double>::quiet_NaN(), {0, 0, 801, 400}));
    CHECK(w.equalize_split(root)); CHECK(w.node(root)->ratio == 0.5);
    auto c = w.split(a, Direction::down); CHECK(c); auto d = w.split(*b.value, Direction::down); CHECK(d);
    CHECK(w.focus(a)); CHECK(w.focus_direction(Direction::right, {0, 0, 801, 601})); CHECK(w.active_pane() == *b.value);
    CHECK(w.focus_direction(Direction::down, {0, 0, 801, 601})); CHECK(w.active_pane() == *d.value);
    CHECK(w.focus_direction(Direction::left, {0, 0, 801, 601})); CHECK(w.active_pane() == *c.value);
    CHECK(w.focus_direction(Direction::up, {0, 0, 801, 601})); CHECK(w.active_pane() == a);
    CHECK(!w.focus_direction(Direction::left, {0, 0, 801, 601}));
}
void move_and_restore() {
    Workspace w; auto t1 = w.create_tab({101}); CHECK(t1); auto a = w.active_pane();
    auto b = w.split(a, Direction::right, {102}); CHECK(b);
    auto t2 = w.create_tab({103}); CHECK(t2); auto c = w.active_pane();
    CHECK(w.rename_tab(*t1.value, "A \\\"quoted\\\" title"));
    CHECK(w.move_pane(*b.value, c, Direction::down, 0.4)); CHECK(w.pane(*b.value)->session == SessionId{102});
    CHECK(w.tab_of(*b.value) == t2.value); CHECK(w.tab_of(a) == t1.value); CHECK(w.pane_order(*t1.value).size() == 1);
    CHECK(w.swap_panes(a, c)); CHECK(w.tab_of(a) == t2.value); CHECK(w.tab_of(c) == t1.value);
    CHECK(w.pane(a)->session == SessionId{101}); CHECK(w.pane(c)->session == SessionId{103});
    CHECK(w.move_pane(c, a, Direction::left)); CHECK(w.state().tabs.size() == 1); CHECK(!w.tab(*t1.value));
    auto detached = w.move_pane_to_new_tab(a, "detached"); CHECK(detached); CHECK(w.pane(a)->session == SessionId{101});
    CHECK(w.reorder_tab(*detached.value, 0)); CHECK(w.state().tabs.front().id == *detached.value);
    CHECK(w.set_zoom(*b.value, true));
    const auto serialized = serialize_workspace(w); auto restored = deserialize_workspace(serialized); CHECK(restored);
    auto expected = w.state(); for (auto& [id, pane] : expected.panes) { (void)id; pane.session = {}; }
    CHECK(restored.value->state() == expected); CHECK(serialize_workspace(*restored.value) == serialized);
    CHECK(!deserialize_workspace(serialized + "junk\n"));
    CHECK(!deserialize_workspace("STORMTTY-WORKSPACE 1\n\x1b[31m")); CHECK(!deserialize_workspace("STORMTTY-WORKSPACE 99\n"));
    CHECK(!deserialize_workspace("STORMTTY-WORKSPACE 1\nnext 1\nactive 0\n"));
    CHECK(!deserialize_workspace("STORMTTY-WORKSPACE 1\nnext 1\nnext 2\nactive 0\nend\n"));
    CHECK(!deserialize_workspace(std::string(4 * 1024 * 1024 + 1, 'x')));
    CHECK(w.close_tab(*t2.value)); CHECK(w.state().tabs.size() == 1); CHECK(w.close_tab(*detached.value)); CHECK(w.validate());
}
void serialization_mutations() {
    Workspace original; CHECK(original.create_tab({444}));
    for (unsigned i = 0; i < 6; ++i) CHECK(original.split(original.active_pane(), static_cast<Direction>(i % 4), {445 + i}));
    const auto seed = serialize_workspace(original); std::mt19937 rng(0x5E21A1);
    for (unsigned iteration = 0; iteration < 2500; ++iteration) {
        std::string bytes = seed;
        for (unsigned n = 0, edits = rng() % 5 + 1; n < edits; ++n) {
            const auto pos = rng() % (bytes.size() + 1);
            if (rng() % 3 == 0) bytes.insert(pos, 1, static_cast<char>(rng() % 256));
            else if (pos < bytes.size() && rng() % 2) bytes.erase(pos, 1);
            else if (pos < bytes.size()) bytes[pos] = static_cast<char>(rng() % 256);
        }
        auto parsed = deserialize_workspace(bytes);
        if (parsed) {
            CHECK(parsed.value->validate());
            for (const auto& [id, p] : parsed.value->state().panes) { (void)id; CHECK(!p.session); }
            auto again = deserialize_workspace(serialize_workspace(*parsed.value)); CHECK(again); CHECK(parsed.value->state() == again.value->state());
        } else CHECK(!parsed.error.empty());
    }
}
void operation_trace() {
    std::mt19937 rng(0x570A17); Workspace w; std::uint64_t session = 1;
    for (unsigned step = 0; step < 5000; ++step) {
        if (w.state().tabs.empty()) CHECK(w.create_tab({session++}));
        std::vector<PaneId> panes; for (auto& [id, p] : w.state().panes) { (void)p; panes.push_back(id); }
        const auto a = panes[rng() % panes.size()], b = panes[rng() % panes.size()];
        const auto tab = w.state().tabs[rng() % w.state().tabs.size()].id;
        const auto before = w.state(); bool success = false;
        const auto direction = static_cast<Direction>(rng() % 4);
        switch (rng() % 15) {
        case 0: if (panes.size() < 40) success = static_cast<bool>(w.create_tab({session++})); break;
        case 1: if (panes.size() < 40) success = static_cast<bool>(w.split(a, direction, {session++}, "default", 0.37)); break;
        case 2: success = static_cast<bool>(w.close_pane(a)); break;
        case 3: success = static_cast<bool>(w.move_pane(a, b, direction, 0.63)); break;
        case 4: success = static_cast<bool>(w.swap_panes(a, b)); break;
        case 5: success = static_cast<bool>(w.move_pane_to_new_tab(a, "trace")); break;
        case 6: success = static_cast<bool>(w.focus(a)); break;
        case 7: success = static_cast<bool>(w.set_zoom(a, (rng() & 1) != 0)); break;
        case 8: success = static_cast<bool>(w.reorder_tab(tab, rng() % w.state().tabs.size())); break;
        case 9: success = static_cast<bool>(w.close_tab(tab)); break;
        case 10: success = static_cast<bool>(w.rename_tab(tab, "step " + std::to_string(step))); break;
        case 11: success = static_cast<bool>(w.focus_direction(direction, {0, 0, 1200.5, 700.25})); break;
        case 12: success = static_cast<bool>(w.set_split_ratio(w.tab(tab)->root, 0.27)); break;
        case 13: success = static_cast<bool>(w.resize_split(w.tab(tab)->root, 0.81, {0, 0, 1200.5, 700.25})); break;
        default: success = static_cast<bool>(w.close_pane({999999})); break;
        }
        if (!success) CHECK(w.state() == before);
        CHECK(w.validate()); std::set<PaneId> all; std::set<SessionId> sessions;
        for (const auto& t : w.state().tabs) {
            for (auto id : w.pane_order(t.id)) CHECK(all.insert(id).second);
            const auto layout = w.layout(t.id, {-100.25, 0, 999.3, 654.7}); CHECK(layout);
            for (const auto& placement : layout.panes) {
                CHECK(placement.bounds.width >= 0 && placement.bounds.height >= 0);
                CHECK(placement.bounds.x >= -100.25 - 1e-8 && placement.bounds.x + placement.bounds.width <= 899.05 + 1e-8);
            }
        }
        CHECK(all.size() == w.state().panes.size());
        for (const auto& [id, p] : w.state().panes) {
            CHECK(sessions.insert(p.session).second);
            auto old = before.panes.find(id); if (old != before.panes.end()) CHECK(old->second.session == p.session);
        }
        if (step % 29 == 0) { auto restored = deserialize_workspace(serialize_workspace(w)); CHECK(restored); CHECK(restored.value->validate()); }
    }
}
}
int main() { return run_core_tests("workspace", [] { basic_model(); transactional_and_invariants(); geometry(); move_and_restore(); serialization_mutations(); operation_trace(); }); }
