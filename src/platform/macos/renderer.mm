#include "stormtty/renderer.hpp"
#include "chrome_painter.hpp"
#include "terminal_painter.hpp"
#include "core_text_font_resources.hpp"
#include "metal_texture_resources.hpp"
#include "metal_compositor.hpp"
#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace stormtty {
namespace {
struct PaneStamp {
    std::uint64_t id{}, generation{}, images{};
    PixelRect content;
    bool focused{}, cursor{}, blink{};
    bool operator==(const PaneStamp&) const = default;
};

}

struct Renderer::Impl {
    // Early storage keeps native resources alive until texture/font destruction.
    // Deferred emplacement preserves the original shared-state allocation phase.
    std::optional<macos::MetalCompositor> compositor;
    RendererOptions options;
    RendererStats statistics;
    macos::CoreTextFontResources font_resources;
    std::uint32_t width{}, height{};
    float dpi{96};
    std::uint64_t font_generation{1}, chrome_generation{};
    std::vector<PaneStamp> previous;
    Color previous_background{};
    bool have_previous{};
    macos::MetalTextureResources textures;

    explicit Impl(void* handle, RendererOptions settings)
        : options(std::move(settings)),
          textures({options.shape_cache_bytes, options.image_cache_bytes,
                    options.maximum_image_dimension}) {
        compositor.emplace(handle);
        try {
            if (options.force_warp) {
                throw std::invalid_argument("WARP is Windows-only; macOS native tests require a Metal device");
            }
            if (!std::isfinite(options.background_opacity) ||
                options.background_opacity < 0.f || options.background_opacity > 1.f) {
                throw std::invalid_argument("Invalid default background opacity");
            }
            create_device();
            measure();
        } catch (...) {
            compositor->release_generation_references_for_unwind();
            throw;
        }
    }

    ~Impl() {
        compositor->release_generation_references_for_unwind();
    }

    void create_device() {
        compositor->create_device(textures);
        statistics.composition = true;
        statistics.color_fonts = true;
        have_previous = false;
    }

    void measure() {
        font_resources.reset(options.font, dpi);
        ++font_generation;
        textures.clear_rows();
        have_previous = false;
    }

    void resize(std::uint32_t w, std::uint32_t h, float new_dpi) {
        if (!std::isfinite(new_dpi) || new_dpi < 48 || new_dpi > 768) {
            throw std::invalid_argument("Invalid renderer DPI");
        }
        macos::MetalCompositor::validate_surface_size(w, h);
        if (new_dpi != dpi) {
            dpi = new_dpi;
            measure();
        }
        compositor->update_layer_geometry(dpi);
        if (compositor->surface_matches(w, h)) {
            return;
        }
        width = w;
        height = h;
        have_previous = false;
        compositor->replace_surface(w, h, dpi);
    }

    RenderResult render(const WindowScene& scene) {
        if (!width || !height || !compositor->has_surface()) {
            return RenderResult::suspended;
        }
        if (scene.panes.size() > 4096 || scene.primitives.size() > 16384 ||
            scene.texts.size() > 4096) {
            throw std::runtime_error("Metal scene exceeds bounded primitive counts");
        }
        compositor->check_gpu_health();
        if (compositor->is_occluded()) {
            return RenderResult::occluded;
        }

        std::vector<PaneStamp> stamps;
        for (const auto& p : scene.panes) {
            if (p.frame) {
                stamps.push_back({p.id, p.frame->generation, p.frame->image_placements_generation,
                                  p.content, p.focused, p.cursor_on, p.text_blink_on});
            }
        }
        if (have_previous && !scene.force_redraw && chrome_generation == scene.chrome_generation &&
            stamps == previous && scene.background == previous_background) {
            return RenderResult::unchanged;
        }
        if (compositor->submission_busy()) {
            return RenderResult::busy;
        }

        @autoreleasepool {
            // Do not retain CAMetalDrawable across frames. Apple may briefly wait
            // inside nextDrawable; two in-flight buffers plus a three-drawable pool
            // avoid saturating it. This is not a proven nonblocking present path.
            auto paint = compositor->begin_frame(textures);
            if (!paint) {
                return RenderResult::busy;
            }
            Color canvas = scene.background;
            canvas.a = static_cast<std::uint8_t>(std::lround(options.background_opacity * 255));
            paint.fill({0, 0, float(width), float(height)}, canvas);
            macos::TerminalPainter terminal(paint, textures, font_resources.borrow_font(),
                                            options.font.locale, font_resources.metrics(), dpi,
                                            options.background_opacity, font_generation,
                                            options.font.ligatures);
            for (const auto& pane : scene.panes) {
                terminal.draw(pane);
            }
            paint.clip({0, 0, float(width), float(height)});
            macos::ChromePainter chrome(paint, {0, 0, float(width), float(height)}, dpi);
            for (const auto& item : chrome_paint_order(scene)) {
                if (item.kind == ChromeDrawKind::primitive) {
                    chrome.draw(scene.primitives[item.index], textures.remaining_upload_bytes());
                } else {
                    chrome.draw(scene.texts[item.index], textures.remaining_upload_bytes());
                }
            }
            paint.submit();
            ++statistics.presents;
            previous = std::move(stamps);
            chrome_generation = scene.chrome_generation;
            previous_background = scene.background;
            have_previous = true;
            return RenderResult::presented;
        }
    }
};

Renderer::Renderer(void* handle, RendererOptions options)
    : impl_(std::make_unique<Impl>(handle, std::move(options))) {}

Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

void Renderer::resize(std::uint32_t w, std::uint32_t h, float dpi) {
    impl_->resize(w, h, dpi);
}

void Renderer::set_font(FontOptions font) {
    impl_->options.font = std::move(font);
    impl_->measure();
}

void Renderer::set_background_opacity(float opacity) {
    if (!std::isfinite(opacity) || opacity < 0.f || opacity > 1.f) {
        throw std::invalid_argument("Background opacity must be between 0 and 1");
    }
    impl_->options.background_opacity = opacity;
    impl_->have_previous = false;
}

CellMetrics Renderer::metrics() const noexcept {
    return impl_->font_resources.metrics();
}

RenderResult Renderer::render(const WindowScene& scene) {
    return impl_->render(scene);
}

void* Renderer::frame_latency_wait_handle() const noexcept {
    return nullptr;
}

RendererStats Renderer::stats() const noexcept {
    auto result = impl_->statistics;
    const auto textures = impl_->textures.stats();
    result.shaped_rows = textures.shaped_rows;
    result.shape_cache_hits = textures.row_cache_hits;
    result.image_uploads = textures.image_uploads;
    result.dirty_rectangles = textures.dirty_rectangles;
    result.shape_cache_bytes = textures.row_bytes;
    result.image_cache_bytes = textures.image_bytes;
    return result;
}

// The alpha diagnostic currently targets the Windows DComp investigation.
// Explicitly unavailable rather than blocking AppKit or pretending a sample.
bool Renderer::begin_alpha_probe(PixelRect) {
    return false;
}

std::optional<RendererAlphaProbe> Renderer::poll_alpha_probe() {
    return std::nullopt;
}

void Renderer::cancel_alpha_probe() noexcept {}

std::vector<std::uint8_t> Renderer::read_pixels() {
    return impl_->compositor->read_pixels();
}

void Renderer::recreate_device() {
    const auto w = impl_->width, h = impl_->height;
    impl_->create_device();
    impl_->resize(w, h, impl_->dpi);
    ++impl_->statistics.device_recoveries;
}

}
