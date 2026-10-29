#include "stormtty/renderer.hpp"
#include <algorithm>
// Match the Windows SDK rpcndr.h macro visible to the native window host.
#define small char
#include "stormtty/cursor_motion.hpp"
#undef small
#include <stdexcept>
#include <iostream>
using namespace stormtty;
void require(bool value) { if (!value) throw std::runtime_error("Cursor motion invariant failed"); }
int main() {
    auto f=std::make_shared<RenderFrame>(); f->columns=80; f->rows=24; f->cursor.visible=true;
    auto row=std::make_shared<FrameRow>(); f->lines.push_back(row);
    RenderPane p{1,{0,0,800,480},f}; CursorMotion m;
    require(m.observe(p,100,true)==1); m.input(110); ++row->generation; f->cursor.column=1;
    const auto initial=m.observe(p,120,true); require(initial>.57f && initial<.59f);
    require(m.observe(p,150,true)>initial && m.active(150));
    require(m.observe(p,205,true)==1 && !m.active(205));
    ++row->generation; f->cursor.column=2; require(m.observe(p,210,true)==1); // output alone never animates
    m.input(220); ++row->generation; f->cursor.column=3; require(m.observe(p,230,true)<1);
    require(m.observe(p,240,false)==1); // reduced motion / IME
    m.observe(p,250,true); m.input(260); ++row->generation; f->cursor.column=4; p.content.width=900;
    require(m.observe(p,270,true)==1); // resize
    m.input(280); ++row->generation; f->cursor.column=5; f->alternate_screen=true; require(m.observe(p,290,true)==1);
    f->alternate_screen=false; m.observe(p,300,true); m.input(310); f->cursor.row=1;
    require(m.observe(p,320,true)==1); // row jumps
    m.input(330); ++row->generation; f->cursor.column=6; p.focused=false; require(m.observe(p,340,true)==1);
    p.focused=true; f->cursor.row=0; m.observe(p,350,true); m.input(360); ++row->generation; f->cursor.column=7;
    require(m.observe(p,600,true)==1); // expired input eligibility
    m.input(610); ++row->generation; f->cursor.column=8; require(m.observe(p,620,true)<1);
    p.cursor_on=false; m.observe(p,621,true); p.cursor_on=true; m.input(622); ++row->generation; f->cursor.column=9;
    require(m.observe(p,623,true)<1); // first keystroke after ordinary blink retains baseline
    row->selection=SelectionRange{0,1};
    require(m.observe(p,630,true)==1); // selection cancels in-flight motion
    row->selection.reset();
    m.reset(); f->cursor.column=39; m.observe(p,700,true);
    // A shell repaint can publish an echoed character, then move back onto it
    // (e.g. an asynchronous line-editor redraw). Presentation must not hide or
    // postpone that authoritative retreat, nor manufacture a forward position.
    m.input(710); ++row->generation; f->cursor.column=40;
    require(m.observe(p,720,true)<1);
    const auto forward_cursor=f->cursor;
    f->cursor.column=39;
    const auto retreat_cursor=f->cursor;
    require(m.observe(p,750,true)==1 && !m.active(750));
    require(f->cursor==retreat_cursor && f->cursor!=forward_cursor);
    // Cursor-only restoration is not evidence of new input/character echo.
    f->cursor.column=40;
    require(m.observe(p,780,true)==1 && !m.active(780));
    // The same rules hold with an autosuggestion occupying destination cells.
    row->cells.resize(42); row->cells[40].grapheme="x";
    m.input(790); ++row->generation; f->cursor.column=41;
    require(m.observe(p,800,true)<1);
    f->cursor.column=40;
    require(m.observe(p,810,true)==1 && !m.active(810));
    require(row->cells[40].grapheme=="x");
    std::cout<<"PASS finite input-gated cursor settle and cancellation\n";
}
