#include "paint_surface.hpp"
#include "image_cache.hpp"

#include "stormtty/scene_damage.hpp"

namespace stormtty::windows {

D2D1_COLOR_F paint_color(Color color, float opacity) noexcept {
    return {
        float(color.r) / 255,
        float(color.g) / 255,
        float(color.b) / 255,
        float(color.a) / 255 * opacity};
}

D2D1_RECT_F paint_rect(PixelRect rect) noexcept {
    return {rect.x, rect.y, rect.x + rect.width, rect.y + rect.height};
}

PaintClip::PaintClip(ID2D1DeviceContext* context, PixelRect bounds)
    : context_(context) {
    context_->PushAxisAlignedClip(paint_rect(bounds), D2D1_ANTIALIAS_MODE_ALIASED);
}

PaintClip::~PaintClip() {
    context_->PopAxisAlignedClip();
}

PaintOpacity::PaintOpacity(ID2D1DeviceContext* context, PixelRect bounds, float opacity)
    : context_(context), active_(opacity < 1) {
    if (active_) {
        D2D1_LAYER_PARAMETERS1 parameters{};
        parameters.contentBounds = paint_rect(bounds);
        parameters.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
        parameters.maskTransform._11 = 1;
        parameters.maskTransform._22 = 1;
        parameters.opacity = opacity;
        parameters.layerOptions = D2D1_LAYER_OPTIONS1_NONE;
        context_->PushLayer(&parameters, nullptr);
    }
}

PaintOpacity::~PaintOpacity() {
    if (active_) {
        context_->PopLayer();
    }
}

PaintSurface::PaintSurface(
    ID2D1DeviceContext& context,
    ID2D1SolidColorBrush& brush,
    ID2D1Brush* replacement_background,
    ID2D1DeviceContext4* color_context) noexcept
    : context_(context),
      brush_(brush),
      replacement_background_(replacement_background),
      color_context_(color_context) {}

PaintClip PaintSurface::clip(PixelRect bounds) {
    return PaintClip(&context_, bounds);
}

PaintOpacity PaintSurface::opacity(PixelRect bounds, float opacity) {
    return PaintOpacity(&context_, bounds, opacity);
}

void PaintSurface::fill(PixelRect bounds, Color color, float opacity, bool replace) {
    if (!render_geometry::nonempty(bounds)) {
        return;
    }
    brush_.SetColor(paint_color(color, opacity));
    if (replace && replacement_background_) {
        context_.FillRectangle(paint_rect(bounds), replacement_background_);
    }
    if (replace && !replacement_background_) {
        context_.SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
    }
    context_.FillRectangle(paint_rect(bounds), &brush_);
    if (replace) {
        context_.SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    }
}

void PaintSurface::fill_rounded(PixelRect bounds, float radius, Color color, bool replace) {
    const D2D1_ROUNDED_RECT rounded{paint_rect(bounds), radius, radius};
    brush_.SetColor(paint_color(color));
    if (replace && replacement_background_) {
        context_.FillRoundedRectangle(rounded, replacement_background_);
    }
    if (replace && !replacement_background_) {
        context_.SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
    }
    context_.FillRoundedRectangle(rounded, &brush_);
    if (replace) {
        context_.SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    }
}

void PaintSurface::stroke_rounded(PixelRect bounds, float radius, Color color, float width) {
    const D2D1_ROUNDED_RECT rounded{paint_rect(bounds), radius, radius};
    brush_.SetColor(paint_color(color));
    context_.DrawRoundedRectangle(rounded, &brush_, width);
}

void PaintSurface::line(PixelRect endpoints, Color color, float width) {
    brush_.SetColor(paint_color(color));
    context_.DrawLine(
        {endpoints.x, endpoints.y},
        {endpoints.x + endpoints.width, endpoints.y + endpoints.height},
        &brush_,
        width);
}

void PaintSurface::line(D2D1_POINT_2F start, D2D1_POINT_2F end, Color color, float width) {
    brush_.SetColor(paint_color(color));
    context_.DrawLine(start, end, &brush_, width);
}

void PaintSurface::stroke_rectangle(D2D1_RECT_F bounds, Color color, float width) {
    brush_.SetColor(paint_color(color));
    context_.DrawRectangle(bounds, &brush_, width);
}

bool PaintSurface::supports_color_glyphs() const noexcept {
    return color_context_ != nullptr;
}

void PaintSurface::glyph_run(D2D1_POINT_2F origin, const DWRITE_GLYPH_RUN& run, Color color) {
    brush_.SetColor(paint_color(color));
    context_.DrawGlyphRun(origin, &run, &brush_, DWRITE_MEASURING_MODE_NATURAL);
}

void PaintSurface::color_glyph_layer(const DWRITE_COLOR_GLYPH_RUN1& layer, Color foreground) {
    const D2D1_POINT_2F position{layer.baselineOriginX, layer.baselineOriginY};
    switch (layer.glyphImageFormat) {
    case DWRITE_GLYPH_IMAGE_FORMATS_PNG:
    case DWRITE_GLYPH_IMAGE_FORMATS_JPEG:
    case DWRITE_GLYPH_IMAGE_FORMATS_TIFF:
    case DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8:
        color_context_->DrawColorBitmapGlyphRun(
            layer.glyphImageFormat, position, &layer.glyphRun,
            DWRITE_MEASURING_MODE_NATURAL, D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION_DEFAULT);
        break;
    case DWRITE_GLYPH_IMAGE_FORMATS_SVG:
        brush_.SetColor(paint_color(foreground));
        color_context_->DrawSvgGlyphRun(
            position, &layer.glyphRun, &brush_, nullptr, 0, DWRITE_MEASURING_MODE_NATURAL);
        break;
    default:
        brush_.SetColor(layer.paletteIndex == 0xffff ? paint_color(foreground) : layer.runColor);
        context_.DrawGlyphRun(position, &layer.glyphRun, &brush_, DWRITE_MEASURING_MODE_NATURAL);
        break;
    }
}

void PaintSurface::image(
    ImageCache& cache,
    std::uint64_t pane_id,
    const ImagePixels& pixels,
    std::uint64_t render_tick,
    PixelRect destination,
    D2D1_RECT_F source) {
    auto* bitmap = cache.bitmap(context_, pane_id, pixels, render_tick);
    if (!bitmap) {
        return;
    }
    const auto destination_rect = paint_rect(destination);
    context_.DrawBitmap(
        bitmap, &destination_rect, 1, D2D1_INTERPOLATION_MODE_LINEAR, &source, nullptr);
}

void PaintSurface::text_layout(PixelRect bounds, IDWriteTextLayout& layout, Color color) {
    brush_.SetColor(paint_color(color));
    context_.DrawTextLayout(
        {bounds.x, bounds.y}, &layout, &brush_, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

} // namespace stormtty::windows
