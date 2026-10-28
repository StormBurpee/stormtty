#include "stormtty/renderer.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <limits>

namespace stormtty {
namespace {
template<class T> struct ComOwner {
    T* value{};
    ~ComOwner() { if (value) value->Release(); }
    T* operator->() const noexcept { return value; }
    T** put() noexcept { return &value; }
};
struct Apartment {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}
} // namespace

std::optional<DecodedPng> decode_windows_png(std::span<const std::uint8_t> bytes,
                                            std::size_t max_decoded_bytes) {
    // The header is checked before COM decoder initialization as well as before
    // allocating the destination. No file, URL, shell, or registered non-PNG
    // decoder can be selected by terminal-controlled input.
    constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    constexpr std::size_t max_encoded = 64u * 1024u * 1024u;
    try {
        if (bytes.size() < 33 || bytes.size() > max_encoded ||
            !std::equal(signature.begin(), signature.end(), bytes.begin()) ||
            be32(bytes.data() + 8) != 13 ||
            bytes[12] != 'I' || bytes[13] != 'H' || bytes[14] != 'D' || bytes[15] != 'R')
            return std::nullopt;
        const auto width = be32(bytes.data() + 16), height = be32(bytes.data() + 20);
        const auto decoded = std::uint64_t(width) * height * 4;
        if (!width || !height || width > 8192 || height > 8192 ||
            decoded > max_decoded_bytes || decoded > std::numeric_limits<UINT>::max())
            return std::nullopt;
        Apartment apartment;
        if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE) return std::nullopt;
        ComOwner<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    __uuidof(IWICImagingFactory),
                                    reinterpret_cast<void**>(factory.put())))) return std::nullopt;
        ComOwner<IWICStream> stream;
        if (FAILED(factory->CreateStream(stream.put())) ||
            FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                                 static_cast<DWORD>(bytes.size())))) return std::nullopt;
        ComOwner<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoder(GUID_ContainerFormatPng, nullptr, decoder.put())) ||
            FAILED(decoder->Initialize(stream.value, WICDecodeMetadataCacheOnDemand))) return std::nullopt;
        ComOwner<IWICBitmapFrameDecode> frame;
        UINT actual_width{}, actual_height{};
        if (FAILED(decoder->GetFrame(0, frame.put())) ||
            FAILED(frame->GetSize(&actual_width, &actual_height)) ||
            actual_width != width || actual_height != height) return std::nullopt;
        ComOwner<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(converter.put())) ||
            FAILED(converter->Initialize(frame.value, GUID_WICPixelFormat32bppRGBA,
                                         WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom))) return std::nullopt;
        DecodedPng result{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(decoded))};
        if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(decoded),
                                         result.rgba.data()))) return std::nullopt;
        return result;
    } catch (...) {
        return std::nullopt; // Never unwind through the VT C decoder callback.
    }
}
void install_windows_png_decoder() { install_png_decoder(&decode_windows_png); }
} // namespace stormtty
