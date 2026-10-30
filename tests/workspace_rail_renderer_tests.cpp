#include "core_test.hpp"
#include "stormtty/workspace_rail_renderer.hpp"
#include "stormtty/chrome_paint.hpp"

#include <array>
#include <map>

using namespace stormtty;

namespace {

Color blend(Color a, Color b, double t) {
    auto channel = [&](int x, int y) { return static_cast<std::uint8_t>(std::lround(x + (y - x) * t)); };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), channel(a.a, b.a)};
}

// Independent copy of the pre-extraction host accent parser.
Color color_hex(std::string_view text, Color fallback) {
    if (text.size() != 7 || text[0] != '#') return fallback;
    unsigned value{};
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        const int n = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (n < 0) return fallback;
        value = value * 16 + static_cast<unsigned>(n);
    }
    return {static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value), 255};
}

struct Metadata { std::string name, icon, accent; };
struct Project { ProjectWorkspaceId id; Metadata metadata; };
struct Catalog {
    std::vector<Project> entries;
    ProjectWorkspaceId active;
    std::vector<std::string>* trace{};
    const Project* find(ProjectWorkspaceId id) const {
        if (trace) trace->push_back("find:" + std::to_string(id.value));
        const auto found = std::find_if(entries.begin(), entries.end(),
            [id](const auto& entry) { return entry.id == id; });
        return found == entries.end() ? nullptr : &*found;
    }
    ProjectWorkspaceId active_id() const {
        if (trace) trace->push_back("active");
        return active;
    }
};
struct Sample {
    double amount{};
    double value(int) const { return amount; }
};
struct Fixture {
    ProjectNavigationLayout navigation;
    Catalog projects;
    std::map<ProjectWorkspaceId, Sample> focus;
    std::map<std::uint64_t, double> hovers;
    std::map<ProjectWorkspaceId, bool> unread;
    double caption{32};
    bool backdrop{};
    Color contrast_background{7, 29, 53, 255};
    WorkspaceRailStyle style;
};

// Original source was committed before production movement. Only environmental
// lookups are supplied here; it never calls WorkspaceRailRenderer helpers.
void append_environment(WindowScene& result, Fixture fixture, bool owned,
                        std::vector<std::string>& trace) {
    fixture.projects.trace = &trace;
    auto& navigation_ = fixture.navigation;
    auto& projects_ = fixture.projects;
    struct FocusSamples {
        std::map<ProjectWorkspaceId, Sample>& values;
        std::vector<std::string>& trace;
        Sample& operator[](ProjectWorkspaceId id) {
            trace.push_back("focus:" + std::to_string(id.value));
            return values[id];
        }
    } workspace_focus_{fixture.focus, trace};
    const auto hover_amount = [&](std::uint64_t id) {
        trace.push_back("hover:" + std::to_string(id));
        return fixture.hovers[id];
    };
    const auto project_watch_unread = [&](ProjectWorkspaceId id) {
        trace.push_back("watch:" + std::to_string(id.value));
        return fixture.unread[id];
    };
    const struct { Rect band; } chrome_{{0, 0, 1000, fixture.caption}};
    const bool high_contrast_ = fixture.style.high_contrast;
    const bool backdrop_ = fixture.backdrop;
    const double scale_ = fixture.style.scale;
    const int now = 0;
    const auto background = fixture.contrast_background;
    const auto raised = fixture.style.raised;
    const auto seam = fixture.style.seam;
    const auto text = fixture.style.ink;
    const auto secondary = fixture.style.secondary;
    const auto accent = fixture.style.accent;
    const struct { Color navigation, selected; } ui{fixture.style.surface, fixture.style.selected_surface};
    const ChromeZOrder z;
    const auto pixels = [&](Rect r) {
        return PixelRect{static_cast<float>(r.x * scale_), static_cast<float>(r.y * scale_),
            static_cast<float>(r.width * scale_), static_cast<float>(r.height * scale_)};
    };
    auto rectangle = [&](Rect bounds, Color fill, double radius = 0, Color stroke = {}, double stroke_width = 0, bool replace = false) {
        result.primitives.push_back({pixels(bounds), fill, stroke, static_cast<float>(radius * scale_), static_cast<float>(stroke_width * scale_), ChromePrimitiveKind::rectangle, replace, z});
    };
    auto line = [&](double x1, double y1, double x2, double y2, Color color, double width = 1) {
        result.primitives.push_back({pixels({x1,y1,x2-x1,y2-y1}), {}, color, 0, static_cast<float>(width * scale_), ChromePrimitiveKind::line, false, z});
    };
    auto label = [&](Rect bounds, std::string value, Color color, float size = 13, bool bold = false, ChromeTextAlignment alignment = ChromeTextAlignment::leading) {
        result.texts.push_back({pixels(bounds), std::move(value), color, size, bold, "Segoe UI Variable Text", alignment, z});
    };
    auto opacity = [](Color color, double amount) {
        color.a = static_cast<std::uint8_t>(std::clamp(std::lround(color.a * amount), 0L, 255L));
        return color;
    };
    if (owned) {
#ifdef STORMTTY_RAIL_HOST_FRAGMENT
#include STORMTTY_RAIL_HOST_FRAGMENT
#else
        throw std::runtime_error("Host characterization requires the extracted production fragment");
#endif
    } else {
#include "reference/workspace_rail_original.inc"
    }
}

