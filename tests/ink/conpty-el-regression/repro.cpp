// Diagnostic of an intentional parser-semantics difference, not an Ink pass.
#include "stormtty/terminal.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace stormtty;
int main() {
    try {
        struct Case { std::uint16_t columns; const char* bytes; const char* expected; };
        const Case cases[]{{4,"ABCD\x1b[K","ABC "}, {4,"ABCD\x1b[0K","ABC "},
                           {4,"ABCD","ABCD"}, {5,"ABCD\x1b[K","ABCD "},
                           {4,"ABCD\x1b[KZ","ABCZ"}};
        for (const auto& test : cases) {
            TerminalOptions options; options.size = {test.columns,2,8,16};
            TerminalCore terminal(options); terminal.feed(test.bytes);
            const auto frame = terminal.snapshot();
            std::string row;
            for (const auto& cell : frame->lines[0]->cells) row += cell.grapheme.empty() ? " " : cell.grapheme;
            terminal.select(0,0,test.columns-1,0);
            std::cout << "columns=" << test.columns << " row=[" << row
                      << "] selection=[" << terminal.selection_text() << "]\n";
            if (row != test.expected) throw std::runtime_error("Pinned Ghostty semantics changed; investigate");
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
