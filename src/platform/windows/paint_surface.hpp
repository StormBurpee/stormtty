#pragma once

#include "stormtty/renderer.hpp"

#include <d2d1_1.h>
#include <dwrite.h>

namespace stormtty::windows {

D2D1_COLOR_F paint_color(Color color, float opacity = 1) noexcept;
D2D1_RECT_F paint_rect(PixelRect rect) noexcept;

// Stack-only drawing state. The context and its active draw transaction outlive
// these scopes. Copying would pop the native stack twice, so it is forbidden.
class PaintClip final {
public:
    PaintClip(ID2D1DeviceContext* context, PixelRect bounds);
    ~PaintClip();
    PaintClip(const PaintClip&) = delete;
    PaintClip& operator=(const PaintClip&) = delete;

private:
    ID2D1DeviceContext* context_;
};

class PaintOpacity final {
public:
    PaintOpacity(ID2D1DeviceContext* context, PixelRect bounds, float opacity);
    ~PaintOpacity();
    PaintOpacity(const PaintOpacity&) = delete;
    PaintOpacity& operator=(const PaintOpacity&) = delete;

private:
    ID2D1DeviceContext* context_;
    bool active_;
};

// Short-lived, renderer-thread draw borrow, never a GPU/resource owner. Geometry
// is physical pixels: the renderer configures the context at 96 DPI. No target,
// transaction, device, cache, material-policy or presentation controls escape.
class PaintSurface final {
public:
    PaintSurface(
        ID2D1DeviceContext& context,
        ID2D1SolidColorBrush& brush,
        ID2D1Brush* replacement_background = nullptr) noexcept;
    PaintSurface(const PaintSurface&) = delete;
    PaintSurface& operator=(const PaintSurface&) = delete;

    PaintClip clip(PixelRect bounds);
    PaintOpacity opacity(PixelRect bounds, float opacity);
    void fill(PixelRect bounds, Color color, float opacity = 1, bool replace = false);
    void fill_rounded(PixelRect bounds, float radius, Color color, bool replace);
    void stroke_rounded(PixelRect bounds, float radius, Color color, float width);
    void line(PixelRect endpoints, Color color, float width);
    void text_layout(PixelRect bounds, IDWriteTextLayout& layout, Color color);

private:
    ID2D1DeviceContext& context_;
    ID2D1SolidColorBrush& brush_;
    ID2D1Brush* replacement_background_;
};

} // namespace stormtty::windows
