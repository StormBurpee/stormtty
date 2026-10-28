#include "stormtty/renderer.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <dcomp.h>
#include "smoke_teardown_trace.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace stormtty {
namespace {
template<class T> class Com {
public:
    Com() = default;
    explicit Com(T* p, bool add_ref = false) : p_(p) { if (add_ref && p_) p_->AddRef(); }
    ~Com() { reset(); }
    Com(const Com& o) : Com(o.p_, true) {}
    Com& operator=(const Com& o) { if (this != &o) { Com copy(o); swap(copy); } return *this; }
    Com(Com&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    Com& operator=(Com&& o) noexcept { if (this != &o) { reset(); p_ = std::exchange(o.p_, nullptr); } return *this; }
    const char* teardown_label{}; // Enabled only at explicit smoke destruction.
    void reset() noexcept {
        if (auto* p = std::exchange(p_, nullptr)) {
            windows::smoke_teardown_trace(teardown_label!=nullptr,teardown_label," begin");
            p->Release();
            windows::smoke_teardown_trace(teardown_label!=nullptr,teardown_label," end");
        }
    }
    void swap(Com& o) noexcept { std::swap(p_, o.p_); }
    T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    T** put() noexcept { reset(); return &p_; }
    template<class U> HRESULT as(Com<U>& out) const noexcept {
        return p_ ? p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out.put())) : E_POINTER;
    }
private:
    T* p_{};
};
struct Apartment {
    HRESULT hr{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    bool smoke{};
    ~Apartment() {
        windows::smoke_teardown_trace(smoke,"renderer apartment unwind begin");
        if (SUCCEEDED(hr)) CoUninitialize();
        windows::smoke_teardown_trace(smoke,"renderer apartment unwind end");
    }
};
struct NativeHandle {
    HANDLE value{};
    ~NativeHandle() { reset(); }
    void reset(HANDLE v = nullptr) noexcept { if (value) CloseHandle(value); value = v; }
};
class GraphicsError : public std::runtime_error {
public:
    HRESULT result;
    GraphicsError(HRESULT hr, const char* action) : std::runtime_error(message(hr, action)), result(hr) {}
private:
    static std::string message(HRESULT hr, const char* action) {
        char code[16]{}; std::snprintf(code, sizeof code, "0x%08lx", static_cast<unsigned long>(hr));
        return std::string(action) + " failed (" + code + ")";
    }
};
void check(HRESULT hr, const char* action) { if (FAILED(hr)) throw GraphicsError(hr, action); }
HRESULT win32_error() noexcept {
    const auto error=GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}
LONG_PTR extended_style(HWND window) {
    SetLastError(ERROR_SUCCESS);
    const auto style=GetWindowLongPtrW(window,GWL_EXSTYLE);
    const auto error=GetLastError();
    if(!style && error) check(HRESULT_FROM_WIN32(error),"Read renderer host extended style");
    return style;
}
// NOREDIRECTIONBITMAP is a creation-time choice, not a mutable style. Keep the
// shell HWND stable and give opaque fallback its own ordinary presentation HWND.
// This child is never an input/focus owner; all chrome and modals remain pixels
// in the same renderer scene and their input stays with the parent shell.
class OpaquePresentationWindow {
public:
    ~OpaquePresentationWindow() { reset(); }
    OpaquePresentationWindow()=default;
    OpaquePresentationWindow(const OpaquePresentationWindow&)=delete;
    OpaquePresentationWindow& operator=(const OpaquePresentationWindow&)=delete;
    HWND get() const noexcept { return window_; }
    void reset() noexcept {
        if(const auto window=std::exchange(window_,nullptr); window && IsWindow(window)) DestroyWindow(window);
    }
    void create(HWND parent, UINT width, UINT height) {
        constexpr const wchar_t* class_name=L"StormTTY.Renderer.OpaqueSurface.v1";
        static const bool registered=[] {
            WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);
            wc.lpszClassName=L"StormTTY.Renderer.OpaqueSurface.v1";wc.lpfnWndProc=dispatch;
            if(!RegisterClassW(&wc)) {
                const auto error=GetLastError();
                WNDCLASSW existing{};
                if(error!=ERROR_CLASS_ALREADY_EXISTS || !GetClassInfoW(wc.hInstance,wc.lpszClassName,&existing) ||
                   existing.lpfnWndProc!=dispatch)
                    check(error ? HRESULT_FROM_WIN32(error) : E_FAIL,"Register opaque presentation window");
            }
            return true;
        }();
        (void)registered;
        window_=CreateWindowExW(WS_EX_NOPARENTNOTIFY,class_name,L"",WS_CHILD|WS_VISIBLE|WS_DISABLED,
            0,0,static_cast<int>(width),static_cast<int>(height),parent,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!window_) check(win32_error(),"Create opaque presentation child");
        if(extended_style(window_) & WS_EX_NOREDIRECTIONBITMAP)
            throw std::runtime_error("Opaque presentation child unexpectedly lacks redirection");
    }
    void resize(UINT width, UINT height) {
        if(window_ && !SetWindowPos(window_,HWND_TOP,0,0,static_cast<int>(width),static_cast<int>(height),
            SWP_NOACTIVATE|SWP_NOOWNERZORDER))
            check(win32_error(),"Resize opaque presentation child");
    }
private:
    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        switch(message) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATEANDEAT;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT paint{};BeginPaint(window,&paint);EndPaint(window,&paint);return 0; }
        default: return DefWindowProcW(window,message,wparam,lparam);
        }
    }
    HWND window_{};
};
bool device_lost(HRESULT hr) noexcept {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}
std::wstring utf16(std::string_view text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("UTF-8 text exceeds platform length");
    const auto count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return L"\uFFFD";
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
// Resolve only beside this executable, never relative to the terminal's CWD,
// PATH, a user-controlled shell title, or the system font installation directory.
std::wstring bundled_font_directory() {
    constexpr std::size_t maximum_path = 32768;
    std::wstring path(256,L'\0');
    for (;;) {
        const auto length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
        if(!length) return {};
        if(length<path.size()) {path.resize(length);break;}
        if(path.size()==maximum_path) return {};
        path.resize(std::min(maximum_path,path.size()*2));
    }
    const auto slash=path.find_last_of(L"\\/");
    if(slash==std::wstring::npos) return {};
    path.resize(slash+1);
    path+=L"assets\\fonts\\";
    if(path.size()+64>=maximum_path) return {};
    // Extended absolute paths avoid dependence on a process manifest's long-path
    // setting. Keep existing extended paths and short paths unchanged.
    if(path.size()+64>=MAX_PATH && path.rfind(L"\\\\?\\",0)!=0) {
        if(path.rfind(L"\\\\",0)==0) path=L"\\\\?\\UNC\\"+path.substr(2);
        else path=L"\\\\?\\"+path;
    }
    return path;
}
Com<IDWriteFontCollection> load_bundled_fonts(IDWriteFactory* factory) noexcept {
    try {
        // Factory5 is runtime-probed. AddFontFile imports all variable-font
        // instances, including real weights, rather than just a default face.
        Com<IDWriteFactory5> factory5;
        if(FAILED(factory->QueryInterface(__uuidof(IDWriteFactory5),reinterpret_cast<void**>(factory5.put())))) return {};
        const auto directory=bundled_font_directory();
        if(directory.empty()) return {};
        Com<IDWriteFontSetBuilder1> builder;
        if(FAILED(factory5->CreateFontSetBuilder(builder.put()))) return {};
        constexpr std::array<const wchar_t*,2> filenames{L"CascadiaMono.ttf",L"CascadiaMonoItalic.ttf"};
        for(const auto* filename:filenames) {
            const auto path=directory+filename;
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attributes) ||
               (attributes.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || attributes.nFileSizeHigh ||
               !attributes.nFileSizeLow || attributes.nFileSizeLow>8u*1024u*1024u) return {};
            Com<IDWriteFontFile> file;
            if(FAILED(factory->CreateFontFileReference(path.c_str(),&attributes.ftLastWriteTime,file.put()))) return {};
            BOOL supported{};DWRITE_FONT_FILE_TYPE file_type{};DWRITE_FONT_FACE_TYPE face_type{};UINT32 faces{};
            if(FAILED(file->Analyze(&supported,&file_type,&face_type,&faces)) || !supported || !faces || faces>64 ||
               (file_type!=DWRITE_FONT_FILE_TYPE_TRUETYPE && file_type!=DWRITE_FONT_FILE_TYPE_CFF)) return {};
            if(FAILED(builder->AddFontFile(file.get()))) return {};
        }
        Com<IDWriteFontSet> set;
        if(FAILED(builder->CreateFontSet(set.put())) || !set->GetFontCount() || set->GetFontCount()>256) return {};
        Com<IDWriteFontCollection1> collection;
        if(FAILED(factory5->CreateFontCollectionFromFontSet(set.get(),collection.put()))) return {};
        UINT32 index{};BOOL exists{};
        if(FAILED(collection->FindFamilyName(L"Cascadia Mono",&index,&exists)) || !exists) return {};
        Com<IDWriteFontFamily1> family;
        if(FAILED(collection->GetFontFamily(index,family.put()))) return {};
        for(auto style:{DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STYLE_ITALIC}) {
            Com<IDWriteFont> face;
            if(FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STRETCH_NORMAL,style,face.put())) ||
               face->GetStyle()!=style || face->GetSimulations()!=DWRITE_FONT_SIMULATIONS_NONE) return {};
        }
        Com<IDWriteFontCollection> result;
        if(FAILED(collection.as(result))) return {};
        return result;
    } catch (...) {
        // Missing, oversized, malformed or unsupported optional assets use the
        // existing system-font fallback. No registration, downloads or retries.
        return {};
    }
}
D2D1_COLOR_F color(Color c, float opacity = 1) noexcept {
    return {float(c.r) / 255, float(c.g) / 255, float(c.b) / 255, float(c.a) / 255 * opacity};
}
D2D1_RECT_F rect(PixelRect r) noexcept { return {r.x, r.y, r.x + r.width, r.y + r.height}; }
PixelRect intersect(PixelRect a, PixelRect b) noexcept {
    const auto x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.0f, std::min(a.x + a.width, b.x + b.width) - x),
                  std::max(0.0f, std::min(a.y + a.height, b.y + b.height) - y)};
}
bool nonempty(PixelRect r) noexcept {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
           std::isfinite(r.height) && r.width > 0 && r.height > 0;
}
bool overlaps(PixelRect a, PixelRect b) noexcept { return nonempty(intersect(a, b)); }
struct Clip {
    ID2D1DeviceContext* dc;
    Clip(ID2D1DeviceContext* context, PixelRect r) : dc(context) {
        dc->PushAxisAlignedClip(rect(r), D2D1_ANTIALIAS_MODE_ALIASED);
    }
    ~Clip() { dc->PopAxisAlignedClip(); }
};
struct OpacityLayer {
    ID2D1DeviceContext* dc;bool active;
    OpacityLayer(ID2D1DeviceContext* context,PixelRect bounds,float opacity):dc(context),active(opacity<1) {
        if(active) {
            D2D1_LAYER_PARAMETERS1 params{};
            params.contentBounds=rect(bounds);params.maskAntialiasMode=D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
            params.maskTransform._11=1;params.maskTransform._22=1;params.opacity=opacity;
            params.layerOptions=D2D1_LAYER_OPTIONS1_NONE;
            dc->PushLayer(&params,nullptr);
        }
    }
    ~OpacityLayer(){if(active)dc->PopLayer();}
};
PixelRect chrome_bounds(const ChromePrimitive& p) noexcept {
    if(!std::isfinite(p.stroke_width) || !std::isfinite(p.radius)) return {};
    const auto pad=std::max(1.0f,p.stroke_width*.5f+1);
    if(p.kind==ChromePrimitiveKind::line)
        return {std::min(p.rect.x,p.rect.x+p.rect.width)-pad,std::min(p.rect.y,p.rect.y+p.rect.height)-pad,
                std::abs(p.rect.width)+pad*2,std::abs(p.rect.height)+pad*2};
    if(!nonempty(p.rect))return {};
    return {p.rect.x-pad,p.rect.y-pad,p.rect.width+pad*2,p.rect.height+pad*2};
}
struct CellMap { UINT32 column{}, end{}; };
struct GlyphCluster {
    Com<IDWriteFontFace> face;
    std::vector<UINT16> glyphs;
    std::vector<FLOAT> advances;
    std::vector<DWRITE_GLYPH_OFFSET> offsets;
    UINT32 column{}, end{}, bidi{};
    float size{}, baseline{}, x{}, natural_left{}, advance_total{};
    bool sideways{};
};
struct ShapedRow {
    std::vector<GlyphCluster> clusters;
    std::size_t bytes{};
    std::uint64_t touched{};
};
// DirectWrite performs contextual shaping AND system font fallback for the whole
// row. We capture its many-to-many clusters, not Unicode scalars. The subsequent
// placement keeps VT columns authoritative rather than accepting paragraph bidi
// reordering or proportional layout widths. Full terminal bidi is not supported.
class ClusterCapture final : public IDWriteTextRenderer {
public:
    explicit ClusterCapture(const std::vector<CellMap>& mapping) : mapping_(mapping) {}
    std::vector<GlyphCluster> clusters;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteTextRenderer) ||
            iid == __uuidof(IDWritePixelSnapping)) { *out = static_cast<IDWriteTextRenderer*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { const auto count = --refs_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) noexcept override {
        if (!disabled) return E_POINTER; *disabled = TRUE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* matrix) noexcept override {
        if (!matrix) return E_POINTER; *matrix = {1, 0, 0, 1, 0, 0}; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) noexcept override {
        if (!pixels) return E_POINTER; *pixels = 1; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT origin_x, FLOAT baseline,
        DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
        const DWRITE_GLYPH_RUN_DESCRIPTION* description, IUnknown*) noexcept override {
        try {
            if (!run || !description || !description->clusterMap || !run->glyphIndices ||
                !run->glyphAdvances || !run->fontFace || run->glyphCount == 0) return S_OK;
            const auto length = description->stringLength;
            if (std::uint64_t(description->textPosition) + length > mapping_.size()) return E_INVALIDARG;
            // Build glyph-cluster boundaries from DirectWrite's UTF-16 cluster map.
            // Sorting glyph offsets handles both LTR and RTL runs without reversing
            // terminal cell order or assuming one character == one glyph.
            std::vector<UINT32> boundaries;
            boundaries.reserve(length + 1);
            std::vector<CellMap> cells(run->glyphCount, {std::numeric_limits<UINT32>::max(), 0});
            for (UINT32 i = 0; i < length; ++i) {
                const auto glyph = description->clusterMap[i];
                if (glyph >= run->glyphCount) return E_INVALIDARG;
                boundaries.push_back(glyph);
                const auto& mapped = mapping_[description->textPosition + i];
                cells[glyph].column = std::min(cells[glyph].column, mapped.column);
                cells[glyph].end = std::max(cells[glyph].end, mapped.end);
            }
            boundaries.push_back(run->glyphCount);
            std::sort(boundaries.begin(), boundaries.end());
            boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
            std::vector<float> positions(run->glyphCount + 1);
            for (UINT32 i = 0; i < run->glyphCount; ++i) positions[i + 1] = positions[i] + run->glyphAdvances[i];
            for (std::size_t b = 0; b + 1 < boundaries.size(); ++b) {
                const auto first = boundaries[b], last = boundaries[b + 1];
                const auto column = cells[first].column, end = cells[first].end;
                if (column == std::numeric_limits<UINT32>::max() || end <= column) continue;
                GlyphCluster cluster;
                cluster.face = Com<IDWriteFontFace>(run->fontFace, true);
                cluster.glyphs.assign(run->glyphIndices + first, run->glyphIndices + last);
                cluster.advances.assign(run->glyphAdvances + first, run->glyphAdvances + last);
                if (run->glyphOffsets) cluster.offsets.assign(run->glyphOffsets + first, run->glyphOffsets + last);
                else cluster.offsets.resize(last - first);
                cluster.column = column; cluster.end = end; cluster.bidi = run->bidiLevel;
                cluster.size = run->fontEmSize; cluster.baseline = baseline;
                cluster.sideways = run->isSideways != FALSE;
                cluster.advance_total = positions[last] - positions[first];
                cluster.natural_left = (run->bidiLevel & 1) ? origin_x - positions[last] : origin_x + positions[first];
                clusters.push_back(std::move(cluster));
            }
            return S_OK;
        } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
          catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) noexcept override { return E_NOTIMPL; }
private:
    std::atomic<ULONG> refs_{1};
    const std::vector<CellMap>& mapping_;
};
struct RowKey {
    std::uint64_t pane{}, row0{}, row1{}, generation{}, font{};
    std::uint32_t columns{}, cell_width{}, cell_height{};
    bool operator==(const RowKey&) const = default;
};
struct RowHash {
    std::size_t operator()(const RowKey& key) const noexcept {
        std::size_t hash = 0;
        const std::array<std::uint64_t, 8> values{key.pane,key.row0,key.row1,key.generation,key.font,
                                               key.columns,key.cell_width,key.cell_height};
        for (auto value : values) hash ^= std::hash<std::uint64_t>{}(value) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        return hash;
    }
};
struct ImageKey {
    std::uint64_t pane{}, generation{};
    std::uint32_t id{}, width{}, height{};
    PixelFormat format{};
    bool operator==(const ImageKey&) const = default;
};
struct ImageHash {
    std::size_t operator()(const ImageKey& k) const noexcept {
        return RowHash{}({k.pane,k.generation,k.id,k.width,k.height,static_cast<std::uint32_t>(k.format),0,0});
    }
};
struct CachedImage { Com<ID2D1Bitmap1> bitmap; std::size_t bytes{}; std::uint64_t touched{}; };
bool same_cursor(const FrameCursor& a, const FrameCursor& b) noexcept {
    return a.column == b.column && a.row == b.row && a.visible == b.visible &&
           a.shape == b.shape && a.color == b.color && a.wide_tail == b.wide_tail;
}
constexpr Color selection_background{58,79,91,255};
constexpr Color selection_foreground{236,238,242,255};
} // namespace

