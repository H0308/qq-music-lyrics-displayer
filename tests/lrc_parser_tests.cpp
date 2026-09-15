#include "lyric/lrc_parser.h"

#include <cassert>

int main() {
    const auto lines = parseLrcText("[00:02.00] later\r\n"
                                   "[ti:demo]\n"
                                   "[00:01.20][00:03.00] first  \n"
                                   "[00:04.00]   \n");

    assert(lines.size() == 3);
    assert(lines[0].ms == 1200);
    assert(lines[0].text == L"first");
    assert(lines[1].ms == 2000);
    assert(lines[1].text == L"later");
    assert(lines[2].ms == 3000);
    assert(lines[2].text == L"first");
    assert(parseLrcText("[ar:artist]\n").empty());
    return 0;
}
