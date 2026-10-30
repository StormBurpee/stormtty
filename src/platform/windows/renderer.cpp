#include "stormtty/renderer.hpp"
#include "stormtty/scene_damage.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "chrome_painter.hpp"
#include "graphics_error.hpp"
#include "image_cache.hpp"
#include "render_device.hpp"
#include "smoke_teardown_trace.hpp"
#include "terminal_painter.hpp"
#include "text_resources.hpp"

#ifdef STORMTTY_RENDER_DEVICE_TESTS
#include "render_device_test_support.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace stormtty {
namespace {
using windows::GraphicsError;
using windows::SurfaceChanges;
using windows::check;
using windows::device_lost;

struct Apartment {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    bool smoke{};

    ~Apartment() {
        windows::smoke_teardown_trace(smoke, "renderer apartment unwind begin");
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
        windows::smoke_teardown_trace(smoke, "renderer apartment unwind end");
    }
};
} // namespace

struct Renderer::Impl {
    Apartment apartment;
    // The native factory dies after TextResources, but before the apartment.
    // Successful teardown explicitly clears the GPU graph before either one.
    std::unique_ptr<windows::RenderDevice> device;
    RendererOptions options;
    RendererStats counters;
    float dpi{96};
    std::uint64_t tick{};
    bool pending_present{};
    std::unique_ptr<windows::TextResources> text;
    std::unique_ptr<windows::ImageCache> image_cache;
    SceneDamageTracker scene_damage;
    bool tracing_teardown{};

    Impl(void* window, RendererOptions requested)
        : device(std::make_unique<windows::RenderDevice>(windows::DeviceOptions{
              window, requested.force_warp, requested.composition})),
          options(std::move(requested)) {
        try {
            if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE) {
                check(apartment.result, "COM initialization");
            }
            windows::TextResources::validate_font(options.font);
            if (!options.maximum_image_dimension || options.maximum_image_dimension > 8192 ||
                options.shape_cache_bytes > 256u * 1024u * 1024u ||
                options.image_cache_bytes > 512u * 1024u * 1024u) {
                throw std::invalid_argument("Renderer cache or image dimensions exceed safety limits");
            }
            if (!std::isfinite(options.background_opacity)) {
                throw std::invalid_argument("Invalid background opacity");
            }
            options.background_opacity = std::clamp(options.background_opacity, 0.0f, 1.0f);
            text = std::make_unique<windows::TextResources>(options.shape_cache_bytes);
            image_cache = std::make_unique<windows::ImageCache>(windows::ImageCacheOptions{
                options.image_cache_bytes, options.maximum_image_dimension});
#ifdef STORMTTY_RENDER_DEVICE_TESTS
            device->set_teardown_trace(true);
            device->enable_teardown_trace();
            text->enable_teardown_trace();
            apartment.smoke = true;
#endif
            SurfaceChanges changes;
            device->initialize(changes);
            apply(changes);
            text->configure(options.font, dpi);
#ifdef STORMTTY_RENDER_DEVICE_TESTS
            windows::testing::checkpoint(windows::testing::AcquisitionPoint::text_configured);
#endif
        } catch (...) {
            // Impl's destructor does not run for a failed constructor. Preserve
            // the old interleaved implicit unwind, without shutdown waits/drains.
            image_cache.reset();
            device->release_failed_initialization_canvas();
            text.reset();
            throw;
        }
    }

    ~Impl() {
        tracing_teardown = options.smoke_teardown_diagnostics;
        apartment.smoke = tracing_teardown;
        device->set_teardown_trace(tracing_teardown);
        if (tracing_teardown) {
            device->enable_teardown_trace();
            text->enable_teardown_trace();
        }
        windows::smoke_teardown_trace(tracing_teardown, "renderer release_gpu begin");
        SurfaceChanges changes;
        device->release(windows::DeviceRelease::final_shutdown, *image_cache, changes);
        apply(changes);
        windows::smoke_teardown_trace(
            tracing_teardown, "renderer release_gpu end; implicit cache/font members begin");
    }

    void apply(const SurfaceChanges& changes) noexcept {
        if (changes.repaint) {
            scene_damage.invalidate();
        }
        if (changes.canvas_installed) {
            pending_present = false;
        }
    }

    float backdrop_opacity() const noexcept {
        return device->material_enabled() ? options.background_opacity : 1.0f;
    }

    RenderResult render(const WindowScene& scene) {
        if (!device->ready()) {
            return RenderResult::suspended;
        }
        SurfaceChanges changes;
        device->prepare_background(dpi, changes);
        apply(changes);
        ++tick;
        const auto bounds = device->bounds();
        const auto dirty = scene_damage.damage(scene, text->metrics(), bounds);
        if (dirty.empty() && !pending_present) {
            scene_damage.commit(scene);
            return RenderResult::unchanged;
        }
        if (!dirty.empty()) {
            const bool full_repaint = dirty.size() == 1 &&
                dirty[0].x <= 0 && dirty[0].y <= 0 &&
                dirty[0].x + dirty[0].width >= bounds.width &&
                dirty[0].y + dirty[0].height >= bounds.height;
            device->mark_canvas_write();
            const auto chrome_order = chrome_paint_order(scene);
            auto drawing = device->begin_canvas_draw();
            auto& surface = drawing.surface();
            try {
                for (auto damage : dirty) {
                    // Outward physical-pixel snapping prevents accumulated alpha
                    // at fractional layout edges during incremental repaint.
                    const auto right = std::ceil(damage.x + damage.width);
                    const auto bottom = std::ceil(damage.y + damage.height);
                    damage.x = std::floor(damage.x);
                    damage.y = std::floor(damage.y);
                    damage.width = right - damage.x;
                    damage.height = bottom - damage.y;
                    auto clip = surface.clip(damage);
                    drawing.clear();
                    surface.fill(bounds, scene.background, backdrop_opacity(), true);
                    for (const auto& pane : scene.panes) {
                        windows::TerminalPainter{}.paint(
                            pane, damage, dpi, backdrop_opacity(), tick,
                            surface, *text, *image_cache);
                    }
                    windows::ChromePainter{}.paint(
                        scene.primitives, scene.texts, chrome_order, damage,
                        bounds, device->material_enabled(), surface, *text);
                }
            } catch (...) {
                drawing.finish(false);
                scene_damage.invalidate();
                throw;
            }
            // EndDraw failure itself retains its existing recovery semantics;
            // it is intentionally outside the paint-body exception handler.
            check(drawing.finish(full_repaint), "Render canvas");
            counters.dirty_rectangles += dirty.size();
            scene_damage.commit(scene);
            pending_present = true;
        }
        const auto result = device->present_canvas();
        if (result != RenderResult::presented) {
            return result;
        }
        pending_present = false;
        ++counters.presents;
        return RenderResult::presented;
    }
};