struct Renderer::Impl {
    Apartment apartment;
    HWND window{};
    OpaquePresentationWindow opaque_window;
    RendererOptions options;
    CellMetrics cell;
    RendererStats counters;
    UINT width{}, height{};
    float dpi{96}, em_size{14};
    std::uint64_t font_generation{1}, tick{};
    bool invalid{true}, pending_present{};
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> immediate;
    Com<IDXGISwapChain1> swapchain;
    Com<IDXGISwapChain2> swapchain2;
    NativeHandle latency;
    Com<IDCompositionDevice> composition;
    Com<IDCompositionTarget> composition_target;
    Com<IDCompositionVisual> composition_visual;
    Com<ID2D1Factory1> d2d_factory;
    Com<ID2D1Device> d2d_device;
    Com<ID2D1DeviceContext> dc;
    Com<ID2D1DeviceContext4> dc4;
    Com<IDWriteFactory> write_factory;
    Com<IDWriteFactory4> write4;
    Com<IDWriteTextFormat> terminal_format;
    Com<IDWriteTypography> typography;
    Com<IDWriteFontCollection> bundled_fonts;
    bool bundled_fonts_attempted{};
    Com<ID3D11Texture2D> canvas_texture, alpha_staging;
    UINT alpha_width{}, alpha_height{};
    RendererAlphaProbe alpha_metadata{};
    Com<ID2D1Bitmap1> canvas;
    Com<ID2D1SolidColorBrush> brush;
    std::unordered_map<RowKey, ShapedRow, RowHash> shapes;
    std::unordered_map<ImageKey, CachedImage, ImageHash> images;
    WindowScene previous;

