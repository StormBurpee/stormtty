#pragma once
#include "stormtty/terminal.hpp"
#include <cstdint>

namespace stormtty::macos {
// Apple virtual key codes; physical identity is separate from keyboard-layout
// text, which comes only from NSTextInputClient committed text.
constexpr Key physical_key(std::uint16_t code) noexcept {
    switch(code) {
    case 0:return Key::a;case 1:return Key::s;case 2:return Key::d;case 3:return Key::f;
    case 4:return Key::h;case 5:return Key::g;case 6:return Key::z;case 7:return Key::x;
    case 8:return Key::c;case 9:return Key::v;case 11:return Key::b;case 12:return Key::q;
    case 13:return Key::w;case 14:return Key::e;case 15:return Key::r;case 16:return Key::y;
    case 17:return Key::t;case 18:return Key::digit1;case 19:return Key::digit2;case 20:return Key::digit3;
    case 21:return Key::digit4;case 22:return Key::digit6;case 23:return Key::digit5;case 24:return Key::equal;
    case 25:return Key::digit9;case 26:return Key::digit7;case 27:return Key::minus;case 28:return Key::digit8;
    case 29:return Key::digit0;case 30:return Key::bracket_right;case 31:return Key::o;case 32:return Key::u;
    case 33:return Key::bracket_left;case 34:return Key::i;case 35:return Key::p;case 36:return Key::enter;
    case 37:return Key::l;case 38:return Key::j;case 39:return Key::quote;case 40:return Key::k;
    case 41:return Key::semicolon;case 42:return Key::backslash;case 43:return Key::comma;case 44:return Key::slash;
    case 45:return Key::n;case 46:return Key::m;case 47:return Key::period;case 48:return Key::tab;
    case 49:return Key::space;case 50:return Key::backquote;case 51:return Key::backspace;case 53:return Key::escape;
    case 65:return Key::numpad_decimal;case 67:return Key::numpad_multiply;case 69:return Key::numpad_add;
    case 75:return Key::numpad_divide;case 76:return Key::numpad_enter;case 78:return Key::numpad_subtract;
    case 82:return Key::numpad0;case 83:return Key::numpad1;case 84:return Key::numpad2;case 85:return Key::numpad3;
    case 86:return Key::numpad4;case 87:return Key::numpad5;case 88:return Key::numpad6;case 89:return Key::numpad7;
    case 91:return Key::numpad8;case 92:return Key::numpad9;case 96:return Key::f5;case 97:return Key::f6;
    case 98:return Key::f7;case 99:return Key::f3;case 100:return Key::f8;case 101:return Key::f9;
    case 103:return Key::f11;case 109:return Key::f10;case 111:return Key::f12;case 114:return Key::insert;
    case 115:return Key::home;case 116:return Key::page_up;case 117:return Key::del;case 118:return Key::f4;
    case 119:return Key::end;case 120:return Key::f2;case 121:return Key::page_down;case 122:return Key::f1;
    case 123:return Key::left;case 124:return Key::right;case 125:return Key::down;case 126:return Key::up;
    default:return Key::unidentified;
    }
}
constexpr bool direct_key(Key key) noexcept {
    return key==Key::enter||key==Key::tab||key==Key::backspace||key==Key::escape||
        (key>=Key::insert&&key<=Key::f12)||key==Key::numpad_enter;
}
}