Renderer::Renderer(void* window, RendererOptions options)
    : impl_(std::make_unique<Impl>(window, std::move(options))) {}

Renderer::~Renderer() {
    const bool smoke = impl_ && impl_->options.smoke_teardown_diagnostics;
    windows::smoke_teardown_trace(smoke, "renderer destructor begin");
    impl_.reset();
    windows::smoke_teardown_trace(smoke, "renderer destructor end");
}

Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

void Renderer::prepare_window_destruction() noexcept {
    if (!impl_) {
        return;
    }
    auto& renderer = *impl_;
    renderer.tracing_teardown = renderer.options.smoke_teardown_diagnostics;
    renderer.device->set_teardown_trace(renderer.tracing_teardown);
    windows::smoke_teardown_trace(renderer.tracing_teardown, "renderer prepare HWND destruction begin");
    SurfaceChanges changes;
    renderer.device->release(windows::DeviceRelease::final_shutdown, *renderer.image_cache, changes);
    renderer.apply(changes);
    windows::smoke_teardown_trace(renderer.tracing_teardown, "renderer prepare HWND destruction end");
}

void Renderer::resize(std::uint32_t width, std::uint32_t height, float dpi) {
    if (!std::isfinite(dpi) || dpi < 48 || dpi > 768) {
        throw std::invalid_argument("Invalid surface DPI");
    }
    if (width > 16384 || height > 16384 || std::uint64_t(width) * height > 64u * 1024u * 1024u) {
        throw std::length_error("Surface exceeds bounded renderer dimensions");
    }
    auto& renderer = *impl_;
    if (renderer.dpi != dpi) {
        renderer.dpi = dpi;
        if (renderer.text->set_dpi(dpi)) {
            renderer.scene_damage.invalidate();
        }
    }
    if (renderer.device->matches_size(width, height)) {
        return;
    }
    SurfaceChanges changes;
    try {
        renderer.device->resize(width, height, changes);
        renderer.apply(changes);
    } catch (const GraphicsError& error) {
        renderer.apply(changes);
        if (!width || !height || !device_lost(error.result)) {
            throw;
        }
        recreate_device();
    } catch (...) {
        renderer.apply(changes);
        throw;
    }
}

