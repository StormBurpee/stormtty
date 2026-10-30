#include "text_resources.hpp"
#include "graphics_error.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace stormtty::windows {
namespace {
std::wstring utf16(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("UTF-8 text exceeds platform length");
    }

    const auto count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        return L"\uFFFD";
    }

    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

// Resolve only beside this executable, never relative to the terminal's CWD,
// PATH, a user-controlled shell title, or the system font installation directory.
std::wstring bundled_font_directory() {
    constexpr std::size_t maximum_path = 32768;
    std::wstring path(256, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        if (path.size() == maximum_path) {
            return {};
        }
        path.resize(std::min(maximum_path, path.size() * 2));
    }

    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }

    path.resize(slash + 1);
    path += L"assets\\fonts\\";
    if (path.size() + 64 >= maximum_path) {
        return {};
    }

    // Extended absolute paths avoid dependence on a process manifest's long-path
    // setting. Keep existing extended paths and short paths unchanged.
    if (path.size() + 64 >= MAX_PATH && path.rfind(L"\\\\?\\", 0) != 0) {
        if (path.rfind(L"\\\\", 0) == 0) {
            path = L"\\\\?\\UNC\\" + path.substr(2);
        } else {
            path = L"\\\\?\\" + path;
        }
    }
    return path;
}

Com<IDWriteFontCollection> load_bundled_fonts(IDWriteFactory* factory) noexcept {
    try {
        // Factory5 is runtime-probed. AddFontFile imports all variable-font
        // instances, including real weights, rather than just a default face.
        Com<IDWriteFactory5> factory5;
        if (FAILED(factory->QueryInterface(__uuidof(IDWriteFactory5), reinterpret_cast<void**>(factory5.put())))) {
            return {};
        }

        const auto directory = bundled_font_directory();
        if (directory.empty()) {
            return {};
        }

        Com<IDWriteFontSetBuilder1> builder;
        if (FAILED(factory5->CreateFontSetBuilder(builder.put()))) {
            return {};
        }
        constexpr std::array<const wchar_t*, 2> filenames{L"CascadiaMono.ttf", L"CascadiaMonoItalic.ttf"};
        for (const auto* filename : filenames) {
            const auto path = directory + filename;
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
               (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || attributes.nFileSizeHigh ||
               !attributes.nFileSizeLow || attributes.nFileSizeLow > 8u * 1024u * 1024u) {
                return {};
            }

            Com<IDWriteFontFile> file;
            if (FAILED(factory->CreateFontFileReference(path.c_str(), &attributes.ftLastWriteTime, file.put()))) {
                return {};
            }

            BOOL supported{};
            DWRITE_FONT_FILE_TYPE file_type{};
            DWRITE_FONT_FACE_TYPE face_type{};
            UINT32 faces{};
            if (FAILED(file->Analyze(&supported, &file_type, &face_type, &faces)) || !supported || !faces || faces > 64 ||
               (file_type != DWRITE_FONT_FILE_TYPE_TRUETYPE && file_type != DWRITE_FONT_FILE_TYPE_CFF)) {
                return {};
            }
            if (FAILED(builder->AddFontFile(file.get()))) {
                return {};
            }
        }

        Com<IDWriteFontSet> set;
        if (FAILED(builder->CreateFontSet(set.put())) || !set->GetFontCount() || set->GetFontCount() > 256) {
            return {};
        }

        Com<IDWriteFontCollection1> collection;
        if (FAILED(factory5->CreateFontCollectionFromFontSet(set.get(), collection.put()))) {
            return {};
        }

        UINT32 index{};
        BOOL exists{};
        if (FAILED(collection->FindFamilyName(L"Cascadia Mono", &index, &exists)) || !exists) {
            return {};
        }

        Com<IDWriteFontFamily1> family;
        if (FAILED(collection->GetFontFamily(index, family.put()))) {
            return {};
        }

        for (auto style : {DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STYLE_ITALIC}) {
            Com<IDWriteFont> face;
            if (FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, style, face.put())) ||
               face->GetStyle() != style || face->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE) {
                return {};
            }
        }

        Com<IDWriteFontCollection> result;
        if (FAILED(collection.as(result))) {
            return {};
        }
        return result;
    } catch (...) {
        // Missing, oversized, malformed or unsupported optional assets use the
        // existing system-font fallback. No registration, downloads or retries.
        return {};
    }
}