    Impl(void* hwnd, RendererOptions opts) : window(static_cast<HWND>(hwnd)), options(std::move(opts)) {
        if (FAILED(apartment.hr) && apartment.hr != RPC_E_CHANGED_MODE) check(apartment.hr, "COM initialization");
        validate_font(options.font);
        if (!options.maximum_image_dimension || options.maximum_image_dimension > 8192 ||
            options.shape_cache_bytes > 256u * 1024u * 1024u || options.image_cache_bytes > 512u * 1024u * 1024u)
            throw std::invalid_argument("Renderer cache or image dimensions exceed safety limits");
        if (!std::isfinite(options.background_opacity)) throw std::invalid_argument("Invalid background opacity");
        options.background_opacity = std::clamp(options.background_opacity, 0.0f, 1.0f);
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown**>(write_factory.put())), "DWriteCreateFactory");
        write_factory.as(write4); // Runtime probe, not a hard requirement.
        D2D1_FACTORY_OPTIONS factory_options{};
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                &factory_options, reinterpret_cast<void**>(d2d_factory.put())), "D2D1CreateFactory");
        create_device();
        measure_font();
    }
    bool tracing_teardown{};
    ~Impl() {
        tracing_teardown=options.smoke_teardown_diagnostics;
        apartment.smoke=tracing_teardown;
        if (tracing_teardown) {
#define TRACE_RELEASE(member) member.teardown_label="renderer " #member " Release"
            TRACE_RELEASE(device); TRACE_RELEASE(immediate); TRACE_RELEASE(swapchain);
            TRACE_RELEASE(swapchain2); TRACE_RELEASE(composition); TRACE_RELEASE(composition_target);
            TRACE_RELEASE(composition_visual); TRACE_RELEASE(d2d_factory); TRACE_RELEASE(d2d_device);
            TRACE_RELEASE(dc); TRACE_RELEASE(dc4); TRACE_RELEASE(write_factory); TRACE_RELEASE(write4);
            TRACE_RELEASE(terminal_format); TRACE_RELEASE(typography); TRACE_RELEASE(bundled_fonts);
            TRACE_RELEASE(canvas_texture); TRACE_RELEASE(alpha_staging); TRACE_RELEASE(canvas); TRACE_RELEASE(brush);
#undef TRACE_RELEASE
        }
        windows::smoke_teardown_trace(tracing_teardown,"renderer release_gpu begin");
        release_gpu();
        windows::smoke_teardown_trace(tracing_teardown,"renderer release_gpu end; implicit cache/font members begin");
    }
    void clear_and_flush() noexcept {
        if (!immediate) return;
        windows::smoke_teardown_trace(tracing_teardown,"renderer ClearState begin");
        immediate->ClearState();
        windows::smoke_teardown_trace(tracing_teardown,"renderer ClearState end; Flush begin");
        immediate->Flush();
        windows::smoke_teardown_trace(tracing_teardown,"renderer Flush end");
    }
    static void validate_font(const FontOptions& font) {
        if (font.family.empty() || font.family.size() > 256 || font.locale.size() > 85 ||
            !std::isfinite(font.size_dip) || font.size_dip < 4 || font.size_dip > 128)
            throw std::invalid_argument("Invalid terminal font");
    }
    void release_swapchain() noexcept {
        composition_visual.reset(); composition_target.reset(); composition.reset();
        windows::smoke_teardown_trace(tracing_teardown,"renderer latency handle close begin");
        latency.reset();
        windows::smoke_teardown_trace(tracing_teardown,"renderer latency handle close end");
        swapchain2.reset(); swapchain.reset();
        clear_and_flush();
        windows::smoke_teardown_trace(tracing_teardown,"renderer opaque HWND destruction begin");
        opaque_window.reset();
        windows::smoke_teardown_trace(tracing_teardown,"renderer opaque HWND destruction end");
        counters.composition = false;
    }
    void release_gpu() noexcept {
        windows::smoke_teardown_trace(tracing_teardown,"renderer SetTarget null begin");
        if (dc) dc->SetTarget(nullptr);
        windows::smoke_teardown_trace(tracing_teardown,"renderer SetTarget null end");
        alpha_staging.reset(); canvas.reset(); canvas_texture.reset(); brush.reset();
        windows::smoke_teardown_trace(tracing_teardown,"renderer image cache clear begin");
        images.clear();
        windows::smoke_teardown_trace(tracing_teardown,"renderer image cache clear end");
        counters.image_cache_bytes = 0;
        // Drop D2D's device-dependent owners before the single deferred-release
        // drain in release_swapchain(). Keep that helper's ClearState/Flush for
        // standalone swapchain replacement too; do not drain twice around D2D
        // destruction. This ordering is a teardown candidate, not proof that a
        // previously observed WARP ClearState stall has been resolved.
        dc4.reset(); dc.reset(); d2d_device.reset();
        release_swapchain();
        immediate.reset(); device.reset();
    }
    void create_device() {
        D3D_FEATURE_LEVEL level{};
        constexpr std::array<D3D_FEATURE_LEVEL, 3> levels{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        auto create = [&](D3D_DRIVER_TYPE type) {
            return D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
                                    device.put(), &level, immediate.put());
        };
        auto hr = options.force_warp ? E_FAIL : create(D3D_DRIVER_TYPE_HARDWARE);
        counters.warp = FAILED(hr);
        if (FAILED(hr)) { device.reset(); immediate.reset(); hr = create(D3D_DRIVER_TYPE_WARP); }
        check(hr, "D3D11CreateDevice hardware/WARP");
        Com<IDXGIDevice> dxgi_device;
        check(device.as(dxgi_device), "Query IDXGIDevice");
        check(d2d_factory->CreateDevice(dxgi_device.get(), d2d_device.put()), "D2D CreateDevice");
        // D2D's internal caches are separate from our explicit image/shape caches.
        // This is a soft purge threshold (D2D may exceed it within one frame).
        d2d_device->SetMaximumTextureMemory(32u*1024u*1024u);
        Com<ID2D1Device4> d2d4;
        if(SUCCEEDED(d2d_device.as(d2d4))) d2d4->SetMaximumColorGlyphCacheMemory(16u*1024u*1024u);
        check(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.put()), "D2D CreateDeviceContext");
        dc.as(dc4);
        counters.color_fonts = bool(dc4) && bool(write4);
        dc->SetDpi(96, 96); // All geometry and fonts are already physical pixels.
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        check(dc->CreateSolidColorBrush(D2D1_COLOR_F{1,1,1,1}, brush.put()), "Create brush");
        if (width && height) create_surfaces();
    }
    void create_surfaces() {
        if (!width || !height) return;
        try {
        if (window && !swapchain) {
            Com<IDXGIDevice> dxgi_device; check(device.as(dxgi_device), "Query DXGI device");
            Com<IDXGIAdapter> adapter; check(dxgi_device->GetAdapter(adapter.put()), "Get adapter");
            Com<IDXGIFactory2> factory;
            check(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.put())), "Get DXGI factory");
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = width; desc.Height = height; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2; desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            counters.composition = false;
            const bool no_redirection=(extended_style(window) & WS_EX_NOREDIRECTIONBITMAP)!=0;
            // Never compose translucent pixels above an ordinary backing surface.
            // An ordinary caller-provided HWND safely selects the opaque path.
            if (options.composition && no_redirection) {
                desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
                const char* stage = "DCompositionCreateDevice";
                auto hr = DCompositionCreateDevice(dxgi_device.get(), __uuidof(IDCompositionDevice),
                                                  reinterpret_cast<void**>(composition.put()));
                auto step = [&](const char* action, auto&& operation) {
                    if (SUCCEEDED(hr)) { stage = action; hr = operation(); }
                };
                step("CreateSwapChainForComposition", [&] { return factory->CreateSwapChainForComposition(device.get(), &desc, nullptr, swapchain.put()); });
                step("CreateTargetForHwnd", [&] { return composition->CreateTargetForHwnd(window, TRUE, composition_target.put()); });
                step("CreateVisual", [&] { return composition->CreateVisual(composition_visual.put()); });
                step("SetContent", [&] { return composition_visual->SetContent(swapchain.get()); });
                step("SetRoot", [&] { return composition_target->SetRoot(composition_visual.get()); });
                step("Commit", [&] { return composition->Commit(); });
                counters.composition = SUCCEEDED(hr);
                if (!counters.composition) {
                    SetLastError(ERROR_SUCCESS);
                    const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
                    const auto style_error = GetLastError();
                    char diagnostic[320]{};
                    std::snprintf(diagnostic, sizeof diagnostic,
                        "StormTTY renderer: DComp fallback stage=%s hr=0x%08lx warp=%u exstyle=0x%08lx style_error=%lu visible=%u size=%ux%u\n",
                        stage, static_cast<unsigned long>(hr), unsigned(counters.warp),
                        static_cast<unsigned long>(style), static_cast<unsigned long>(style_error),
                        unsigned(IsWindowVisible(window)!=FALSE), width, height);
                    OutputDebugStringA(diagnostic);
                    std::fputs(diagnostic, stderr);
                    release_swapchain();
                }
            }
            if (!swapchain) {
                // A no-redirection host cannot be switched back at runtime. The
                // fallback target is an ordinary disabled child, not that host.
                HWND presentation_window=window;
                if(no_redirection) {
                    opaque_window.create(window,width,height);
                    presentation_window=opaque_window.get();
                }
                desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                check(factory->CreateSwapChainForHwnd(device.get(), presentation_window, &desc, nullptr, nullptr, swapchain.put()), "Create HWND swapchain");
            }
            factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
            check(swapchain.as(swapchain2), "Query swapchain2");
            check(swapchain2->SetMaximumFrameLatency(1), "Set frame latency");
            latency.reset(swapchain2->GetFrameLatencyWaitableObject());
            if (!latency.value) throw std::runtime_error("Swapchain did not provide frame latency handle");
        }
        opaque_window.resize(width,height);
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = width; texture.Height = height; texture.MipLevels = 1; texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM; texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_DEFAULT; texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&texture, nullptr, canvas_texture.put()), "Create persistent canvas");
        Com<IDXGISurface> surface; check(canvas_texture.as(surface), "Query canvas DXGI surface");
        const D2D1_BITMAP_PROPERTIES1 properties{{DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED},96,96,
                                                D2D1_BITMAP_OPTIONS_TARGET,nullptr};
        check(dc->CreateBitmapFromDxgiSurface(surface.get(), &properties, canvas.put()), "Create canvas bitmap");
        dc->SetTarget(canvas.get()); invalid = true; pending_present = false;
        } catch (...) {
            dc->SetTarget(nullptr); canvas.reset(); canvas_texture.reset();
            release_swapchain();
            throw;
        }
    }
    void measure_font() {
        em_size = options.font.size_dip * dpi / 96;
        auto family = utf16(options.font.family);
        const auto locale = utf16(options.font.locale);
        IDWriteFontCollection* chosen_collection=nullptr;
        if(CompareStringOrdinal(family.c_str(),-1,L"Cascadia Mono",-1,TRUE)==CSTR_EQUAL) {
            if(!bundled_fonts_attempted) {
                bundled_fonts_attempted=true;
                bundled_fonts=load_bundled_fonts(write_factory.get());
            }
            // Private Cascadia wins over an arbitrary system-installed version.
            // Other configured font families still resolve in the system collection.
            chosen_collection=bundled_fonts.get();
        }
        if(!chosen_collection) {
            Com<IDWriteFontCollection> fonts;
            check(write_factory->GetSystemFontCollection(fonts.put(),FALSE),"Get system fonts");
            UINT32 family_index{};BOOL exists{};
            check(fonts->FindFamilyName(family.c_str(),&family_index,&exists),"Find terminal font");
            if(!exists) {
                family=L"Consolas";
                check(fonts->FindFamilyName(family.c_str(),&family_index,&exists),"Find fallback mono font");
                if(!exists) family=L"Courier New";
            }
        }
        // A custom base collection does not replace DirectWrite's default system
        // fallback list. Keep that list for CJK, emoji and other absent glyphs.
        check(write_factory->CreateTextFormat(family.c_str(), chosen_collection, DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, em_size, locale.c_str(), terminal_format.put()), "Create terminal format");
        counters.bundled_font=chosen_collection!=nullptr;
        check(terminal_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "Disable text wrapping");
        check(write_factory->CreateTypography(typography.put()),"Create terminal typography");
        const UINT32 enabled=options.font.ligatures?1u:0u;
        for(auto feature:{DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES,DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_LIGATURES,
                          DWRITE_FONT_FEATURE_TAG_DISCRETIONARY_LIGATURES,DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_ALTERNATES})
            check(typography->AddFontFeature({feature,enabled}),"Set optional ligature feature");
        // Required script ligatures must stay enabled even when programming
        // ligatures are disabled; disabling rlig would break script shaping.
        check(typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_REQUIRED_LIGATURES,1}),"Enable required ligatures");
        Com<IDWriteTextLayout> measure;
        check(write_factory->CreateTextLayout(L"M", 1, terminal_format.get(), em_size * 4, em_size * 4, measure.put()), "Measure font");
        DWRITE_TEXT_METRICS metrics{}; check(measure->GetMetrics(&metrics), "Get font metrics");
        DWRITE_LINE_METRICS line{}; UINT32 count{};
        check(measure->GetLineMetrics(&line, 1, &count), "Get line metrics");
        cell.width = static_cast<std::uint16_t>(std::clamp(std::ceil(metrics.widthIncludingTrailingWhitespace), 1.0f, 65535.0f));
        cell.height = static_cast<std::uint16_t>(std::clamp(std::ceil(line.height + dpi / 96), 1.0f, 65535.0f));
        cell.baseline = line.baseline;
        shapes.clear(); counters.shape_cache_bytes = 0; ++font_generation; invalid = true;
    }
    ShapedRow build_shape(const FrameRow& row, UINT columns, UINT cw, UINT ch) {
        constexpr std::size_t maximum_utf16_per_row = 131072;
        constexpr std::size_t maximum_utf16_per_grapheme = 4096;
        struct StyleRange { UINT32 start{}, length{}; bool bold{}, italic{}; };
        std::wstring text;
        std::vector<CellMap> mapping;
        std::vector<StyleRange> styles;
        const auto count = std::min<std::size_t>(columns, row.cells.size());
        for (std::size_t column = 0; column < count; ++column) {
            const auto& source = row.cells[column];
            if (!source.span) continue;
            auto grapheme = source.image_placeholder || source.grapheme.empty() ? std::wstring(L" ") :
                source.grapheme.size() > maximum_utf16_per_grapheme * 4 ? std::wstring(L"\uFFFD") : utf16(source.grapheme);
            if (grapheme.size() > maximum_utf16_per_grapheme ||
                text.size() + grapheme.size() + count - column - 1 > maximum_utf16_per_row)
                grapheme = L"\uFFFD"; // Explicit bounded pathological-sequence fallback.
            const auto start = static_cast<UINT32>(text.size());
            text += grapheme;
            mapping.insert(mapping.end(), grapheme.size(), CellMap{static_cast<UINT32>(column),
                static_cast<UINT32>(std::min<std::size_t>(count, column + source.span))});
            if (!styles.empty() && styles.back().bold == source.style.bold && styles.back().italic == source.style.italic)
                styles.back().length += static_cast<UINT32>(grapheme.size());
            else styles.push_back({start, static_cast<UINT32>(grapheme.size()), source.style.bold, source.style.italic});
        }
        ShapedRow result;
        result.bytes=sizeof(ShapedRow)+sizeof(RowKey)+sizeof(void*)*4;
        if (text.empty()) return result;
        Com<IDWriteTextLayout> layout;
        check(write_factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), terminal_format.get(),
                                               std::max(1.0f, float(columns * cw) * 4), float(ch) * 4, layout.put()), "Shape terminal row");
        check(layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "Set row no-wrap");
        check(layout->SetReadingDirection(DWRITE_READING_DIRECTION_LEFT_TO_RIGHT), "Set VT row direction");
        check(layout->SetTypography(typography.get(),{0,static_cast<UINT32>(text.size())}),"Apply terminal typography");
        check(layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,float(ch),std::min(float(ch),cell.baseline)),
              "Anchor row baseline to cell metrics");
        for (const auto& range : styles) {
            if (range.bold) check(layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, {range.start, range.length}), "Set row weight");
            if (range.italic) check(layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, {range.start, range.length}), "Set row italic");
        }
        Com<ClusterCapture> capture(new ClusterCapture(mapping));
        check(layout->Draw(nullptr, capture.get(), 0, 0), "Capture shaped clusters");
        result.clusters = std::move(capture->clusters);
        // If fallback splits one grapheme into several clusters, preserve their
        // relative natural placement as a group instead of piling scalars onto
        // the same origin. Unsupported emoji may still display multiple glyphs.
        std::unordered_map<std::uint64_t, float> group_left;
        for (const auto& cluster : result.clusters) {
            const auto key = (std::uint64_t(cluster.column) << 32) | cluster.end;
            const auto [entry, inserted] = group_left.emplace(key, cluster.natural_left);
            if (!inserted) entry->second = std::min(entry->second, cluster.natural_left);
        }
        for (auto& cluster : result.clusters) {
            const auto key = (std::uint64_t(cluster.column) << 32) | cluster.end;
            cluster.x = float(cluster.column * cw) + cluster.natural_left - group_left.at(key);
            if (cluster.bidi & 1) cluster.x += cluster.advance_total;
        }
        // Batch adjacent LTR clusters of the same physical font into one glyph
        // run. Padding is assigned only at shaped cluster boundaries, preserving
        // intra-cluster offsets and ligatures. Ordinary rows need one draw per
        // paint/font run, not one Direct2D call or UI object per terminal cell.
        std::vector<GlyphCluster> batches;
        for (auto& cluster : result.clusters) {
            if (!batches.empty()) {
                auto& prior = batches.back();
                if (!(prior.bidi & 1) && prior.bidi == cluster.bidi && prior.end == cluster.column &&
                    prior.face.get() == cluster.face.get() && prior.size == cluster.size &&
                    prior.baseline == cluster.baseline && prior.sideways == cluster.sideways &&
                    !prior.advances.empty()) {
                    const auto natural_end = prior.x + prior.advance_total;
                    prior.advances.back() += cluster.x - natural_end;
                    prior.advance_total = cluster.x - prior.x + cluster.advance_total;
                    prior.glyphs.insert(prior.glyphs.end(),cluster.glyphs.begin(),cluster.glyphs.end());
                    prior.advances.insert(prior.advances.end(),cluster.advances.begin(),cluster.advances.end());
                    prior.offsets.insert(prior.offsets.end(),cluster.offsets.begin(),cluster.offsets.end());
                    prior.end=cluster.end;
                    continue;
                }
            }
            batches.push_back(std::move(cluster));
        }
        result.clusters=std::move(batches);
        result.bytes += result.clusters.capacity()*sizeof(GlyphCluster);
        for (const auto& cluster:result.clusters)
            result.bytes += cluster.glyphs.capacity()*sizeof(UINT16) +
                cluster.advances.capacity()*sizeof(FLOAT) + cluster.offsets.capacity()*sizeof(DWRITE_GLYPH_OFFSET);
        ++counters.shaped_rows;
        return result;
    }
    void trim_shapes(std::size_t incoming) {
        while (!shapes.empty() && counters.shape_cache_bytes + incoming > options.shape_cache_bytes) {
            const auto oldest = std::min_element(shapes.begin(), shapes.end(),
                [](const auto& a, const auto& b) { return a.second.touched < b.second.touched; });
            counters.shape_cache_bytes -= oldest->second.bytes; shapes.erase(oldest);
        }
    }
    const ShapedRow& shaped(const RenderPane& pane, const FrameRow& row, UINT cw, UINT ch, ShapedRow& temporary) {
        const RowKey key{pane.id,row.id.words[0],row.id.words[1],row.generation,font_generation,pane.frame->columns,cw,ch};
        if (auto found = shapes.find(key); found != shapes.end()) {
            ++counters.shape_cache_hits; found->second.touched = tick; return found->second;
        }
        temporary = build_shape(row, pane.frame->columns, cw, ch);
        if (!options.shape_cache_bytes || temporary.bytes > options.shape_cache_bytes) return temporary;
        trim_shapes(temporary.bytes); temporary.touched = tick;
        const auto bytes = temporary.bytes;
        auto [entry, inserted] = shapes.emplace(key, std::move(temporary));
        if (inserted) counters.shape_cache_bytes += bytes;
        return entry->second;
    }
    bool material_enabled() const noexcept {
        // Offscreen composition surfaces preserve alpha so WARP can verify its
        // math. composition=false explicitly tests the opaque fallback there.
        // A real WARP HWND never advertises an accelerated translucent material.
        return options.composition && (!window || (counters.composition && !counters.warp));
    }
    float backdrop_opacity() const noexcept {
        return material_enabled() ? options.background_opacity : 1.0f;
    }
    void fill(PixelRect r, Color c, float opacity = 1, bool replace = false) {
        if (!nonempty(r)) return;
        brush->SetColor(color(c, opacity));
        if (replace) dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        dc->FillRectangle(rect(r), brush.get());
        if (replace) dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    }
    bool selected(const FrameRow& row, std::size_t column) const noexcept {
        return (column < row.cells.size() && row.cells[column].selected) ||
               (row.selection && column >= row.selection->first && column <= row.selection->last);
    }
    Color foreground(const FrameRow& row, std::size_t column) const noexcept {
        if (column >= row.cells.size()) return {236,238,242,255};
        const auto& c = row.cells[column];
        if (selected(row,column)) return selection_foreground;
        auto result = c.style.inverse ? c.background : c.foreground;
        if (c.style.faint) { result.r /= 2; result.g /= 2; result.b /= 2; }
        return result;
    }
    Color background(const FrameRow& row, std::size_t column) const noexcept {
        const auto& c = row.cells[column];
        return selected(row,column) ? selection_background : (c.style.inverse ? c.foreground : c.background);
    }
    void glyph_run(const GlyphCluster& cluster, D2D1_POINT_2F origin, Color fg) {
        if (cluster.glyphs.empty()) return;
        DWRITE_GLYPH_RUN run{};
        run.fontFace = cluster.face.get(); run.fontEmSize = cluster.size;
        run.glyphCount = static_cast<UINT32>(cluster.glyphs.size()); run.glyphIndices = cluster.glyphs.data();
        run.glyphAdvances = cluster.advances.data(); run.glyphOffsets = cluster.offsets.data();
        run.isSideways = cluster.sideways; run.bidiLevel = cluster.bidi;
        brush->SetColor(color(fg));
        if (write4 && dc4) {
            constexpr auto formats = static_cast<DWRITE_GLYPH_IMAGE_FORMATS>(
                DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE | DWRITE_GLYPH_IMAGE_FORMATS_CFF |
                DWRITE_GLYPH_IMAGE_FORMATS_COLR | DWRITE_GLYPH_IMAGE_FORMATS_SVG |
                DWRITE_GLYPH_IMAGE_FORMATS_PNG | DWRITE_GLYPH_IMAGE_FORMATS_JPEG |
                DWRITE_GLYPH_IMAGE_FORMATS_TIFF | DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8);
            Com<IDWriteColorGlyphRunEnumerator1> layers;
            const auto hr = write4->TranslateColorGlyphRun(origin, &run, nullptr, formats,
                DWRITE_MEASURING_MODE_NATURAL, nullptr, 0, layers.put());
            if (SUCCEEDED(hr)) {
                BOOL available{};
                while (SUCCEEDED(layers->MoveNext(&available)) && available) {
                    const DWRITE_COLOR_GLYPH_RUN1* layer{};
                    if (FAILED(layers->GetCurrentRun(&layer)) || !layer) break;
                    const D2D1_POINT_2F position{layer->baselineOriginX,layer->baselineOriginY};
                    switch (layer->glyphImageFormat) {
                    case DWRITE_GLYPH_IMAGE_FORMATS_PNG:
                    case DWRITE_GLYPH_IMAGE_FORMATS_JPEG:
                    case DWRITE_GLYPH_IMAGE_FORMATS_TIFF:
                    case DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8:
                        dc4->DrawColorBitmapGlyphRun(layer->glyphImageFormat, position, &layer->glyphRun,
                            DWRITE_MEASURING_MODE_NATURAL, D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION_DEFAULT);
                        break;
                    case DWRITE_GLYPH_IMAGE_FORMATS_SVG:
                        brush->SetColor(color(fg));
                        dc4->DrawSvgGlyphRun(position, &layer->glyphRun, brush.get(), nullptr, 0, DWRITE_MEASURING_MODE_NATURAL);
                        break;
                    default:
                        brush->SetColor(layer->paletteIndex == 0xffff ? color(fg) : layer->runColor);
                        dc->DrawGlyphRun(position, &layer->glyphRun, brush.get(), DWRITE_MEASURING_MODE_NATURAL);
                        break;
                    }
                }
                return;
            }
            if (device_lost(hr)) check(hr,"Translate color glyphs");
            // Unsupported color font/API uses explicit monochrome fallback.
        }
        brush->SetColor(color(fg));
        dc->DrawGlyphRun(origin, &run, brush.get(), DWRITE_MEASURING_MODE_NATURAL);
    }
    void draw_text(const RenderPane& pane, const FrameRow& row, UINT row_index, UINT cw, UINT ch,
                   std::optional<Color> override_color = std::nullopt) {
        ShapedRow temporary;
        const auto& shape = shaped(pane, row, cw, ch, temporary);
        const auto y = pane.content.y + float(row_index * ch);
        Clip row_clip(dc.get(), intersect(pane.content, {pane.content.x,y,pane.content.width,float(ch)}));
        for (const auto& cluster : shape.clusters) {
            if (cluster.column >= row.cells.size()) continue;
            const auto left = pane.content.x + float(cluster.column * cw);
            const auto right = pane.content.x + float(cluster.end * cw);
            // At most half a cell of horizontal ink overhang, never outside row
            // or pane. Multi-cell ligatures remain one shaped glyph cluster.
            const PixelRect ink{left - cw * .5f, y, right - left + cw, float(ch)};
            Clip ink_clip(dc.get(), ink);
            const D2D1_POINT_2F origin{pane.content.x + cluster.x,y + cluster.baseline};
            // Paint one cluster multiple times only when paint/selection changes
            // inside it. Each pass is clipped to exact cell backgrounds; shaping
            // is unchanged, so selection never tears apart grapheme processing.
            auto start = cluster.column;
            while (start < cluster.end && start < row.cells.size()) {
                const auto fg = override_color.value_or(foreground(row,start));
                const bool invisible = row.cells[start].style.invisible ||
                    (row.cells[start].style.blink && !pane.text_blink_on);
                auto end = start + 1;
                while (end < cluster.end && end < row.cells.size() && override_color.value_or(foreground(row,end)) == fg &&
                       (row.cells[end].style.invisible || (row.cells[end].style.blink && !pane.text_blink_on)) == invisible) ++end;
                if (!invisible) {
                    const float x0 = start == cluster.column ? ink.x : pane.content.x + float(start * cw);
                    const float x1 = end == cluster.end ? ink.x + ink.width : pane.content.x + float(end * cw);
                    Clip paint_clip(dc.get(), {x0,y,x1-x0,float(ch)});
                    glyph_run(cluster,origin,fg);
                }
                start = end;
            }
        }
    }
    void decorations(const RenderPane& pane, const FrameRow& row, UINT row_index, UINT cw, UINT ch) {
        const auto y = pane.content.y + float(row_index * ch);
        const auto thickness = std::max(1.0f,std::floor(dpi / 96));
        const auto count = std::min<std::size_t>(row.cells.size(),pane.frame->columns);
        for (std::size_t column = 0; column < count; ++column) {
            const auto& c = row.cells[column];
            if (!c.span || c.style.invisible || (c.style.blink && !pane.text_blink_on)) continue;
            const auto x = pane.content.x + float(column * cw);
            const auto length = float(std::min<std::size_t>(c.span,count-column) * cw);
            auto underline_color = selected(row,column) ? selection_foreground : c.underline_color;
            if (c.style.faint) { underline_color.r /= 2; underline_color.g /= 2; underline_color.b /= 2; }
            const float uy = y + float(ch) - thickness * 2;
            if (c.style.underline == 1 || c.style.underline == 2) {
                fill({x,uy,length,thickness},underline_color);
                if (c.style.underline == 2) fill({x,uy-thickness*2,length,thickness},underline_color);
            } else if (c.style.underline >= 3) {
                brush->SetColor(color(underline_color));
                const float step = c.style.underline == 5 ? thickness * 4 : thickness * 2;
                for (float p = 0; p < length; p += step) {
                    if (c.style.underline == 3) {
                        const auto end = std::min(length,p+step);
                        dc->DrawLine({x+p,uy},{x+(p+end)/2,uy-thickness},brush.get(),thickness);
                        dc->DrawLine({x+(p+end)/2,uy-thickness},{x+end,uy},brush.get(),thickness);
                    } else fill({x+p,uy,std::min(length-p,c.style.underline==5?step*.65f:thickness),thickness},underline_color);
                }
            }
            const auto fg = foreground(row,column);
            if (c.style.strikethrough) fill({x,y+ch*.52f,length,thickness},fg);
            if (c.style.overline) fill({x,y+thickness,length,thickness},fg);
        }
    }
    ID2D1Bitmap1* image_bitmap(std::uint64_t pane, const ImagePixels& pixels) {
        const ImageKey key{pane,pixels.generation,pixels.image_id,pixels.width,pixels.height,pixels.format};
        if (auto found = images.find(key); found != images.end()) { found->second.touched = tick; return found->second.bitmap.get(); }
        const std::size_t channels = pixels.format == PixelFormat::rgba ? 4 :
            pixels.format == PixelFormat::rgb ? 3 : pixels.format == PixelFormat::gray_alpha ? 2 : 1;
        const auto pixel_count = std::uint64_t(pixels.width) * pixels.height;
        const auto decoded = pixel_count * 4;
        if (!pixels.width || !pixels.height || pixels.width > options.maximum_image_dimension ||
            pixels.height > options.maximum_image_dimension || decoded > options.image_cache_bytes ||
            decoded > std::numeric_limits<UINT>::max() || pixel_count * channels != pixels.bytes.size()) return nullptr;
        while (!images.empty() && counters.image_cache_bytes + decoded > options.image_cache_bytes) {
            const auto oldest = std::min_element(images.begin(),images.end(),
                [](const auto& a, const auto& b) { return a.second.touched < b.second.touched; });
            counters.image_cache_bytes -= oldest->second.bytes; images.erase(oldest);
        }
        std::vector<std::uint8_t> premultiplied(static_cast<std::size_t>(decoded));
        for (std::size_t i = 0; i < static_cast<std::size_t>(pixel_count); ++i) {
            const auto* p = pixels.bytes.data() + i * channels;
            const auto r = p[0], g = channels >= 3 ? p[1] : r, b = channels >= 3 ? p[2] : r;
            const std::uint8_t a = channels == 4 ? p[3] : channels == 2 ? p[1] : 255;
            premultiplied[i*4] = static_cast<std::uint8_t>((std::uint32_t(b)*a+127)/255);
            premultiplied[i*4+1] = static_cast<std::uint8_t>((std::uint32_t(g)*a+127)/255);
            premultiplied[i*4+2] = static_cast<std::uint8_t>((std::uint32_t(r)*a+127)/255);
            premultiplied[i*4+3] = a;
        }
        CachedImage cached;
        const D2D1_BITMAP_PROPERTIES1 props{{DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED},96,96,
                                           D2D1_BITMAP_OPTIONS_NONE,nullptr};
        check(dc->CreateBitmap({pixels.width,pixels.height},premultiplied.data(),pixels.width*4,&props,cached.bitmap.put()), "Upload image");
        cached.bytes = static_cast<std::size_t>(decoded); cached.touched = tick;
        auto [entry, inserted] = images.emplace(key,std::move(cached));
        if (inserted) { counters.image_cache_bytes += static_cast<std::size_t>(decoded); ++counters.image_uploads; }
        return entry->second.bitmap.get();
    }
    void draw_images(const RenderPane& pane, ImageLayer layer, UINT cw, UINT ch, PixelRect damage) {
        for (const auto& placement : pane.frame->images) {
            if (placement.layer != layer || !placement.pixels) continue;
            const auto& pixels = *placement.pixels;
            if (placement.source_x >= pixels.width || placement.source_y >= pixels.height) continue;
            const auto sw = std::min(placement.source_width ? placement.source_width : pixels.width-placement.source_x,
                                     pixels.width-placement.source_x);
            const auto sh = std::min(placement.source_height ? placement.source_height : pixels.height-placement.source_y,
                                     pixels.height-placement.source_y);
            const auto dw = placement.pixel_width ? float(placement.pixel_width) : placement.columns ? float(placement.columns)*float(cw) : float(sw);
            const auto dh = placement.pixel_height ? float(placement.pixel_height) : placement.rows ? float(placement.rows)*float(ch) : float(sh);
            const PixelRect target{pane.content.x+float(placement.column)*cw+placement.offset_x,
                                   pane.content.y+float(placement.row)*ch+placement.offset_y,dw,dh};
            if (!sw || !sh || !overlaps(target,damage)) continue;
            auto* bitmap = image_bitmap(pane.id,pixels);
            if (!bitmap) continue;
            const auto destination = rect(target);
            const D2D1_RECT_F source{float(placement.source_x),float(placement.source_y),
                                    float(placement.source_x+sw),float(placement.source_y+sh)};
            dc->DrawBitmap(bitmap,&destination,1,D2D1_INTERPOLATION_MODE_LINEAR,&source,nullptr);
        }
    }
    PixelRect cursor_rect(const RenderPane& pane) const noexcept {
        if (!pane.frame || !pane.frame->cursor.visible || !pane.cursor_on ||
            pane.frame->cursor.row >= pane.frame->rows || pane.frame->cursor.column >= pane.frame->columns) return {};
        const auto& frame = *pane.frame;
        auto column = frame.cursor.column;
        if (frame.cursor.wide_tail && column) --column;
        UINT span = 1;
        if (frame.cursor.row < frame.lines.size() && frame.lines[frame.cursor.row] &&
            column < frame.lines[frame.cursor.row]->cells.size())
            span = std::max<UINT>(1,frame.lines[frame.cursor.row]->cells[column].span);
        const auto cw = frame.cell_width ? frame.cell_width : cell.width;
        const auto ch = frame.cell_height ? frame.cell_height : cell.height;
        return intersect(pane.content,{pane.content.x+float(column*cw),pane.content.y+float(frame.cursor.row*ch),float(span*cw),float(ch)});
    }
    void draw_cursor(const RenderPane& pane, UINT cw, UINT ch) {
        auto bounds = cursor_rect(pane);
        if (!nonempty(bounds)) return;
        const auto& cursor = pane.frame->cursor;
        const auto thickness = std::max(1.0f,std::round(dpi / 96));
        const auto shape = pane.focused ? cursor.shape : CursorShape::hollow_block;
        if (shape == CursorShape::bar) bounds.width = thickness*2;
        if (shape == CursorShape::underline) { bounds.y += bounds.height-thickness*2; bounds.height = thickness*2; }
        if (shape == CursorShape::hollow_block) {
            brush->SetColor(color(cursor.color));
            const D2D1_RECT_F outline{bounds.x+thickness*.5f,bounds.y+thickness*.5f,
                                      bounds.x+bounds.width-thickness*.5f,bounds.y+bounds.height-thickness*.5f};
            dc->DrawRectangle(outline,brush.get(),thickness);
        } else {
            fill(bounds,cursor.color);
            if (shape == CursorShape::block && cursor.row < pane.frame->lines.size() && pane.frame->lines[cursor.row]) {
                Clip clip(dc.get(),bounds);
                const auto& row=*pane.frame->lines[cursor.row];
                const std::size_t column=cursor.wide_tail && cursor.column?cursor.column-1:cursor.column;
                const auto text_color=column<row.cells.size()?background(row,column):pane.frame->background;
                draw_text(pane,row,cursor.row,cw,ch,text_color);
            }
        }
    }
    void draw_pane(const RenderPane& pane, PixelRect damage) {
        if (!pane.frame || !nonempty(pane.content) || !overlaps(pane.content,damage)) return;
        const auto& frame = *pane.frame;
        const UINT cw = frame.cell_width ? frame.cell_width : cell.width;
        const UINT ch = frame.cell_height ? frame.cell_height : cell.height;
        Clip clip(dc.get(),pane.content);
        fill(pane.content,frame.background,backdrop_opacity(),true);
        draw_images(pane,ImageLayer::below_background,cw,ch,damage);
        const auto rows = std::min<std::size_t>(frame.rows,frame.lines.size());
        for (std::size_t r = 0; r < rows; ++r) {
            if (!frame.lines[r] || !overlaps({pane.content.x,pane.content.y+float(r*ch),pane.content.width,float(ch)},damage)) continue;
            const auto& row = *frame.lines[r];
            const auto columns = std::min<std::size_t>(frame.columns,row.cells.size());
            for (std::size_t column = 0; column < columns;) {
                const auto& c = row.cells[column];
                const auto bg = background(row,column);
                const bool default_bg = c.background_is_default && !c.style.inverse && !selected(row,column);
                auto end = column+1;
                while (end < columns && background(row,end) == bg &&
                    (row.cells[end].background_is_default && !row.cells[end].style.inverse && !selected(row,end)) == default_bg) ++end;
                // Default backgrounds were painted once; skipping them also lets
                // Kitty's below-background layer show through. Explicit cells,
                // inverse and selection remain opaque even at matching RGB.
                if (!default_bg) fill({pane.content.x+float(column*cw),pane.content.y+float(r*ch),float((end-column)*cw),float(ch)},bg);
                column = end;
            }
        }
        draw_images(pane,ImageLayer::below_text,cw,ch,damage);
        for (std::size_t r = 0; r < rows; ++r) {
            if (!frame.lines[r] || !overlaps({pane.content.x,pane.content.y+float(r*ch),pane.content.width,float(ch)},damage)) continue;
            draw_text(pane,*frame.lines[r],static_cast<UINT>(r),cw,ch);
            decorations(pane,*frame.lines[r],static_cast<UINT>(r),cw,ch);
        }
        draw_images(pane,ImageLayer::above_text,cw,ch,damage);
        draw_cursor(pane,cw,ch);
    }
    void draw_chrome(const WindowScene& scene, PixelRect damage, std::span<const ChromeDrawItem> order) {
        for (const auto& item : order) {
            if (item.kind==ChromeDrawKind::primitive) {
                const auto& primitive=scene.primitives[item.index];
                if(!overlaps(chrome_bounds(primitive),damage))continue;
                if (primitive.kind == ChromePrimitiveKind::line) {
                    brush->SetColor(color(primitive.stroke_width>0?primitive.stroke:primitive.fill));
                    dc->DrawLine({primitive.rect.x,primitive.rect.y},{primitive.rect.x+primitive.rect.width,primitive.rect.y+primitive.rect.height},
                                 brush.get(),std::max(1.0f,primitive.stroke_width));
                } else {
                    const auto radius=std::clamp(primitive.radius,0.0f,std::min(primitive.rect.width,primitive.rect.height)*.5f);
                    const D2D1_ROUNDED_RECT rounded{rect(primitive.rect),radius,radius};
                    if (primitive.fill.a || primitive.background_replace) {
                        auto fill_color=primitive.fill;
                        if(primitive.background_replace && !material_enabled())fill_color.a=255;
                        brush->SetColor(color(fill_color));
                        if(primitive.background_replace)dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
                        dc->FillRoundedRectangle(rounded,brush.get());
                        if(primitive.background_replace)dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
                    }
                    if (primitive.stroke_width>0 && primitive.stroke.a) {
                        brush->SetColor(color(primitive.stroke)); dc->DrawRoundedRectangle(rounded,brush.get(),primitive.stroke_width);
                    }
                }
            } else {
                const auto& label=scene.texts[item.index];
                if (!nonempty(label.rect) || !overlaps(label.rect,damage) || label.text.empty() || !label.color.a) continue;
                if (label.text.size()>32768 || label.family.empty() || label.family.size()>256 ||
                    !std::isfinite(label.size_dip) || label.size_dip<1 || label.size_dip>256) continue;
                const auto text = utf16(label.text), family = utf16(label.family);
                Com<IDWriteTextFormat> format;
                check(write_factory->CreateTextFormat(family.c_str(),nullptr,label.semibold?DWRITE_FONT_WEIGHT_SEMI_BOLD:DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,label.size_dip*dpi/96,L"en-US",format.put()),"Create chrome font");
                check(format->SetTextAlignment(label.alignment==ChromeTextAlignment::center?DWRITE_TEXT_ALIGNMENT_CENTER:
                    label.alignment==ChromeTextAlignment::trailing?DWRITE_TEXT_ALIGNMENT_TRAILING:DWRITE_TEXT_ALIGNMENT_LEADING),"Set chrome alignment");
                check(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP),"Set chrome no-wrap");
                check(format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER),"Center chrome baseline");
                // DirectWrite owns grapheme/cluster-safe end truncation and the real
                // omission glyph; never shorten UTF-8 strings or clip a partial glyph.
                Com<IDWriteInlineObject> ellipsis;
                check(write_factory->CreateEllipsisTrimmingSign(format.get(),ellipsis.put()),"Create chrome ellipsis");
                const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};
                check(format->SetTrimming(&trimming,ellipsis.get()),"Set chrome end ellipsis");
                Com<IDWriteTextLayout> layout;
                check(write_factory->CreateTextLayout(text.data(),static_cast<UINT32>(text.size()),format.get(),
                                                      label.rect.width,label.rect.height,layout.put()),"Create chrome layout");
                auto foreground=label.color;foreground.a=255;
                brush->SetColor(color(foreground));
                Clip clip(dc.get(),label.rect);
                // Layer opacity applies equally to monochrome and color-font layers.
                // Only deliberate ChromeText alpha is faded; terminal rows never are.
                OpacityLayer opacity(dc.get(),intersect(label.rect,{0,0,float(width),float(height)}),float(label.color.a)/255);
                dc->DrawTextLayout({label.rect.x,label.rect.y},layout.get(),brush.get(),D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            }
        }
    }
    bool same_images(const RenderFrame& a, const RenderFrame& b) const noexcept {
        if (a.image_placements_generation != b.image_placements_generation || a.images.size()!=b.images.size()) return false;
        for (std::size_t i=0;i<a.images.size();++i) {
            const auto& x=a.images[i]; const auto& y=b.images[i];
            if (bool(x.pixels)!=bool(y.pixels)) return false;
            if (x.pixels && (x.pixels->image_id!=y.pixels->image_id || x.pixels->generation!=y.pixels->generation)) return false;
        }
        return true;
    }
    std::vector<PixelRect> damage(const WindowScene& scene) const {
        const PixelRect full{0,0,float(width),float(height)};
        if (invalid || scene.force_redraw ||
            scene.background != previous.background || scene.panes.size()!=previous.panes.size()) return {full};
        std::vector<PixelRect> result;
        auto add = [&](PixelRect r) { if(!nonempty(r))return;r=intersect(r,full);if(nonempty(r))result.push_back(r); };
        if(scene.chrome_generation!=previous.chrome_generation) {
            const auto primitives=std::max(scene.primitives.size(),previous.primitives.size());
            for(std::size_t i=0;i<primitives;++i) {
                if(i<scene.primitives.size() && i<previous.primitives.size() && scene.primitives[i]==previous.primitives[i])continue;
                if(i<scene.primitives.size())add(chrome_bounds(scene.primitives[i]));
                if(i<previous.primitives.size())add(chrome_bounds(previous.primitives[i]));
            }
            const auto texts=std::max(scene.texts.size(),previous.texts.size());
            for(std::size_t i=0;i<texts;++i) {
                if(i<scene.texts.size() && i<previous.texts.size() && scene.texts[i]==previous.texts[i])continue;
                if(i<scene.texts.size())add(scene.texts[i].rect);
                if(i<previous.texts.size())add(previous.texts[i].rect);
            }
        }
        for (std::size_t i=0;i<scene.panes.size();++i) {
            const auto& now=scene.panes[i]; const auto& before=previous.panes[i];
            if (now.id!=before.id || now.content!=before.content) return {full};
            if (!now.frame || !before.frame) { if(now.frame!=before.frame) add(now.content); continue; }
            const auto& a=*now.frame; const auto& b=*before.frame;
            if (a.columns!=b.columns || a.rows!=b.rows || a.cell_width!=b.cell_width || a.cell_height!=b.cell_height ||
                a.background!=b.background || a.foreground!=b.foreground || !same_images(a,b)) { add(now.content); continue; }
            const UINT ch=a.cell_height?a.cell_height:cell.height;
            if (now.frame!=before.frame) {
                const auto rows=std::max(a.lines.size(),b.lines.size());
                for(std::size_t r=0;r<rows;++r) {
                    bool dirty=r>=a.lines.size() || r>=b.lines.size();
                    if(!dirty) {
                        const auto& ar=a.lines[r];const auto& br=b.lines[r];
                        dirty=bool(ar)!=bool(br) || (ar && (ar->id!=br->id || ar->generation!=br->generation));
                    }
                    if(dirty) add(intersect(now.content,{now.content.x,now.content.y+float(r*ch),now.content.width,float(ch)}));
                }
            }
            if (now.text_blink_on!=before.text_blink_on) {
                for (std::size_t r=0;r<a.lines.size();++r) {
                    if(a.lines[r] && std::any_of(a.lines[r]->cells.begin(),a.lines[r]->cells.end(),
                                               [](const Cell& c){return c.style.blink;}))
                        add(intersect(now.content,{now.content.x,now.content.y+float(r*ch),now.content.width,float(ch)}));
                }
            }
            if (!same_cursor(a.cursor,b.cursor) || now.cursor_on!=before.cursor_on || now.focused!=before.focused) {
                add(cursor_rect(before)); add(cursor_rect(now));
            }
        }
        // Merge touching full-width row bands and overlapping cursor rectangles.
        // A very fragmented update is cheaper as one correct canvas repaint.
        if(result.size()>128) return {full};
        for(std::size_t i=0;i<result.size();++i) {
            for(std::size_t j=i+1;j<result.size();) {
                auto& a=result[i];const auto& b=result[j];
                const bool aligned=a.x==b.x && a.width==b.width && a.y<=b.y+b.height && b.y<=a.y+a.height;
                if(aligned || overlaps(a,b)) {
                    const auto x=std::min(a.x,b.x),y=std::min(a.y,b.y);
                    a={x,y,std::max(a.x+a.width,b.x+b.width)-x,std::max(a.y+a.height,b.y+b.height)-y};
                    result.erase(result.begin()+static_cast<std::ptrdiff_t>(j));
                } else ++j;
            }
        }
        return result;
    }
    RenderResult render(const WindowScene& scene) {
        if (!width || !height || !canvas) return RenderResult::suspended;
        ++tick;
        const auto dirty=damage(scene);
        if(dirty.empty() && !pending_present) {previous=scene;previous.force_redraw=false;return RenderResult::unchanged;}
        if(!dirty.empty()) {
            const auto chrome_order=chrome_paint_order(scene);
            dc->SetTarget(canvas.get());
            dc->BeginDraw();
            try {
                for (auto r:dirty) {
                    // Snap damage outward to physical pixels to prevent partially
                    // accumulated alpha at fractional layout boundaries.
                    const auto right=std::ceil(r.x+r.width),bottom=std::ceil(r.y+r.height);
                    r.x=std::floor(r.x);r.y=std::floor(r.y);r.width=right-r.x;r.height=bottom-r.y;
                    Clip clip(dc.get(),r);
                    dc->Clear(D2D1_COLOR_F{0,0,0,0});
                    fill({0,0,float(width),float(height)},scene.background,backdrop_opacity(),true);
                    for(const auto& pane:scene.panes) draw_pane(pane,r);
                    draw_chrome(scene,r,chrome_order);
                }
            } catch (...) { dc->EndDraw(); invalid=true; throw; }
            check(dc->EndDraw(),"Render canvas");
            counters.dirty_rectangles+=dirty.size();
            previous=scene; previous.force_redraw=false; invalid=false; pending_present=true;
        }
        if(swapchain) {
            // Flip buffers are never assumed to retain old pixels. All damage is
            // accumulated into canvas, then the complete canvas is copied to the
            // current backbuffer. Shapes/uploads remain damage-driven.
            Com<ID3D11Texture2D> backbuffer;
            check(swapchain->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(backbuffer.put())),"Get swapchain buffer");
            immediate->CopyResource(backbuffer.get(),canvas_texture.get());
            const DXGI_PRESENT_PARAMETERS parameters{};
            const auto hr=swapchain->Present1(1,DXGI_PRESENT_DO_NOT_WAIT,&parameters);
            if(hr==DXGI_ERROR_WAS_STILL_DRAWING) return RenderResult::busy;
            if(hr==DXGI_STATUS_OCCLUDED) return RenderResult::occluded;
            check(hr,"Present");
        } else immediate->Flush();
        pending_present=false;++counters.presents;
        return RenderResult::presented;
    }
};

