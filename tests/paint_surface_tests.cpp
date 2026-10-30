// Native Windows WARP tests. Cross-compilation is not runtime evidence.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "../src/platform/windows/paint_surface.hpp"
#include "../src/platform/windows/com_owner.hpp"
#include "../src/platform/windows/graphics_error.hpp"
#include "stormtty/frame.hpp"

#include <d3d11.h>
#include <d2d1_1.h>

#include <array>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace stormtty;
using namespace stormtty::windows;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Apartment {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};

    Apartment() {
        check(result, "Initialize test apartment");
    }

    ~Apartment() {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};

struct WarpFixture {
    Apartment apartment;
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> immediate;
    Com<ID2D1Factory1> factory;
    Com<ID2D1Device> drawing_device;
    Com<ID2D1DeviceContext> context;

    WarpFixture() {
        D3D_FEATURE_LEVEL level{};
        check(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, device.put(), &level, immediate.put()),
            "Create paint-test WARP device");
        const D2D1_FACTORY_OPTIONS options{};
        check(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
            reinterpret_cast<void**>(factory.put())), "Create paint-test D2D factory");
        Com<IDXGIDevice> dxgi;
        check(device.as(dxgi), "Query paint-test DXGI device");
        check(factory->CreateDevice(dxgi.get(), drawing_device.put()), "Create paint-test D2D device");
        check(drawing_device->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, context.put()), "Create paint-test D2D context");
    }

    // Copy only this test's synthetic bitmap, without a window or desktop source.
    // Raw premultiplied BGRA avoids unpremultiplication hiding rounding errors.
    std::array<std::uint8_t, 4> pixel(ID2D1Bitmap1* bitmap, UINT x) {
        require(bitmap != nullptr, "Expected image was rejected");
        Com<ID2D1Bitmap1> staging;
        const D2D1_BITMAP_PROPERTIES1 properties{
            {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
            96, 96, D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, nullptr};
        check(context->CreateBitmap(
            {1, 1}, nullptr, 0, &properties, staging.put()), "Create paint-test staging bitmap");
        const D2D1_RECT_U source{x, 0, x + 1, 1};
        check(staging->CopyFromBitmap(nullptr, bitmap, &source), "Copy synthetic image pixel");
        D2D1_MAPPED_RECT mapped{};
        check(staging->Map(D2D1_MAP_OPTIONS_READ, &mapped), "Map synthetic image pixel");
        struct Unmap {
            ID2D1Bitmap1* bitmap;
            ~Unmap() {
                bitmap->Unmap();
            }
        } unmap{staging.get()};
        return {mapped.bits[0], mapped.bits[1], mapped.bits[2], mapped.bits[3]};
    }
};

static_assert(!std::is_copy_constructible_v<PaintClip>);
static_assert(!std::is_copy_constructible_v<PaintOpacity>);
static_assert(!std::is_copy_constructible_v<PaintSurface>);

void scope_and_blend_test() {
    WarpFixture fixture;
    Com<ID2D1Bitmap1> canvas;
    Com<ID2D1SolidColorBrush> brush;
    const D2D1_BITMAP_PROPERTIES1 properties{
        {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
        96, 96, D2D1_BITMAP_OPTIONS_TARGET, nullptr};
    check(fixture.context->CreateBitmap(
        {4, 2}, nullptr, 0, &properties, canvas.put()), "Create synthetic paint canvas");
    check(fixture.context->CreateSolidColorBrush(
        {1, 1, 1, 1}, brush.put()), "Create synthetic paint brush");
    fixture.context->SetDpi(96, 96);
    fixture.context->SetTarget(canvas.get());
    PaintSurface surface(*fixture.context.get(), *brush.get());

    fixture.context->BeginDraw();
    fixture.context->Clear({0, 0, 0, 0});
    surface.fill({0, 0, 4, 2}, {255, 0, 0, 128}, 1, true);
    surface.fill_rounded({0, 0, 2, 2}, 0, {0, 0, 255, 128}, true);
    require(fixture.context->GetPrimitiveBlend() == D2D1_PRIMITIVE_BLEND_SOURCE_OVER,
            "Replacement fill leaked COPY blend");
    surface.fill_rounded({0, 0, 2, 2}, 0, {0, 255, 0, 128}, false);
    check(fixture.context->EndDraw(), "Finish synthetic blend paint");
    fixture.context->SetTarget(nullptr);
    const auto blended = fixture.pixel(canvas.get(), 0);
    require(blended[0] >= 63 && blended[0] <= 65 && blended[1] == 128 &&
            blended[2] == 0 && blended[3] >= 191 && blended[3] <= 193,
            "Rounded replacement accumulated background alpha or failed to restore source-over");
    require(fixture.pixel(canvas.get(), 3) == std::array<std::uint8_t, 4>{0, 0, 128, 128},
            "Rectangle replacement alpha changed");

    fixture.context->SetTarget(canvas.get());
    fixture.context->BeginDraw();
    fixture.context->Clear({0, 0, 0, 0});
    bool unwound = false;
    try {
        auto clip = surface.clip({0, 0, 2, 2});
        auto opacity = surface.opacity({0, 0, 4, 2}, .25f);
        surface.fill({0, 0, 4, 2}, {255, 0, 0, 255});
        // Test-local throw only: no production callback or renderer fault switch.
        throw std::runtime_error("Exercise paint scope unwind");
    } catch (const std::runtime_error&) {
        unwound = true;
    }
    require(unwound, "Paint scope exception was not observed");
    surface.fill({2, 0, 2, 2}, {0, 255, 0, 255});
    check(fixture.context->EndDraw(), "Finish paint after scope unwind");
    fixture.context->SetTarget(nullptr);
    const auto partial = fixture.pixel(canvas.get(), 0);
    require(partial[2] >= 63 && partial[2] <= 65 && partial[3] == partial[2],
            "Opacity scope discarded partial drawing during unwind");
    require(fixture.pixel(canvas.get(), 3) == std::array<std::uint8_t, 4>{0, 255, 0, 255},
            "Exception leaked paint clip or opacity into later drawing");
    Com<ID2D1SolidColorBrush> replacement;
    check(fixture.context->CreateSolidColorBrush(
        {1, 0, 0, 1}, replacement.put()), "Create synthetic replacement background");
    PaintSurface replacement_surface(
        *fixture.context.get(), *brush.get(), replacement.get());
    fixture.context->SetTarget(canvas.get());
    fixture.context->BeginDraw();
    fixture.context->Clear({0, 0, 0, 0});
    replacement_surface.fill({0, 0, 2, 2}, {0, 0, 255, 128}, 1, true);
    replacement_surface.fill_rounded({2, 0, 2, 2}, 0, {0, 0, 255, 128}, true);
    check(fixture.context->EndDraw(), "Finish synthetic replacement-brush paint");
    fixture.context->SetTarget(nullptr);
    const auto replaced = fixture.pixel(canvas.get(), 0);
    require(replaced[0] == 128 && replaced[1] == 0 &&
            replaced[2] >= 126 && replaced[2] <= 128 && replaced[3] == 255,
            "Replacement brush was skipped or covered using COPY instead of source-over");
    require(fixture.pixel(canvas.get(), 3) == replaced,
            "Rectangle and rounded replacement-brush behavior diverged");
}

} // namespace

int main() {
    try {
        scope_and_blend_test();
        std::cout << "PASS native paint surface: replacement alpha, blend restoration, scope unwind\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL paint surface: " << error.what() << '\n';
        return 1;
    }
}
