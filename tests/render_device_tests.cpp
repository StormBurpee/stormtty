// Native WARP resource-graph characterization. This executable compiles its own
// renderer objects with private acquisition checkpoints; production has none.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "../src/platform/windows/render_device.hpp"
#include "../src/platform/windows/render_device_test_support.hpp"
#include "../src/platform/windows/graphics_error.hpp"
#include "../src/platform/windows/image_cache.hpp"
#include "stormtty/renderer.hpp"

#include <io.h>
#include <array>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
using namespace stormtty;
using namespace stormtty::windows;
using testing::AcquisitionPoint;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class StderrCapture final {
public:
    StderrCapture() {
        std::fflush(stderr);
        file_ = std::tmpfile();
        require(file_ != nullptr, "Cannot create trace capture");
        saved_ = _dup(_fileno(stderr));
        if (saved_ < 0 || _dup2(_fileno(file_), _fileno(stderr)) != 0) {
            if (saved_ >= 0) {
                _close(saved_);
            }
            std::fclose(file_);
            throw std::runtime_error("Cannot redirect trace capture");
        }
    }

    ~StderrCapture() {
        restore();
        std::fclose(file_);
    }

    std::string read() {
        restore();
        std::rewind(file_);
        std::string result;
        std::array<char, 4096> buffer{};
        while (const auto count = std::fread(buffer.data(), 1, buffer.size(), file_)) {
            result.append(buffer.data(), count);
        }
        return result;
    }

private:
    void restore() noexcept {
        if (saved_ >= 0) {
            std::fflush(stderr);
            _dup2(saved_, _fileno(stderr));
            _close(saved_);
            saved_ = -1;
        }
    }

    FILE* file_{};
    int saved_{-1};
};

void ordered(const std::string& trace, std::initializer_list<const char*> markers) {
    std::size_t cursor = 0;
    for (const auto* marker : markers) {
        const auto found = trace.find(marker, cursor);
        require(found != std::string::npos, "Native resource release order changed or marker missing");
        cursor = found + std::char_traits<char>::length(marker);
    }
}

void constructor_failure_test() {
    const std::array checkpoints{
        AcquisitionPoint::factory,
        AcquisitionPoint::device,
        AcquisitionPoint::drawing_device,
        AcquisitionPoint::context,
        AcquisitionPoint::brush,
        AcquisitionPoint::text_configured};
    for (const auto point : checkpoints) {
        StderrCapture capture;
        bool failed = false;
        testing::fail_after(point);
        try {
            RendererOptions options;
            options.force_warp = true;
            options.font.family = "Consolas";
            Renderer renderer(nullptr, options);
        } catch (const std::runtime_error& error) {
            failed = std::string(error.what()) == "Synthetic native acquisition failure";
        }
        const auto trace = capture.read();
        std::cout << "Constructor acquisition checkpoint=" << static_cast<int>(point) << '\n' << trace;
        require(failed, "Constructor checkpoint was not reached");
        require(trace.find("completion query") == std::string::npos &&
                trace.find("ClearState") == std::string::npos &&
                trace.find("Flush begin") == std::string::npos &&
                trace.find("SetTarget null") == std::string::npos,
                "Constructor failure added shutdown work");
        ordered(trace, {
            "renderer write_factory Release begin",
            "renderer d2d_factory Release begin",
            "renderer apartment unwind begin",
            "renderer apartment unwind end"});
        if (trace.find("renderer dc4 Release begin") != std::string::npos) {
            ordered(trace, {
                "renderer write_factory Release begin",
                "renderer dc4 Release begin",
                "renderer dc Release begin"});
        }
        if (point == AcquisitionPoint::brush || point == AcquisitionPoint::text_configured) {
            ordered(trace, {
                "renderer brush Release begin",
                "renderer write_factory Release begin",
                "renderer dc Release begin",
                "renderer d2d_device Release begin",
                "renderer d2d_factory Release begin",
                "renderer immediate Release begin",
                "renderer device Release begin"});
        }
        if (point == AcquisitionPoint::context) {
            ordered(trace, {
                "renderer write_factory Release begin",
                "renderer dc Release begin",
                "renderer d2d_device Release begin",
                "renderer d2d_factory Release begin",
                "renderer immediate Release begin",
                "renderer device Release begin"});
        }
        if (point == AcquisitionPoint::drawing_device) {
            ordered(trace, {
                "renderer write_factory Release begin",
                "renderer d2d_device Release begin",
                "renderer d2d_factory Release begin",
                "renderer immediate Release begin",
                "renderer device Release begin"});
        }
        if (point == AcquisitionPoint::device) {
            ordered(trace, {
                "renderer d2d_factory Release begin",
                "renderer immediate Release begin",
                "renderer device Release begin"});
        }
    }
}

