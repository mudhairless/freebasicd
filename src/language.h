/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lexer.h"
#include "symbols.h"

namespace fblang {

// True if `word` (no suffix) is a reserved FreeBASIC keyword, matched
// case-insensitively as fbc matches them. The set was verified against
// fbc 1.10.2: each entry fails `dim <word> as integer`.
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

// True when `lowerName` (already ASCII-folded lowercase, e.g. via
// toLowerChars) is a catalogued project source/include directory name. The
// catalog pairs full names (source, include) with the common abbreviations
// (src, inc) across ~30 languages; workspace-root detection treats a directory
// holding a child by any of these names as a project root.
bool isSourceDirName(std::string_view lowerName);
bool isIncludeDirName(std::string_view lowerName);

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

// Built-in procedure/function catalog (the `keywordDocsUrl` per-word idiom,
// extended with the signature consumers need). FreeBASIC's keyword lexer and
// its standard headers both contribute names that users call without declaring
// them; the catalog is the machine form of that surface, so completion, hover,
// and signatureHelp agree on one spelling, signature, and wiki page.
enum class IntrinsicKind {
  Function,  // callable in an expression: Left$, Mid, Val, CInt, ...
  Statement, // callable in statement position: Print, Cls, Seek, ...
};

struct Intrinsic {
  std::string_view key;   // lowercase bare name, no type suffix
  bool hasDollar = false; // fb aliases left/left$ into one symbol
  IntrinsicKind kind = IntrinsicKind::Function;
  std::string_view signature; // canonical call form, one per base name
  std::string_view page;      // KeyPg suffix; "" derives via keywordDocsUrl
};

// The catalog row for `nameWithSuffix` (one trailing type-suffix char is
// stripped, so `left` and `left$` resolve to the same row), or nullptr.
Intrinsic const *intrinsicFor(std::string_view nameWithSuffix);

// Every catalog row, key-sorted. Views reference static storage.
std::vector<Intrinsic const *> intrinsics();

// Wiki URL of the intrinsic's page (explicit `page`, else the keyword rule).
std::string intrinsicDocsUrl(Intrinsic const &fn);

// Parameter labels parsed from the canonical signature: the identifier before
// ` As ` in each top-level comma-separated parameter, or `...` for a variadic
// tail. Statement rows without a `(` yield an empty list.
std::vector<std::string_view> signatureParamLabels(Intrinsic const &fn);

// True when the cursor at `off` sits where a statement may start: right after
// a logical newline, a `:` separator, or `Then`/`Else`. Conservative by
// design — a false "expression" only withholds statement completion.
bool statementPosition(std::vector<Token> const &tokens, std::uint32_t off);

// The closer the block opened at byte offset `openerBeg` expects — "END SUB",
// "NEXT", "WEND", "#ENDIF", ... — or "" when no token starts there or the token
// opens no block. One answer for the whole server: the inlay-hint closer hints
// and the M12 `unterminated-block` quick fix must never name a closer
// differently.
std::string expectedCloserAt(std::vector<Token> const &tokens,
                             std::uint32_t openerBeg);

// Can the statement spelled by `stmt` (one logical line's tokens, first token
// first) appear in the body of a block of `kind`, whose own type is
// `enclosingTypeKey` ("" for a record block with no name)? A record or enum
// body is a member list rather than a statement list (FreeBASIC.md §7), so a
// statement it cannot accept is where the missing closer belongs (fbc anchors
// `error 19` / `error 74` on exactly that statement); every other kind accepts
// any statement, so this is true for all of them. Conservative by construction:
// anything the tables below are not sure about counts as a member, so a false
// positive here can only delay a boundary, never invent one. `enclosingTypeKey`
// exists for the one member the language cannot have — a field whose type is
// the record declaring it, fbc's `error 88` — which is a boundary for the same
// reason. Closer tokens answer true: a matching closer closes the block, and a
// mismatching one is the parser's separate evidence for the same boundary.

// Is this whole statement a closer — `END <x>`, `NEXT`, `WEND`, `LOOP` —
// rather than a statement that merely starts with one of those words? Those
// words are legal field names (`Next As Node Ptr` is the canonical linked list,
// and fbc accepts keyword field names), so the leading word alone cannot answer
// it: the parser routes on this, and a record body must not end on `next as`
// merely because `next` also closes a loop.
bool isCloserStatement(std::vector<Token> const &stmt);
bool acceptsBodyMember(BlockKind kind, std::vector<Token> const &stmt,
                       std::string_view enclosingTypeKey);

} // namespace fblang