void append_original(WindowScene& result, Fixture fixture) {
    std::vector<std::string> trace;
    append_environment(result, std::move(fixture), false, trace);
}

void append_owned(WindowScene& scene, const Fixture& fixture) {
    std::vector<WorkspaceRailRow> rows;
    for (const auto& row : fixture.navigation.projects) {
        const auto* project = fixture.projects.find(row.project);
        if (!project) continue;
        const double focus = fixture.focus.at(row.project).amount;
        const auto icon = color_hex(project->metadata.accent,
            blend(fixture.style.secondary, fixture.style.accent, focus));
        rows.push_back({row.project, row.bounds, project->metadata.name, project->metadata.icon,
                        icon, row.project == fixture.projects.active, fixture.unread.at(row.project),
                        focus, fixture.hovers.at((1ull << 62) | row.project.value)});
    }
    auto style = fixture.style;
    if (style.high_contrast) style.surface = fixture.contrast_background;
    if (fixture.backdrop) style.surface.a = 240;
    const WorkspaceRailView view{
        .rail = fixture.navigation.rail,
        .toggle = fixture.navigation.toggle,
        .new_workspace = fixture.navigation.new_project,
        .settings = fixture.navigation.settings,
        .caption_height = fixture.caption,
        .expanded = fixture.navigation.expanded,
        .rows = rows,
        .toggle_hover = fixture.hovers.at(8),
        .new_workspace_hover = fixture.hovers.at(9),
        .settings_hover = fixture.hovers.at(11),
    };
    WorkspaceRailRenderer{}.append_scene(scene, view, style);
}
void compare_scene(const WindowScene& actual, const WindowScene& expected) {
    CHECK(actual.primitives == expected.primitives);
    CHECK(actual.texts == expected.texts);
    CHECK(actual.background == expected.background);
    CHECK(actual.chrome_generation == expected.chrome_generation);
    CHECK(actual.force_redraw == expected.force_redraw);
    CHECK(actual.panes.size() == expected.panes.size());
    CHECK(actual.panes.front().id == expected.panes.front().id);
    CHECK(actual.panes.front().content == expected.panes.front().content);
    CHECK(actual.panes.front().frame == expected.panes.front().frame);
    CHECK(actual.panes.front().focused == expected.panes.front().focused);
    CHECK(actual.panes.front().cursor_on == expected.panes.front().cursor_on);
    CHECK(actual.panes.front().text_blink_on == expected.panes.front().text_blink_on);
    CHECK(actual.panes.front().cursor_reveal == expected.panes.front().cursor_reveal);
    const auto actual_order = chrome_paint_order(actual);
    const auto expected_order = chrome_paint_order(expected);
    CHECK(actual_order.size() == expected_order.size());
    for (std::size_t i = 0; i < actual_order.size(); ++i) {
        CHECK(actual_order[i].z == expected_order[i].z);
        CHECK(actual_order[i].kind == expected_order[i].kind);
        CHECK(actual_order[i].index == expected_order[i].index);
    }
    for (std::size_t i = 1; i < actual.primitives.size(); ++i) {
        CHECK(!actual.primitives[i].clip.has_value());
        CHECK(actual.primitives[i].z == ChromeZOrder{});
    }
    for (std::size_t i = 1; i < actual.texts.size(); ++i) {
        CHECK(!actual.texts[i].clip.has_value());
        CHECK(actual.texts[i].z == ChromeZOrder{});
    }
}

