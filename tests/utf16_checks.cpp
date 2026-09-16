// UTF-16 position/range conversion checks. LSP boundary helper used by the
// session to convert the language layer's byte offsets into client positions.

#include <cstdio>
#include <string>

#include "utf16.h"

using namespace fblang;

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static void checkPosition(std::string_view text, std::uint32_t offset,
                          unsigned line, unsigned character) {
  lsPosition p = utf16Position(text, offset);
  if (p.line != line || p.character != character) {
    std::printf("  utf16Position(%u) -> {%u,%u}, expected {%u,%u}\n", offset,
                p.line, p.character, line, character);
    CHECK(false);
  }
}

static void TestAsciiOffsets() {
  std::string text = "print \"hi\"\nprint \"yo\""; // 21 bytes, 2 lines
  checkPosition(text, 0, 0, 0);
  checkPosition(text, 5, 0, 5);
  checkPosition(text, 10, 0, 10);  // end of line 0 (before the '\n')
  checkPosition(text, 11, 1, 0);   // start of line 1
  checkPosition(text, 21, 1, 10);  // end of line 1
  checkPosition(text, 999, 1, 10); // clamped to buffer end
}

static void TestMultibyteBmpLine() {
  std::string text;
  text.reserve(48);
  text +=
      "' \xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF\n"; // こんにちは
                                                                          // (5
                                                                          // x 3
                                                                          // bytes)
  text += "print \"h\xC3\xA9llo\""; // é is 2 bytes, 1 code unit

  checkPosition(text, 17, 0, 7);  // 17 bytes -> ' + ' ' + 5 BMP chars = 7 units
  checkPosition(text, 18, 1, 0);  // start of line 1
  checkPosition(text, 32, 1, 13); // "print \"héllo\"" -> 13 UTF-16 units
}

static void TestNonBmpCountsTwoUnits() {
  std::string text = "? \"\xF0\x9F\x98\x80\""; // ? "😀"
  checkPosition(text, 7, 0, 5);                // after the 4-byte emoji
  checkPosition(text, 8, 0, 6);                // after the closing quote
}

static void TestCarriageReturnsCountAsUnits() {
  std::string text = "a\r\nb";
  checkPosition(text, 2, 0, 2);
  checkPosition(text, 3, 1, 0);
}

static void TestRanges() {
  std::string text = "print \"hi\"\nprint \"yo\"";
  lsRange r = utf16Range(text, 11, 16);
  CHECK(r.start.line == 1 && r.start.character == 0);
  CHECK(r.end.line == 1 && r.end.character == 5);
}

static void TestPositionRoundTrip() {
  std::string text;
  text.reserve(48);
  text += "' \xE3\x81\x93\xE3\x81\xAB\n"; // こに  (2 x 3 bytes)
  text += "print \"h\xC3\xA9llo\"";       // é is 2 bytes, 1 code unit

  checkPosition(text, byteOffsetForUtf16Position(text, lsPosition(0, 5)), 0, 4);
  checkPosition(text, byteOffsetForUtf16Position(text, lsPosition(1, 13)), 1,
                13);
  checkPosition(text, byteOffsetForUtf16Position(text, lsPosition(1, 999)), 1,
                13);
  CHECK(byteOffsetForUtf16Position(text, lsPosition(9, 0)) == text.size());
}

int main() {
  TestAsciiOffsets();
  TestMultibyteBmpLine();
  TestNonBmpCountsTwoUnits();
  TestCarriageReturnsCountAsUnits();
  TestRanges();
  TestPositionRoundTrip();
  std::printf("utf16_checks: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
