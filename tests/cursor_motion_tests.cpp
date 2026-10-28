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
    std::cout<<"PASS finite input-gated cursor settle and cancellation\n";
}