struct CellMap {
    UINT32 column{};
    UINT32 end{};
};

// DirectWrite performs contextual shaping AND system font fallback for the whole
// row. We capture its many-to-many clusters, not Unicode scalars. The subsequent
// placement keeps VT columns authoritative rather than accepting paragraph bidi
// reordering or proportional layout widths. Full terminal bidi is not supported.
class ClusterCapture final : public IDWriteTextRenderer {
public:
    explicit ClusterCapture(const std::vector<CellMap>& mapping)
        : mapping_(mapping) {}

    std::vector<GlyphCluster> clusters;

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid,
        void** out) noexcept override {
        if (!out) {
            return E_POINTER;
        }
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteTextRenderer) ||
            iid == __uuidof(IDWritePixelSnapping)) {
            *out = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return ++refs_;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const auto count = --refs_;
        if (!count) {
            delete this;
        }
        return count;
    }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(
        void*,
        BOOL* disabled) noexcept override {
        if (!disabled) {
            return E_POINTER;
        }
        *disabled = TRUE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentTransform(
        void*,
        DWRITE_MATRIX* matrix) noexcept override {
        if (!matrix) {
            return E_POINTER;
        }
        *matrix = {1, 0, 0, 1, 0, 0};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(
        void*,
        FLOAT* pixels) noexcept override {
        if (!pixels) {
            return E_POINTER;
        }
        *pixels = 1;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(
        void*,
        FLOAT origin_x,
        FLOAT baseline,
        DWRITE_MEASURING_MODE,
        const DWRITE_GLYPH_RUN* run,
        const DWRITE_GLYPH_RUN_DESCRIPTION* description,
        IUnknown*) noexcept override {
        try {
            if (!run || !description || !description->clusterMap || !run->glyphIndices ||
                !run->glyphAdvances || !run->fontFace || run->glyphCount == 0) {
                return S_OK;
            }
            const auto length = description->stringLength;
            if (std::uint64_t(description->textPosition) + length > mapping_.size()) {
                return E_INVALIDARG;
            }
            // Build glyph-cluster boundaries from DirectWrite's UTF-16 cluster map.
            // Sorting glyph offsets handles both LTR and RTL runs without reversing
            // terminal cell order or assuming one character == one glyph.
            std::vector<UINT32> boundaries;
            boundaries.reserve(length + 1);
            std::vector<CellMap> cells(run->glyphCount, {std::numeric_limits<UINT32>::max(), 0});
            for (UINT32 i = 0; i < length; ++i) {
                const auto glyph = description->clusterMap[i];
                if (glyph >= run->glyphCount) {
                    return E_INVALIDARG;
                }
                boundaries.push_back(glyph);
                const auto& mapped = mapping_[description->textPosition + i];
                cells[glyph].column = std::min(cells[glyph].column, mapped.column);
                cells[glyph].end = std::max(cells[glyph].end, mapped.end);
            }
            boundaries.push_back(run->glyphCount);
            std::sort(boundaries.begin(), boundaries.end());
            boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
            std::vector<float> positions(run->glyphCount + 1);
            for (UINT32 i = 0; i < run->glyphCount; ++i) {
                positions[i + 1] = positions[i] + run->glyphAdvances[i];
            }
            for (std::size_t b = 0; b + 1 < boundaries.size(); ++b) {
                const auto first = boundaries[b];
                const auto last = boundaries[b + 1];
                const auto column = cells[first].column;
                const auto end = cells[first].end;
                if (column == std::numeric_limits<UINT32>::max() || end <= column) {
                    continue;
                }
                GlyphCluster cluster;
                cluster.face = Com<IDWriteFontFace>(run->fontFace, true);
                cluster.glyphs.assign(run->glyphIndices + first, run->glyphIndices + last);
                cluster.advances.assign(run->glyphAdvances + first, run->glyphAdvances + last);
                if (run->glyphOffsets) {
                    cluster.offsets.assign(run->glyphOffsets + first, run->glyphOffsets + last);
                } else {
                    cluster.offsets.resize(last - first);
                }
                cluster.column = column;
                cluster.end = end;
                cluster.bidi = run->bidiLevel;
                cluster.size = run->fontEmSize;
                cluster.baseline = baseline;
                cluster.sideways = run->isSideways != FALSE;
                cluster.advance_total = positions[last] - positions[first];
                cluster.natural_left = (run->bidiLevel & 1) ? origin_x - positions[last] : origin_x + positions[first];
                clusters.push_back(std::move(cluster));
            }
            return S_OK;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return E_FAIL;
        }
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(
        void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) noexcept override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawStrikethrough(
        void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) noexcept override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawInlineObject(
        void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) noexcept override {
        return E_NOTIMPL;
    }

private:
    std::atomic<ULONG> refs_{1};
    const std::vector<CellMap>& mapping_;
};
struct RowKey {
    std::uint64_t pane{};
    std::uint64_t row0{};
    std::uint64_t row1{};
    std::uint64_t generation{};
    std::uint64_t font{};
    std::uint32_t columns{};
    std::uint32_t cell_width{};
    std::uint32_t cell_height{};
    bool operator == (const RowKey&) const = default;
};
struct RowHash {
    std::size_t operator()(const RowKey& key) const noexcept {
        std::size_t hash = 0;
        const std::array<std::uint64_t, 8> values{
            key.pane,
            key.row0,
            key.row1,
            key.generation,
            key.font,
            key.columns,
            key.cell_width,
            key.cell_height,
        };
        for (auto value : values) {
            hash ^= std::hash<std::uint64_t>{}(value) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};
} // namespace

struct TextResources::Impl {
    FontOptions font;
    float dpi{96};
    float em_size{14};
    CellMetrics cell;
    std::uint64_t font_generation{1};
    std::size_t shape_cache_budget{};
    TextResourceStats counters;
    bool configured{};

    // Declaration order retains cache -> collection -> format -> factory release.
    Com<IDWriteFactory> write_factory;
    Com<IDWriteFactory4> write4;
    Com<IDWriteTextFormat> terminal_format;
    Com<IDWriteTypography> typography;
    Com<IDWriteFontCollection> bundled_fonts;
    bool bundled_fonts_attempted{};
    std::unordered_map<RowKey, ShapedRow, RowHash> shapes;

    explicit Impl(std::size_t budget) : shape_cache_budget(budget) {
        if (budget > 256u * 1024u * 1024u) {
            throw std::invalid_argument("Renderer cache or image dimensions exceed safety limits");
        }
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
              reinterpret_cast<IUnknown**>(write_factory.put())), "DWriteCreateFactory");
        write_factory.as(write4); // Runtime probe, not a hard requirement.
    }

    // Font selection/size are shared by all chrome uses. Layout policy is not:
    // paint ellipsizes and centers, measurement remains untrimmed, wrap breaks lines.
    Com<IDWriteTextFormat> chrome_format(
        const ChromeText& label,
        const char* action) const {
        const auto family = utf16(label.family);
        const auto weight = label.semibold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
        Com<IDWriteTextFormat> format;
        check(write_factory->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
              DWRITE_FONT_STRETCH_NORMAL, label.size_dip * dpi / 96, L"en-US", format.put()), action);
        return format;
    }

    void measure_font() {
        em_size = font.size_dip * dpi / 96;
        auto family = utf16(font.family);
        const auto locale = utf16(font.locale);
        IDWriteFontCollection* chosen_collection = nullptr;
        if (CompareStringOrdinal(family.c_str(), -1, L"Cascadia Mono", -1, TRUE) == CSTR_EQUAL) {
            if (!bundled_fonts_attempted) {
                bundled_fonts_attempted = true;
                bundled_fonts = load_bundled_fonts(write_factory.get());
            }
            // Private Cascadia wins over an arbitrary system-installed version.
            // Other configured font families still resolve in the system collection.
            chosen_collection = bundled_fonts.get();
        }
        if (!chosen_collection) {
            Com<IDWriteFontCollection> fonts;
            check(write_factory->GetSystemFontCollection(fonts.put(), FALSE), "Get system fonts");
            UINT32 family_index{};
            BOOL exists{};
            check(fonts->FindFamilyName(family.c_str(), &family_index, &exists), "Find terminal font");
            if (!exists) {
                family = L"Consolas";
                check(fonts->FindFamilyName(family.c_str(), &family_index, &exists), "Find fallback mono font");
                if (!exists) {
                    family = L"Courier New";
                }
            }
        }

        // A custom base collection does not replace DirectWrite's default system
        // fallback list. Keep that list for CJK, emoji and other absent glyphs.
        check(write_factory->CreateTextFormat(family.c_str(), chosen_collection, DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, em_size, locale.c_str(), terminal_format.put()),
              "Create terminal format");
        counters.bundled_font = chosen_collection != nullptr;
        check(terminal_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "Disable text wrapping");
        check(write_factory->CreateTypography(typography.put()), "Create terminal typography");
        const UINT32 enabled = font.ligatures ? 1u : 0u;
        for (auto feature : {
                 DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES,
                 DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_LIGATURES,
                 DWRITE_FONT_FEATURE_TAG_DISCRETIONARY_LIGATURES,
                 DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_ALTERNATES}) {
            check(typography->AddFontFeature({feature, enabled}), "Set optional ligature feature");
        }

        // Required script ligatures must stay enabled even when programming
        // ligatures are disabled; disabling rlig would break script shaping.
        check(typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_REQUIRED_LIGATURES, 1}), "Enable required ligatures");
        Com<IDWriteTextLayout> measure;
        check(write_factory->CreateTextLayout(L"M", 1, terminal_format.get(), em_size * 4, em_size * 4, measure.put()), "Measure font");
        DWRITE_TEXT_METRICS metrics{};
        check(measure->GetMetrics(&metrics), "Get font metrics");
        DWRITE_LINE_METRICS line{};
        UINT32 count{};
        check(measure->GetLineMetrics(&line, 1, &count), "Get line metrics");
        cell.width = static_cast<std::uint16_t>(std::clamp(std::ceil(metrics.widthIncludingTrailingWhitespace), 1.0f, 65535.0f));
        cell.height = static_cast<std::uint16_t>(std::clamp(std::ceil(line.height + dpi / 96), 1.0f, 65535.0f));
        cell.baseline = line.baseline;
        shapes.clear();
        counters.retained_bytes = 0;
        ++font_generation;
    }

    ShapedRow build_shape(
        const FrameRow& row,
        UINT columns,
        UINT cw,
        UINT ch) {
        constexpr std::size_t maximum_utf16_per_row = 131072;
        constexpr std::size_t maximum_utf16_per_grapheme = 4096;
        struct StyleRange {
            UINT32 start{};
            UINT32 length{};
            bool bold{};
            bool italic{};
        };
        std::wstring text;
        std::vector<CellMap> mapping;
        std::vector<StyleRange> styles;
        const auto count = std::min<std::size_t>(columns, row.cells.size());
        for (std::size_t column = 0; column < count; ++column) {
            const auto& source = row.cells[column];
            if (!source.span) {
                continue;
            }
            auto grapheme = source.image_placeholder || source.grapheme.empty() ? std::wstring(L" ") :
                source.grapheme.size() > maximum_utf16_per_grapheme * 4 ? std::wstring(L"\uFFFD") : utf16(source.grapheme);
            if (grapheme.size() > maximum_utf16_per_grapheme ||
                text.size() + grapheme.size() + count - column - 1 > maximum_utf16_per_row) {
                // Explicit bounded pathological-sequence fallback.
                grapheme = L"\uFFFD";
            }
            const auto start = static_cast<UINT32>(text.size());
            text += grapheme;
            mapping.insert(mapping.end(), grapheme.size(), CellMap{static_cast<UINT32>(column),
                static_cast<UINT32>(std::min<std::size_t>(count, column + source.span))});
            if (!styles.empty() && styles.back().bold == source.style.bold && styles.back().italic == source.style.italic) {
                styles.back().length += static_cast<UINT32>(grapheme.size());
            } else {
                styles.push_back({start, static_cast<UINT32>(grapheme.size()), source.style.bold, source.style.italic});
            }
        }

        ShapedRow result;
        result.bytes = sizeof(ShapedRow) + sizeof(RowKey) + sizeof(void*) * 4;
        if (text.empty()) {
            return result;
        }

        Com<IDWriteTextLayout> layout;
        check(write_factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), terminal_format.get(),
              std::max(1.0f, float(columns * cw) * 4), float(ch) * 4, layout.put()), "Shape terminal row");
        check(layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "Set row no-wrap");
        check(layout->SetReadingDirection(DWRITE_READING_DIRECTION_LEFT_TO_RIGHT), "Set VT row direction");
        check(layout->SetTypography(typography.get(), {0, static_cast<UINT32>(text.size())}), "Apply terminal typography");
        check(layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, float(ch), std::min(float(ch), cell.baseline)),
              "Anchor row baseline to cell metrics");
        for (const auto& range : styles) {
            if (range.bold) {
                check(layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, {range.start, range.length}), "Set row weight");
            }
            if (range.italic) {
                check(layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, {range.start, range.length}), "Set row italic");
            }
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
            if (!inserted) {
                entry->second = std::min(entry->second, cluster.natural_left);
            }
        }
        for (auto& cluster : result.clusters) {
            const auto key = (std::uint64_t(cluster.column) << 32) | cluster.end;
            cluster.x = float(cluster.column * cw) + cluster.natural_left - group_left.at(key);
            if (cluster.bidi & 1) {
                cluster.x += cluster.advance_total;
            }
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
                    prior.glyphs.insert(prior.glyphs.end(), cluster.glyphs.begin(), cluster.glyphs.end());
                    prior.advances.insert(prior.advances.end(), cluster.advances.begin(), cluster.advances.end());
                    prior.offsets.insert(prior.offsets.end(), cluster.offsets.begin(), cluster.offsets.end());
                    prior.end = cluster.end;
                    continue;
                }
            }
            batches.push_back(std::move(cluster));
        }
        result.clusters = std::move(batches);
        result.bytes += result.clusters.capacity() * sizeof(GlyphCluster);
        for (const auto& cluster : result.clusters) {
            result.bytes += cluster.glyphs.capacity() * sizeof(UINT16) +
                cluster.advances.capacity() * sizeof(FLOAT) +
                cluster.offsets.capacity() * sizeof(DWRITE_GLYPH_OFFSET);
        }
        ++counters.shaped_rows;
        return result;
    }

    void trim_shapes(std::size_t incoming) {
        while (!shapes.empty() && counters.retained_bytes + incoming > shape_cache_budget) {
            const auto oldest = std::min_element(shapes.begin(), shapes.end(),
                [](const auto& a, const auto& b) {
                    return a.second.touched < b.second.touched;
                });
            counters.retained_bytes -= oldest->second.bytes;
            shapes.erase(oldest);
        }
    }

    const ShapedRow& shape(
        RowShapeRequest request,
        const FrameRow& row,
        std::uint64_t tick,
        ShapedRow& temporary) {
        const RowKey key{request.pane_id, row.id.words[0], row.id.words[1], row.generation,
                         font_generation, request.columns, request.cell_width, request.cell_height};
        if (auto found = shapes.find(key); found != shapes.end()) {
            ++counters.cache_hits;
            found->second.touched = tick;
            return found->second;
        }
        temporary = build_shape(row, request.columns, request.cell_width, request.cell_height);
        if (!shape_cache_budget || temporary.bytes > shape_cache_budget) {
            return temporary;
        }
        trim_shapes(temporary.bytes);
        temporary.touched = tick;
        const auto bytes = temporary.bytes;
        auto [entry, inserted] = shapes.emplace(key, std::move(temporary));
        if (inserted) {
            counters.retained_bytes += bytes;
        }
        return entry->second;
    }
};