WindowScene seeded_scene() {
    WindowScene scene;
    scene.background = {3, 7, 11, 129};
    scene.chrome_generation = 912;
    scene.force_redraw = true;
    auto frame = std::make_shared<RenderFrame>();
    frame->generation = 481;
    scene.panes.push_back({777, {2, 3, 4, 5}, frame, false, false, false, .4f});
    ChromePrimitive marker;
    marker.rect = {2, 3, 4, 5};
    marker.fill = {1, 2, 3, 4};
    marker.clip = PixelRect{1, 1, 1, 1};
    marker.z = {ChromeLayer::modal, 3};
    scene.primitives.push_back(marker);
    ChromeText label;
    label.rect = {4, 5, 6, 7};
    label.text = "Existing clipped modal";
    label.clip = PixelRect{3, 4, 5, 6};
    label.z = {ChromeLayer::modal, 3};
    scene.texts.push_back(label);
    return scene;
}

#ifdef STORMTTY_RAIL_HOST_FRAGMENT
void host_admission_edges() {
    Fixture f;
    const ProjectWorkspaceId active{17}, missing{23}, other{41};
    f.projects.active = active;
    f.projects.entries = {{active, {"active", "folder", ""}}, {other, {"other", "git", ""}}};
    f.navigation = layout_project_navigation(800, 400, 32, {active, missing, other}, 0, true, false, false);
    f.navigation.new_project.height = 0;
    f.navigation.settings.height = 0;
    f.hovers[9] = 1;
    // Both focus maps are initially empty: the real operator[] insertion path
    // remains in the host and must occur only for admitted catalog identities.
    auto original = seeded_scene();
    auto actual = original;
    std::vector<std::string> expected_trace, actual_trace;
    append_environment(original, f, false, expected_trace);
    append_environment(actual, f, true, actual_trace);
    compare_scene(actual, original);
    CHECK(actual_trace == expected_trace);
    const std::vector<std::string> expected{
        "hover:8", "find:17", "active", "hover:" + std::to_string((1ull << 62) | 17),
        "focus:17", "find:23", "find:41", "active",
        "hover:" + std::to_string((1ull << 62) | 41), "focus:41", "watch:41", "hover:9"};
    CHECK(actual_trace == expected);
    f.navigation.rail.width = 0;
    actual_trace.clear();
    auto hidden = seeded_scene();
    const auto unchanged = hidden;
    append_environment(hidden, f, true, actual_trace);
    compare_scene(hidden, unchanged);
    CHECK(actual_trace.empty());
}
#endif

