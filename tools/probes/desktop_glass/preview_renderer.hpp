#pragma once
#include <windows.h>
#include <d3d11.h>
#include <memory>
#include <string_view>
namespace stormtty::glass_probe {
class PreviewRenderer final {
public:
    explicit PreviewRenderer(HWND window);
    ~PreviewRenderer();
    ID3D11Device* device() const noexcept;
    void draw(ID3D11Texture2D* texture, std::wstring_view status);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