TextResources::TextResources(std::size_t budget)
    : impl_(std::make_unique<Impl>(budget)) {}

TextResources::~TextResources() = default;

void TextResources::validate_font(const FontOptions& font) {
    if (font.family.empty() || font.family.size() > 256 || font.locale.size() > 85 ||
        !std::isfinite(font.size_dip) || font.size_dip < 4 || font.size_dip > 128) {
        throw std::invalid_argument("Invalid terminal font");
    }
}

bool TextResources::configure(const FontOptions& font, float dpi) {
    validate_font(font);
    if (!std::isfinite(dpi) || dpi < 48 || dpi > 768) {
        throw std::invalid_argument("Invalid surface DPI");
    }
    auto& state = *impl_;
    if (state.configured && state.font == font && state.dpi == dpi) {
        return false;
    }
    state.font = font;
    state.dpi = dpi;
    state.measure_font();
    state.configured = true;
    return true;
}

bool TextResources::set_font(const FontOptions& font) {
    return configure(font, impl_->dpi);
}

bool TextResources::set_dpi(float dpi) {
    return configure(impl_->font, dpi);
}

CellMetrics TextResources::metrics() const noexcept {
    return impl_->cell;
}

TextResourceStats TextResources::stats() const noexcept {
    return impl_->counters;
}

