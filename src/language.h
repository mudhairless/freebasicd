#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "symbols.h"

namespace fblang {

// True if `word` (lowercase, no suffix) is a reserved FreeBASIC keyword. The
// set was verified against fbc 1.10.2: each entry fails `dim <word> as
// integer`.
bool isReservedWord(std::string_view word);

// The full reserved-word catalog (lowercase, sorted). Completion iterates it
// to offer keyword items; the returned views reference static storage.
std::vector<std::string_view> reservedWords();

// URL of the FreeBASIC wiki page documenting `word`, or "" when the word is
// not a reserved keyword. Page suffixes were harvested from CatPgFullIndex;
// operator/compound keywords use their real page names (e.g. "OpNew",
// "Ifthen").
std::string keywordDocsUrl(std::string_view word);

// Built-in type names (also reserved, listed separately for completion/hover).
bool isBuiltinType(std::string_view wordLower);

// Opening keyword -> block closer facts.
struct BlockCloser {
  BlockKind kind = BlockKind::Scope;
  std::string_view
      closeWord; // how the closer is written after END, or the bare closer
  bool needsEnd = false; // closer is "END <closeWord>"
};

bool blockForOpener(std::string_view wordLower, BlockCloser *out);
bool blockForCloser(std::string_view wordLower, BlockCloser *out);

// Display form of the expected closer, e.g. "END SUB", "NEXT", "WEND".
std::string closerDisplay(const BlockCloser &closer);

// First identifier word of a preprocessor line (e.g. "#IF X" -> "if").
std::string_view preprocessorWord(std::string_view line);

// Type-suffix characters attached to identifiers/numbers.
bool isSuffixChar(char c);

// Machine-form operator table mirroring Lexer::lexSymbol() (src/lexer.cpp).
// The semantic-token classifier and the grammar generator consume it;
// lexer_checks asserts each entry lexes as TokenKind::Symbol so the two
// cannot drift. Pure punctuation (( ) , : ; [ ] { } #) is deliberately not
// in the table.
std::vector<std::string_view> symbolOperators();

// True for the 8 keyword-lexed combined assigns: and= or= xor= eqv= imp=
// mod= shl= shr= (lexer.cpp lexes them as one Keyword token).
bool isCombinedAssignKeyword(std::string_view wordLower);

// Dialect declared by a #LANG directive. Only Fb is implemented; the others
// are recognized so the server can report that it is parsing best-effort.
enum class LangMode {
  Fb,
  FbLite,
  Qb,
  Deprecated,
};

// Parse a whole preprocessor line like `#LANG "qb"`: returns true and sets
// `out` when the directive is a well-formed, known dialect.
bool langFromDirective(std::string_view line, LangMode *out);

// Parse a `$`-metacommand comment body (`'$LANG: "qb"`, `rem $LANG: "qb"`):
// returns true and sets `out` when the comment contains `$lang` (case-
// insensitive) followed by a quoted, known dialect name.
bool langFromMetaDirective(std::string_view text, LangMode *out);

const char *langName(LangMode mode);

} // namespace fblang
