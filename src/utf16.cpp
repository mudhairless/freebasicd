#include "utf16.h"

#include <cstddef>

namespace fblang {

namespace {

// Width in UTF-16 code units of the code point starting at text[i]. Advances
// i past the whole code point (or past a single malformed byte).
int utf16UnitsOfCodePoint(std::string_view text, std::size_t& i)
{
    unsigned char const c = static_cast<unsigned char>(text[i]);
    if (c < 0x80)
    {
        ++i;
        return 1;
    }
    int len;
    std::uint32_t cp;
    if (c >= 0xF0)
    {
        len = 4;
        cp = c & 0x07U;
    }
    else if (c >= 0xE0)
    {
        len = 3;
        cp = c & 0x0FU;
    }
    else if (c >= 0xC0)
    {
        len = 2;
        cp = c & 0x1FU;
    }
    else
    {
        ++i;  // stray continuation byte: count as one unit
        return 1;
    }
    if (i + static_cast<std::size_t>(len) > text.size())
    {
        ++i;  // truncated sequence at buffer end
        return 1;
    }
    for (int k = 1; k < len; ++k)
    {
        cp = (cp << 6) | static_cast<std::uint32_t>(text[i + static_cast<std::size_t>(k)] & 0x3FU);
    }
    i += static_cast<std::size_t>(len);
    return cp >= 0x10000U ? 2 : 1;
}

}  // namespace

lsPosition utf16Position(std::string_view text, std::uint32_t byteOffset)
{
    std::size_t const clipped = byteOffset < text.size() ? byteOffset : text.size();

    unsigned line = 0;
    std::size_t lineStart = 0;
    for (std::size_t i = 0; i < clipped; ++i)
    {
        if (text[i] == '\n')
        {
            ++line;
            lineStart = i + 1;
        }
    }

    unsigned character = 0;
    for (std::size_t i = lineStart; i < clipped;)
    {
        character += static_cast<unsigned>(utf16UnitsOfCodePoint(text, i));
    }

    lsPosition pos;
    pos.line = line;
    pos.character = character;
    return pos;
}

lsRange utf16Range(std::string_view text, std::uint32_t beginByte, std::uint32_t endByte)
{
    lsRange range;
    range.start = utf16Position(text, beginByte);
    range.end = utf16Position(text, endByte);
    return range;
}

std::uint32_t byteOffsetForUtf16Position(std::string_view text, lsPosition pos)
{
    std::size_t lineStart = 0;
    unsigned line = 0;
    while (line < pos.line)
    {
        std::size_t const nl = text.find('\n', lineStart);
        if (nl == std::string_view::npos)
        {
            return static_cast<std::uint32_t>(text.size());
        }
        lineStart = nl + 1;
        ++line;
    }

    std::size_t const lineEnd = text.find('\n', lineStart);
    std::size_t const stop = lineEnd == std::string_view::npos ? text.size() : lineEnd;

    std::size_t i = lineStart;
    unsigned units = 0;
    while (i < stop && units < pos.character)
    {
        units += static_cast<unsigned>(utf16UnitsOfCodePoint(text, i));
    }
    return static_cast<std::uint32_t>(i);
}

}  // namespace fblang