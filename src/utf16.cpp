#include "utf16.h"

#include <cstddef>

// UTF-8 lead-byte thresholds, payload masks, and the UTF-16 surrogate-pair
// boundary. A sequence's lead byte carries the codepoint's high bits (5/4/3
// for 2/3/4-byte sequences); each continuation byte adds 6 more.
#define UTF8_MIN_MULTIBYTE 0x80          // first lead byte of a multi-byte seq.
#define UTF8_LEAD2_MIN 0xC0              // first lead byte of a 2-byte sequence
#define UTF8_LEAD3_MIN 0xE0              // first lead byte of a 3-byte sequence
#define UTF8_LEAD4_MIN 0xF0              // first lead byte of a 4-byte sequence
#define UTF8_LEAD2_PAYLOAD_MASK 0x1F     // payload bits kept in a 2-byte lead
#define UTF8_LEAD3_PAYLOAD_MASK 0x0F     // payload bits kept in a 3-byte lead
#define UTF8_LEAD4_PAYLOAD_MASK 0x07     // payload bits kept in a 4-byte lead
#define UTF8_CONT_PAYLOAD_MASK 0x3F      // payload bits kept per continuation
#define UTF8_CONT_BITS 6                 // payload bits added per continuation
#define UTF16_SURROGATE_PAIR_MIN 0x10000 // first codepoint needing 2 units

namespace fblang {

namespace {

// Width in UTF-16 code units of the code point starting at text[i]. Advances
// i past the whole code point (or past a single malformed byte).
int utf16UnitsOfCodePoint(std::string_view text, std::size_t &i) {
  unsigned char const c = static_cast<unsigned char>(text[i]);
  if (c < UTF8_MIN_MULTIBYTE) {
    ++i;
    return 1;
  }
  int len;
  std::uint32_t cp;
  if (c >= UTF8_LEAD4_MIN) {
    len = 4;
    cp = c & UTF8_LEAD4_PAYLOAD_MASK;
  } else if (c >= UTF8_LEAD3_MIN) {
    len = 3;
    cp = c & UTF8_LEAD3_PAYLOAD_MASK;
  } else if (c >= UTF8_LEAD2_MIN) {
    len = 2;
    cp = c & UTF8_LEAD2_PAYLOAD_MASK;
  } else {
    ++i; // stray continuation byte: count as one unit
    return 1;
  }
  if (i + static_cast<std::size_t>(len) > text.size()) {
    ++i; // truncated sequence at buffer end
    return 1;
  }
  for (int k = 1; k < len; ++k) {
    cp = (cp << UTF8_CONT_BITS) |
         static_cast<std::uint32_t>(text[i + static_cast<std::size_t>(k)] &
                                    UTF8_CONT_PAYLOAD_MASK);
  }
  i += static_cast<std::size_t>(len);
  return cp >= UTF16_SURROGATE_PAIR_MIN ? 2 : 1;
}

} // namespace

lsPosition utf16Position(std::string_view text, std::uint32_t byteOffset) {
  std::size_t const clipped =
      byteOffset < text.size() ? byteOffset : text.size();

  unsigned line = 0;
  std::size_t lineStart = 0;
  for (std::size_t i = 0; i < clipped; ++i) {
    if (text[i] == '\n') {
      ++line;
      lineStart = i + 1;
    }
  }

  unsigned character = 0;
  for (std::size_t i = lineStart; i < clipped;) {
    character += static_cast<unsigned>(utf16UnitsOfCodePoint(text, i));
  }

  lsPosition pos;
  pos.line = line;
  pos.character = character;
  return pos;
}

lsRange utf16Range(std::string_view text, std::uint32_t beginByte,
                   std::uint32_t endByte) {
  lsRange range;
  range.start = utf16Position(text, beginByte);
  range.end = utf16Position(text, endByte);
  return range;
}

std::uint32_t byteOffsetForUtf16Position(std::string_view text,
                                         lsPosition pos) {
  std::size_t lineStart = 0;
  unsigned line = 0;
  while (line < pos.line) {
    std::size_t const nl = text.find('\n', lineStart);
    if (nl == std::string_view::npos) {
      return static_cast<std::uint32_t>(text.size());
    }
    lineStart = nl + 1;
    ++line;
  }

  std::size_t const lineEnd = text.find('\n', lineStart);
  std::size_t const stop =
      lineEnd == std::string_view::npos ? text.size() : lineEnd;

  std::size_t i = lineStart;
  unsigned units = 0;
  while (i < stop && units < pos.character) {
    units += static_cast<unsigned>(utf16UnitsOfCodePoint(text, i));
  }
  return static_cast<std::uint32_t>(i);
}

} // namespace fblang
