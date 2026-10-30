#include "paint_surface.hpp"

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
    ID2D1Brush* replacement_background) noexcept
    : context_(context),
      brush_(brush),
      replacement_background_(replacement_background) {}

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

void PaintSurface::text_layout(PixelRect bounds, IDWriteTextLayout& layout, Color color) {
    brush_.SetColor(paint_color(color));
    context_.DrawTextLayout(
        {bounds.x, bounds.y}, &layout, &brush_, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

} // namespace stormtty::windows