const ShapedRow& TextResources::shape(
    RowShapeRequest request,
    const FrameRow& row,
    std::uint64_t render_tick,
    ShapedRow& scratch) {
    return impl_->shape(request, row, render_tick, scratch);
}

bool TextResources::supports_color_glyphs() const noexcept {
    return bool(impl_->write4);
}

HRESULT TextResources::color_glyph_layers(
    const DWRITE_GLYPH_RUN& run,
    float origin_x,
    float origin_y,
    Com<IDWriteColorGlyphRunEnumerator1>& layers) const noexcept {
    if (!impl_->write4) {
        return E_NOINTERFACE;
    }
    constexpr auto formats = static_cast<DWRITE_GLYPH_IMAGE_FORMATS>(
        DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE | DWRITE_GLYPH_IMAGE_FORMATS_CFF |
        DWRITE_GLYPH_IMAGE_FORMATS_COLR | DWRITE_GLYPH_IMAGE_FORMATS_SVG |
        DWRITE_GLYPH_IMAGE_FORMATS_PNG | DWRITE_GLYPH_IMAGE_FORMATS_JPEG |
        DWRITE_GLYPH_IMAGE_FORMATS_TIFF | DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8);
    return impl_->write4->TranslateColorGlyphRun({origin_x, origin_y}, &run, nullptr, formats,
        DWRITE_MEASURING_MODE_NATURAL, nullptr, 0, layers.put());
}