struct Apartment {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    Apartment() {
        check(result, "Initialize native owner test apartment");
    }
    ~Apartment() {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};

struct Fixture {
    Apartment apartment;
    RenderDevice device{DeviceOptions{nullptr, true, true}};
    ImageCache images{ImageCacheOptions{1024, 8192}};

    Fixture() {
        SurfaceChanges changes;
        device.initialize(changes);
    }

    ~Fixture() {
        SurfaceChanges changes;
        device.release(DeviceRelease::final_shutdown, images, changes);
    }
};

void canvas_failure_test() {
    for (const auto point : {AcquisitionPoint::canvas_texture, AcquisitionPoint::canvas_bitmap}) {
        Fixture fixture;
        SurfaceChanges changes;
        testing::fail_after(point);
        bool failed = false;
        try {
            fixture.device.resize(32, 24, changes);
        } catch (const std::runtime_error& error) {
            failed = std::string(error.what()) == "Synthetic native acquisition failure";
        }
        require(failed && changes.repaint && !changes.canvas_installed,
                "Failed canvas replacement lost invalidation or claimed a completed canvas");
        require(!fixture.device.ready() && fixture.device.read_pixels().empty(),
                "Failed canvas replacement retained partial target");
        SurfaceChanges restored;
        fixture.device.resize(32, 24, restored);
        require(restored.repaint && restored.canvas_installed && fixture.device.ready(),
                "Canvas replacement did not recover after failed acquisition");
    }
}

void draw_and_release_test() {
    Fixture fixture;
    SurfaceChanges changes;
    fixture.device.resize(32, 24, changes);
    require(changes.canvas_installed && fixture.device.material_enabled(), "Offscreen canvas setup failed");
    {
        auto draw = fixture.device.begin_canvas_draw();
        draw.clear();
        draw.surface().fill({0, 0, 32, 24}, {30, 210, 60, 255}, 1, true);
        check(draw.finish(true), "Finish native test canvas");
        require(draw.finish(true) == D2DERR_WRONG_STATE, "Draw scope allowed double EndDraw");
    }
    require(fixture.device.present_canvas() == RenderResult::presented, "Offscreen presentation failed");
    const auto pixels = fixture.device.read_pixels();
    require(pixels.size() == 32 * 24 * 4 && pixels[0] == 30 && pixels[1] == 210 && pixels[2] == 60,
            "Native canvas readback changed pixel conversion");
    try {
        auto draw = fixture.device.begin_canvas_draw();
        draw.surface().fill({0, 0, 1, 1}, {1, 2, 3, 255});
        throw std::runtime_error("paint body");
    } catch (const std::runtime_error&) {
    }
    {
        auto draw = fixture.device.begin_canvas_draw();
        check(draw.finish(false), "Draw scope did not EndDraw while unwinding");
    }
    fixture.device.cancel_alpha_probe();
    require(!fixture.device.poll_alpha_probe(), "Canceled probe retained native staging");
    SurfaceChanges minimized;
    fixture.device.resize(0, 0, minimized);
    require(minimized.repaint && !minimized.canvas_installed && !fixture.device.ready(),
            "Zero-size resize changed canvas installation contract");
}

void stale_taint_test() {
    Fixture fixture;
    SurfaceChanges changes;
    fixture.device.resize(16, 16, changes);
    {
        auto draw = fixture.device.begin_canvas_draw();
        draw.clear();
        check(draw.finish(true), "Initialize privacy-test canvas");
    }
    RendererAlphaProbe metadata;
    require(fixture.device.begin_alpha_probe({0, 0, 4, 4}, metadata), "Clean alpha probe rejected");
    testing::taint_canvas(fixture.device);
    require(!fixture.device.poll_alpha_probe(), "Tainted canvas exposed pending alpha probe");
    require(!fixture.device.begin_alpha_probe({0, 0, 4, 4}, metadata), "Tainted canvas accepted alpha probe");
    bool rejected = false;
    try {
        fixture.device.read_pixels();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "Tainted canvas allowed readback");
    SurfaceChanges stopped;
    fixture.device.stop_liquid_glass("Off for synthetic privacy test.", stopped);
    require(stopped.repaint, "Stopping material did not invalidate the scene");
    {
        auto draw = fixture.device.begin_canvas_draw();
        draw.clear();
        check(draw.finish(false), "Partial privacy-test repaint");
    }
    require(!fixture.device.begin_alpha_probe({0, 0, 4, 4}, metadata),
            "Stop or partial repaint cleared stale canvas taint");
    try {
        auto draw = fixture.device.begin_canvas_draw();
        draw.clear();
        throw std::runtime_error("synthetic aborted repaint");
    } catch (const std::runtime_error&) {
    }
    require(!fixture.device.begin_alpha_probe({0, 0, 4, 4}, metadata),
            "Aborted repaint cleared stale canvas taint");
    {
        auto draw = fixture.device.begin_canvas_draw();
        draw.clear();
        check(draw.finish(true), "Full privacy-test repaint");
    }
    require(fixture.device.read_pixels().size() == 16 * 16 * 4,
            "Successful full clean repaint did not restore readback");
    require(fixture.device.begin_alpha_probe({0, 0, 4, 4}, metadata),
            "Successful full clean repaint did not restore alpha diagnostic");
}

void normal_release_order_test() {
    StderrCapture capture;
    {
        Fixture fixture;
        fixture.device.enable_teardown_trace();
        fixture.device.set_teardown_trace(true);
        SurfaceChanges changes;
        fixture.device.resize(16, 16, changes);
        {
            auto draw = fixture.device.begin_canvas_draw();
            draw.clear();
            ImagePixels pixels;
            pixels.width = 1;
            pixels.height = 1;
            pixels.bytes = {20, 40, 60, 255};
            draw.surface().image(
                fixture.images, 1, pixels, 1, {0, 0, 1, 1}, {0, 0, 1, 1});
            require(fixture.images.stats().retained_bytes == 4,
                    "Release-order fixture did not retain a native image bitmap");
            check(draw.finish(true), "Finish release-order canvas");
        }
    }
    const auto trace = capture.read();
    std::cout << "Normal offscreen native release trace\n" << trace;
    ordered(trace, {
        "renderer completion query begin",
        "renderer SetTarget null begin",
        "renderer canvas Release begin",
        "renderer canvas_texture Release begin",
        "renderer brush Release begin",
        "renderer image cache clear begin",
        "renderer image cache clear end",
        "renderer dc Release begin",
        "renderer d2d_device Release begin",
        "renderer latency handle close begin",
        "renderer latency handle close end",
        "renderer final device release; no swapchain replacement drain",
        "renderer opaque HWND destruction begin",
        "renderer immediate Release begin",
        "renderer device Release begin",
        "renderer d2d_factory Release begin"});
    require(trace.find("renderer ClearState begin") == std::string::npos,
            "Offscreen final teardown added a replacement drain");
}

static_assert(!std::is_copy_constructible_v<CanvasDraw>);
static_assert(!std::is_move_constructible_v<CanvasDraw>);
static_assert(!std::is_copy_constructible_v<RenderDevice>);

} // namespace

int main() {
    try {
        constructor_failure_test();
        canvas_failure_test();
        draw_and_release_test();
        stale_taint_test();
        normal_release_order_test();
        std::cout << "PASS native render device: acquisition unwind, scoped draw, canvas recovery, release order\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL render device: " << error.what() << '\n';
        return 1;
    }
}
