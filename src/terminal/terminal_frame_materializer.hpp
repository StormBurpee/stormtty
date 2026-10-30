#pragma once

#include "stormtty/terminal.hpp"
#include <ghostty/vt.h>

namespace stormtty {

// Private materialization owner, used only inside TerminalCore's actor operation.
// Core checks thread/callback failures and owns terminal mutation, render holds
// and animation timing. Terminal and image views are borrowed synchronously;
// capture never exposes them or yields before copying their data into owned rows
// and pixels. This owner must be destroyed before the terminal it samples.
class TerminalFrameMaterializer final {
public:
    TerminalFrameMaterializer() = default;
    ~TerminalFrameMaterializer() = default;
    TerminalFrameMaterializer(const TerminalFrameMaterializer&) = delete;
    TerminalFrameMaterializer& operator=(const TerminalFrameMaterializer&) = delete;
    TerminalFrameMaterializer(TerminalFrameMaterializer&&) = delete;
    TerminalFrameMaterializer& operator=(TerminalFrameMaterializer&&) = delete;

    // Called once by Core. Explicit allocation retains terminal/encoder creation order.
    void initialize();
    GhosttyRenderStateColors initial_colors() const;

    FramePtr capture(GhosttyTerminal terminal, TerminalSize geometry,
                     std::uint64_t image_budget_bytes);
    FramePtr last_frame() const { return frame_; }

private:
    template<class T, void (*Free)(T)>
    struct Handle {
        T value{};

        Handle() = default;
        ~Handle() { if (value) Free(value); }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

        operator T() const noexcept { return value; }
    };

    void capture_images(GhosttyTerminal terminal, std::uint64_t image_budget_bytes,
                        RenderFrame& target);

    Handle<GhosttyRenderState, ghostty_render_state_free> render_;
    Handle<GhosttyRenderStateRowIterator, ghostty_render_state_row_iterator_free> rows_;
    Handle<GhosttyRenderStateRowCells, ghostty_render_state_row_cells_free> cells_;
    Handle<GhosttyRenderStateImageIterator, ghostty_render_state_image_iterator_free> images_;
    FramePtr frame_;
    std::uint64_t frame_generation_{}, row_generation_{};
};

} // namespace stormtty