void TextResources::enable_teardown_trace() noexcept {
    impl_->write_factory.teardown_label =  "renderer write_factory Release";
    impl_->write4.teardown_label =  "renderer write4 Release";
    impl_->terminal_format.teardown_label =  "renderer terminal_format Release";
    impl_->typography.teardown_label =  "renderer typography Release";
    impl_->bundled_fonts.teardown_label =  "renderer bundled_fonts Release";
}

Com<IDWriteTextLayout> TextResources::chrome_paint_layout(const ChromeText& label) const {
    const auto text = utf16(label.text);
    auto format = impl_->chrome_format(label, "Create chrome font");
    const auto alignment = label.alignment == ChromeTextAlignment::center ? DWRITE_TEXT_ALIGNMENT_CENTER :
        label.alignment == ChromeTextAlignment::trailing ? DWRITE_TEXT_ALIGNMENT_TRAILING :
        DWRITE_TEXT_ALIGNMENT_LEADING;
    check(format->SetTextAlignment(alignment), "Set chrome alignment");
    check(format->SetWordWrapping(label.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP),
          "Set chrome wrapping");
    check(format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER), "Center chrome baseline");

    // DirectWrite owns grapheme/cluster-safe end truncation and the real
    // omission glyph; never shorten UTF-8 strings or clip a partial glyph.
    Com<IDWriteInlineObject> ellipsis;
    check(impl_->write_factory->CreateEllipsisTrimmingSign(format.get(), ellipsis.put()), "Create chrome ellipsis");
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    check(format->SetTrimming(&trimming, ellipsis.get()), "Set chrome end ellipsis");

    Com<IDWriteTextLayout> layout;
    check(impl_->write_factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.get(),
          label.rect.width, label.rect.height, layout.put()), "Create chrome layout");
    return layout;
}

