#pragma once
#include "stormtty/renderer.hpp"
#include <algorithm>

namespace stormtty {
// Presentation only: no predicted characters, delayed input or VT mutations.
// A short destination-cell reveal keeps block-cursor inverse glyph clipping exact.
class CursorMotion {
public:
    void input(std::uint64_t now) noexcept { armed_=true; input_at_=now; }
    void reset() noexcept { *this=CursorMotion{}; }
    bool active(std::uint64_t now) const noexcept { return moving_ && now>=start_ && now-start_<85; }
    float amount(std::uint64_t now) const noexcept {
        if (!active(now)) return 1;
        const float remaining=1-float(now-start_)/85;
        return 1-.42f*remaining*remaining*remaining;
    }
    float observe(const RenderPane& pane,std::uint64_t now,bool allowed) noexcept {
        const auto* f=pane.frame.get();
        const bool selected=f && std::any_of(f->lines.begin(),f->lines.end(),[](const auto& r){return r && r->selection.has_value();});
        if (!allowed || !f || !pane.focused || !f->cursor.visible || f->alternate_screen || selected) {
            reset(); return 1;
        }
        const bool same_view=valid_ && id_==pane.id && bounds_==pane.content && columns_==f->columns && rows_==f->rows && cw_==f->cell_width && ch_==f->cell_height;
        const auto* row=f->cursor.row<f->lines.size()?f->lines[f->cursor.row].get():nullptr;
        const bool echoed=row && row->id==row_id_ && row->generation!=row_generation_;
        const bool changed=!same_view || cursor_!=f->cursor;
        if (changed) {
            const int delta=int(f->cursor.column)-int(cursor_.column);
            const bool small=same_view && cursor_.visible && cursor_.row==f->cursor.row && cursor_.shape==f->cursor.shape && delta>0 && delta<=2;
            moving_=small && echoed && armed_ && now>=input_at_ && now-input_at_<=150;
            start_=now; armed_=false;
        }
        if (!pane.cursor_on) moving_=false; // Blink hides presentation, not the cursor baseline.
        if (armed_ && now-input_at_>150) armed_=false;
        valid_=true; id_=pane.id; bounds_=pane.content; columns_=f->columns; rows_=f->rows; cw_=f->cell_width; ch_=f->cell_height; cursor_=f->cursor;
        if (row) { row_id_=row->id; row_generation_=row->generation; }
        return amount(now);
    }
private:
    bool armed_{},moving_{},valid_{};
    std::uint64_t input_at_{},start_{},id_{};
    std::uint16_t columns_{},rows_{},cw_{},ch_{};
    PixelRect bounds_{};
    FrameCursor cursor_{};
    RowId row_id_{};
    std::uint64_t row_generation_{};
};
}