void emission_matrix() {
    constexpr std::array<double, 4> scales{1, 1.25, 1.5, 2};
    constexpr std::array<double, 6> widths{0, 1, 43, 319, 640, 1280};
    constexpr std::array<double, 8> heights{0, 32, 107, 108, 187, 188, 480, 960};
    constexpr std::array<double, 3> samples{0, .37, 1};
    constexpr std::array icons{"folder", "code", "terminal", "bolt", "globe", "git", "flask", "cloud", "book", "unknown", ""};
    constexpr std::array accents{"#12aBef", "", "#00zz00", "#00000", "bad"};
    std::size_t cases = 0;
    for (double scale : scales) for (double width : widths) for (double height : heights) {
        for (unsigned flags = 0; flags < 64; ++flags) {
            Fixture f;
            f.caption = flags & 16 ? 0 : std::min(32.0, height);
            std::vector<ProjectWorkspaceId> ids;
            for (unsigned i = 0; i < icons.size(); ++i) {
                const ProjectWorkspaceId id{101 + i * 17};
                ids.push_back(id);
                f.projects.entries.push_back({id, {i % 2 ? "Task café 文件 👩‍💻" : "", icons[i], accents[(flags + i) % accents.size()]}});
                f.focus[id] = {samples[(flags + i) % samples.size()]};
                f.hovers[(1ull << 62) | id.value] = samples[(flags / 3 + i) % samples.size()];
                f.unread[id] = (flags + i) % 2 != 0;
            }
            f.projects.active = ids[flags % ids.size()];
            if (flags & 32) {
                std::reverse(ids.begin(), ids.end());
                // A missing catalog identity is skipped without shifting geometry.
                f.projects.entries.erase(f.projects.entries.begin() + 3);
            }
            f.navigation = layout_project_navigation(width, height, f.caption, ids,
                flags % 4, (flags & 1) != 0, (flags & 2) != 0, false);
            if (flags & 32) {
                // Synthetic non-origin rail: preserve the legacy seam's use of
                // width/caption rather than silently adding rail.x or rail.y.
                f.navigation.rail.x = 9.25;
                f.navigation.rail.y += .75;
            }
            f.backdrop = (flags & 4) != 0;
            f.hovers[8] = samples[flags % samples.size()];
            f.hovers[9] = samples[(flags / 3) % samples.size()];
            f.hovers[11] = samples[(flags / 7) % samples.size()];
            const bool light = (flags & 16) != 0;
            f.style = {
                .surface = light ? Color{239, 242, 245, 255} : Color{23, 25, 29, 255},
                .raised = light ? Color{248, 249, 252, 255} : Color{45, 51, 63, 255},
                .selected_surface = {64, 73, 81, 255},
                .seam = {78, 87, 99, 140},
                .ink = light ? Color{21, 24, 29, 255} : Color{236, 238, 242, 255},
                .secondary = {144, 161, 177, 255},
                .accent = {171, 209, 238, 255},
                .scale = scale,
                .high_contrast = (flags & 8) != 0,
            };
            const auto seed = seeded_scene();
            auto actual = seed;
            auto expected = seed;
            append_original(expected, f);
            append_owned(actual, f);
            compare_scene(actual, expected);
#ifdef STORMTTY_RAIL_HOST_FRAGMENT
            auto original_host = seed;
            auto owned_host = seed;
            std::vector<std::string> original_trace, owned_trace;
            append_environment(original_host, f, false, original_trace);
            append_environment(owned_host, f, true, owned_trace);
            compare_scene(owned_host, original_host);
            CHECK(owned_trace == original_trace);
            if (f.navigation.rail.width == 0) CHECK(owned_trace.empty());
#endif
            if (f.navigation.rail.width > 0) {
                auto surface = f.style.high_contrast ? f.contrast_background : f.style.surface;
                if (f.backdrop) surface.a = 240;
                CHECK(actual.primitives[1].fill == surface);
                CHECK(actual.primitives[1].background_replace);
                const PixelRect seam{static_cast<float>((f.navigation.rail.width - 1 / scale) * scale),
                    static_cast<float>(f.caption * scale), static_cast<float>((1 / scale) * scale),
                    static_cast<float>(f.navigation.rail.height * scale)};
                CHECK(actual.primitives[2].rect == seam);
            }
            CHECK(actual.primitives.front() == seed.primitives.front());
            CHECK(actual.texts.front() == seed.texts.front());
            ++cases;
        }
    }
    CHECK(cases == 12288);
    std::cout << "workspace rail matrix: " << cases << " cases\n";
}

} // namespace

int main() {
    return run_core_tests("workspace rail renderer", [] {
        emission_matrix();
#ifdef STORMTTY_RAIL_HOST_FRAGMENT
        host_admission_edges();
#endif
    });
}