Renderer::Renderer(void* window,RendererOptions options) : impl_(std::make_unique<Impl>(window,std::move(options))) {}
Renderer::~Renderer() {
    const bool smoke=impl_ && impl_->options.smoke_teardown_diagnostics;
    windows::smoke_teardown_trace(smoke,"renderer destructor begin");
    impl_.reset();
    windows::smoke_teardown_trace(smoke,"renderer destructor end");
}
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;
void Renderer::resize(std::uint32_t width,std::uint32_t height,float dpi) {
    if (!std::isfinite(dpi) || dpi<48 || dpi>768) throw std::invalid_argument("Invalid surface DPI");
    if(width>16384 || height>16384 || std::uint64_t(width)*height>64u*1024u*1024u)
        throw std::length_error("Surface exceeds bounded renderer dimensions");
    auto& p=*impl_;
    if(p.dpi!=dpi) { p.dpi=dpi;p.measure_font(); }
    if(width==p.width && height==p.height && p.canvas) return;
    p.width=width;p.height=height;
    p.dc->SetTarget(nullptr);p.canvas.reset();p.canvas_texture.reset();p.invalid=true;
    if(!width || !height) { p.opaque_window.resize(width,height); return; }
    try {
        if(p.swapchain) check(p.swapchain->ResizeBuffers(2,width,height,DXGI_FORMAT_B8G8R8A8_UNORM,
                                DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),"Resize swapchain");
        p.create_surfaces();
    } catch(const GraphicsError& error) { if(!device_lost(error.result)) throw;recreate_device(); }
}
void Renderer::set_font(FontOptions font) {
    Impl::validate_font(font);
    if(impl_->options.font==font) return;
    impl_->options.font=std::move(font);impl_->measure_font();
}
void Renderer::set_background_opacity(float opacity) {
    if(!std::isfinite(opacity) || opacity<0 || opacity>1) throw std::invalid_argument("Invalid background opacity");
    if(impl_->options.background_opacity!=opacity) {impl_->options.background_opacity=opacity;impl_->invalid=true;}
}
CellMetrics Renderer::metrics() const noexcept {return impl_->cell;}
RenderResult Renderer::render(const WindowScene& scene) {
    try {return impl_->render(scene);}
    catch(const GraphicsError& error) {
        if(!device_lost(error.result)) throw;
        recreate_device();
        return impl_->render(scene); // One bounded real recovery, never infinite retry.
    }
}
void* Renderer::frame_latency_wait_handle() const noexcept {return impl_->latency.value;}
RendererStats Renderer::stats() const noexcept {return impl_->counters;}
void Renderer::recreate_device() {
    auto& p=*impl_;
    p.release_gpu();
    p.create_device();p.invalid=true;++p.counters.device_recoveries;
}
void Renderer::cancel_alpha_probe() noexcept { impl_->alpha_staging.reset(); }
bool Renderer::begin_alpha_probe(PixelRect area) {
    auto& p=*impl_;p.alpha_staging.reset();
    if(!p.canvas_texture || !std::isfinite(area.x) || !std::isfinite(area.y) || !std::isfinite(area.width) || !std::isfinite(area.height) ||
        area.x<0 || area.y<0 || area.width<1 || area.height<1 || area.x>=p.width || area.y>=p.height) return false;
    const UINT x=static_cast<UINT>(area.x),y=static_cast<UINT>(area.y);
    p.alpha_width=std::min({16u,static_cast<UINT>(std::min(area.width,16.f)),p.width-x});
    p.alpha_height=std::min({16u,static_cast<UINT>(std::min(area.height,16.f)),p.height-y});
    D3D11_TEXTURE2D_DESC desc{};desc.Width=p.alpha_width;desc.Height=p.alpha_height;desc.MipLevels=1;desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(p.device->CreateTexture2D(&desc,nullptr,p.alpha_staging.put()),"Create alpha diagnostic staging texture");
    const D3D11_BOX source{x,y,0,x+p.alpha_width,y+p.alpha_height,1};
    p.immediate->CopySubresourceRegion(p.alpha_staging.get(),0,0,0,0,p.canvas_texture.get(),0,&source);
    p.alpha_metadata={255,0,p.alpha_width*p.alpha_height,p.backdrop_opacity(),p.counters.composition,p.counters.warp,p.counters.device_recoveries,p.counters.presents};
    p.immediate->Flush(); // Submit the tiny copy, never wait for completion.
    return true;
}
std::optional<RendererAlphaProbe> Renderer::poll_alpha_probe() {
    auto& p=*impl_;if(!p.alpha_staging)return std::nullopt;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto hr=p.immediate->Map(p.alpha_staging.get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
    if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return std::nullopt;
    if(FAILED(hr)){p.alpha_staging.reset();check(hr,"Map alpha diagnostic staging texture");}
    auto result=p.alpha_metadata;
    for(UINT y=0;y<p.alpha_height;++y) {
        const auto* bytes=static_cast<const std::uint8_t*>(mapped.pData)+std::size_t(y)*mapped.RowPitch;
        for(UINT x=0;x<p.alpha_width;++x) {const auto alpha=bytes[x*4+3];result.minimum=std::min(result.minimum,alpha);result.maximum=std::max(result.maximum,alpha);}
    }
    p.immediate->Unmap(p.alpha_staging.get(),0);p.alpha_staging.reset();return result;
}
std::vector<std::uint8_t> Renderer::read_pixels() {
    auto& p=*impl_;if(!p.canvas_texture) return {};
    D3D11_TEXTURE2D_DESC desc{};p.canvas_texture->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> staging;
    check(p.device->CreateTexture2D(&desc,nullptr,staging.put()),"Create readback texture");
    p.immediate->CopyResource(staging.get(),p.canvas_texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(p.immediate->Map(staging.get(),0,D3D11_MAP_READ,0,&mapped),"Readback map");
    struct Unmap {ID3D11DeviceContext* context;ID3D11Texture2D* texture;~Unmap(){context->Unmap(texture,0);}} unmap{p.immediate.get(),staging.get()};
    std::vector<std::uint8_t> result(std::size_t(p.width)*p.height*4);
    for(UINT y=0;y<p.height;++y) {
        const auto* source=static_cast<const std::uint8_t*>(mapped.pData)+std::size_t(y)*mapped.RowPitch;
        for(UINT x=0;x<p.width;++x) {
            const auto target=(std::size_t(y)*p.width+x)*4;
            const auto alpha=source[x*4+3];
            // Public readback is straight RGBA, unlike the premultiplied canvas.
            auto straight=[alpha](std::uint8_t value)->std::uint8_t {
                return alpha?static_cast<std::uint8_t>(std::min(255u,(unsigned(value)*255u+alpha/2)/alpha)):0;
            };
            result[target]=straight(source[x*4+2]);result[target+1]=straight(source[x*4+1]);
            result[target+2]=straight(source[x*4]);result[target+3]=alpha;
        }
    }
    return result;
}
} // namespace stormtty