void Renderer::set_font(FontOptions font) {
    if (impl_->text->set_font(font)) {
        impl_->scene_damage.invalidate();
    }
}

void Renderer::set_background_opacity(float opacity) {
    if (!std::isfinite(opacity) || opacity < 0 || opacity > 1) {
        throw std::invalid_argument("Invalid background opacity");
    }
    if (impl_->options.background_opacity != opacity) {
        impl_->options.background_opacity = opacity;
        impl_->scene_damage.invalidate();
    }
}

void Renderer::set_additional_blur(float sigma) {
    if (!std::isfinite(sigma) || sigma < 0 || sigma > 64) {
        throw std::invalid_argument("Invalid additional blur");
    }
    SurfaceChanges changes;
    try {
        impl_->device->set_additional_blur(sigma, changes);
        impl_->apply(changes);
    } catch (...) {
        impl_->apply(changes);
        throw;
    }
}

bool Renderer::liquid_glass_available() const noexcept {
    return impl_->device->liquid_glass_available();
}

bool Renderer::start_liquid_glass(void* monitor) {
    SurfaceChanges changes;
    const bool started = impl_->device->start_liquid_glass(monitor, changes);
    impl_->apply(changes);
    return started;
}

void Renderer::stop_liquid_glass(const char* reason) noexcept {
    SurfaceChanges changes;
    impl_->device->stop_liquid_glass(reason, changes);
    impl_->apply(changes);
}

bool Renderer::liquid_glass_active() const noexcept {
    return impl_->device->liquid_glass_active();
}

std::string Renderer::liquid_glass_status() const {
    return impl_->device->liquid_glass_status();
}

bool Renderer::poll_liquid_glass() {
    SurfaceChanges changes;
    const bool updated = impl_->device->poll_liquid_glass(impl_->dpi, changes);
    impl_->apply(changes);
    return updated;
}

CellMetrics Renderer::metrics() const noexcept {
    return impl_->text->metrics();
}

RenderResult Renderer::render(const WindowScene& scene) {
    try {
        return impl_->render(scene);
    } catch (const GraphicsError& error) {
        if (!device_lost(error.result)) {
            throw;
        }
        recreate_device();
        return impl_->render(scene);
    }
}

void* Renderer::frame_latency_wait_handle() const noexcept {
    return impl_->device->frame_latency_wait_handle();
}

RendererStats Renderer::stats() const noexcept {
    auto result = impl_->counters;
    const auto text = impl_->text->stats();
    result.shaped_rows = text.shaped_rows;
    result.shape_cache_hits = text.cache_hits;
    result.shape_cache_bytes = text.retained_bytes;
    result.bundled_font = text.bundled_font;
    const auto images = impl_->image_cache->stats();
    result.image_uploads = images.uploads;
    result.image_cache_bytes = images.retained_bytes;
    const auto device = impl_->device->capabilities();
    result.warp = device.warp;
    result.composition = device.composition;
    result.host_backdrop = device.host_backdrop;
    result.color_fonts = device.color_glyphs && impl_->text->supports_color_glyphs();
    return result;
}

void Renderer::recreate_device() {
    SurfaceChanges changes;
    try {
        impl_->device->recreate(*impl_->image_cache, changes);
        impl_->apply(changes);
        ++impl_->counters.device_recoveries;
    } catch (...) {
        impl_->apply(changes);
        throw;
    }
}

void Renderer::cancel_alpha_probe() noexcept {
    impl_->device->cancel_alpha_probe();
}

bool Renderer::begin_alpha_probe(PixelRect area) {
    const auto capabilities = impl_->device->capabilities();
    RendererAlphaProbe metadata;
    metadata.effective_background_opacity = impl_->backdrop_opacity();
    metadata.composition = capabilities.composition;
    metadata.software = capabilities.warp;
    metadata.device_generation = impl_->counters.device_recoveries;
    metadata.presents = impl_->counters.presents;
    return impl_->device->begin_alpha_probe(area, metadata);
}

std::optional<RendererAlphaProbe> Renderer::poll_alpha_probe() {
    return impl_->device->poll_alpha_probe();
}

PixelRect Renderer::measure_chrome_text(const ChromeText& label) const {
    return impl_->text->measure_chrome_text(label);
}

std::vector<std::string> Renderer::wrap_chrome_text(const ChromeText& label) const {
    return impl_->text->wrap_chrome_text(label);
}

std::vector<std::uint8_t> Renderer::read_pixels() {
    return impl_->device->read_pixels();
}

} // namespace stormtty
