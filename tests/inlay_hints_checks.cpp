// Inlay-hint checks for the M9 hint module.
//
// Byte-offset, LSP-agnostic: exercises expected-closer hints at block openers
// (keyword and preprocessor blocks), the single-line skip, and the cosmetic
// suffix-typed `dim` inferred-type hints.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "inlay_hints.h"
#include "resolve.h"
#include "symbols.h"

using namespace fblang;

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

int main() {
  std::string const src = "sub greet()\n"
                          "end sub\n"
                          "for i = 1 to 3\n"
                          "next\n"
                          "#if X\n"
                          "#endif\n"
                          "#macro m()\n"
                          "#endmacro\n"
                          "if x then print x\n"
                          "dim t$\n"
                          "dim u$ as string\n"
                          "dim p%\n"
                          "dim q&\n"
                          "dim r!\n"
                          "dim s#\n";
  AnalyzedDoc const doc = analyze(src);
  std::vector<InlayHintItem> const hints = inlayHints(doc, src);

  auto hasAt = [&](std::uint32_t pos, std::string const &label) {
    for (InlayHintItem const &h : hints) {
      if (h.bytePos == pos && h.label == label) {
        return true;
      }
    }
    return false;
  };
  // Offset of the newline ending the line that contains `needle`.
  auto lineEnd = [&](std::string const &needle) -> std::uint32_t {
    std::size_t const beg = src.find(needle);
    CHECK(beg != std::string::npos);
    return static_cast<std::uint32_t>(src.find('\n', beg));
  };
  // Offset just after the whole declaration text (== the name token's end).
  auto afterDecl = [&](std::string const &decl) -> std::uint32_t {
    std::size_t const beg = src.find(decl);
    CHECK(beg != std::string::npos);
    return static_cast<std::uint32_t>(beg + decl.size());
  };

  // Expected closers, anchored at the end of the opener's line.
  CHECK(hasAt(lineEnd("sub greet()"), "END SUB"));
  CHECK(hasAt(lineEnd("for i = 1"), "NEXT"));
  CHECK(hasAt(lineEnd("#if X"), "#ENDIF"));
  CHECK(hasAt(lineEnd("#macro m()"), "#ENDMACRO"));

  // Suffix-typed declarations without an AS clause get an inferred type.
  CHECK(hasAt(afterDecl("dim t$"), "As String"));
  CHECK(hasAt(afterDecl("dim p%"), "As Integer"));
  CHECK(hasAt(afterDecl("dim q&"), "As Long"));
  CHECK(hasAt(afterDecl("dim r!"), "As Single"));
  CHECK(hasAt(afterDecl("dim s#"), "As Double"));

  // A suffix-typed declaration WITH an AS clause carries no inferred hint.
  CHECK(!hasAt(afterDecl("dim u$"), "As String"));

  // The single-line `if x then print x` is not a block: no closer hint.
  CHECK(!hasAt(lineEnd("if x then print x"), "END IF"));

  // Exactly four closers plus five inferred-type hints.
  CHECK(hints.size() == 9);

  if (failures == 0) {
    std::printf("inlay_hints_checks: all passed\n");
    return 0;
  }
  std::printf("inlay_hints_checks: %d failures\n", failures);
  return 1;
}