PixelRect TextResources::measure_chrome_text(const ChromeText& label) const {
    if (label.text.empty() || label.text.size() > 32768 || label.family.empty() || label.family.size() > 256 ||
        !std::isfinite(label.size_dip) || label.size_dip < 1 || label.size_dip > 256 ||
        !std::isfinite(label.rect.width) || !std::isfinite(label.rect.height) ||
        label.rect.width <= 0 || label.rect.height <= 0) {
        return {};
    }

    const auto text = utf16(label.text);
    auto format = impl_->chrome_format(label, "Measure chrome font");
    check(format->SetWordWrapping(label.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP),
          "Measure chrome wrapping");
    Com<IDWriteTextLayout> layout;
    check(impl_->write_factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.get(),
          label.rect.width, label.rect.height, layout.put()), "Measure chrome layout");
    DWRITE_TEXT_METRICS metrics{};
    check(layout->GetMetrics(&metrics), "Read chrome metrics");
    return {0, 0, metrics.widthIncludingTrailingWhitespace, metrics.height};
}

std::vector<std::string> TextResources::wrap_chrome_text(const ChromeText& label) const {
    if (label.text.empty() || label.text.size() > 4u * 1024u * 1024u ||
        label.family.empty() || label.family.size() > 256 ||
        !std::isfinite(label.size_dip) || label.size_dip < 1 || label.size_dip > 256 ||
        !std::isfinite(label.rect.width) || label.rect.width <= 0) {
        return {};
    }

    const auto text = utf16(label.text);
    auto format = impl_->chrome_format(label, "Wrap chrome font");
    check(format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP), "Wrap chrome words");
    Com<IDWriteTextLayout> layout;
    check(impl_->write_factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.get(),
          label.rect.width, 1.0e8f, layout.put()), "Wrap chrome layout");

    UINT32 count{};
    const auto hr = layout->GetLineMetrics(nullptr, 0, &count);
    if (hr != E_NOT_SUFFICIENT_BUFFER) {
        check(hr, "Count chrome lines");
    }
    if (count > text.size() + 1) {
        throw std::runtime_error("Invalid chrome line count");
    }
    std::vector<DWRITE_LINE_METRICS> metrics(count);
    check(layout->GetLineMetrics(metrics.data(), count, &count), "Read chrome lines");

    std::vector<std::string> lines;
    lines.reserve(count);
    std::size_t offset = 0;
    for (const auto& metric : metrics) {
        if (metric.newlineLength > metric.length || metric.length > text.size() - offset) {
            throw std::runtime_error("Invalid chrome line boundary");
        }
        const auto length = metric.length - metric.newlineLength;
        if (length && text[offset + length - 1] >= 0xd800 && text[offset + length - 1] <= 0xdbff) {
            throw std::runtime_error("Chrome line split a Unicode scalar");
        }
        const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data() + offset,
                                                  static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        if (length && !required) {
            throw std::runtime_error("Invalid Unicode chrome line");
        }
        std::string line(static_cast<std::size_t>(required), '\0');
        if (required && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data() + offset,
                                           static_cast<int>(length), line.data(), required, nullptr, nullptr)) {
            throw std::runtime_error("Could not encode chrome line");
        }
        lines.push_back(std::move(line));
        offset += metric.length;
    }
    return lines;
}

} // namespace stormtty::windows
