/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The format engine (M17): a line-oriented re-emission of the token stream.
// Every physical line is rebuilt from its significant tokens with the
// configured indent, spacing, and catalog-word case; everything the engine
// has no opinion about is copied from the original bytes.
//
// Conservative by construction. The factory config fixes indentation,
// spacing, trailing whitespace, and final newlines and rewrites no spelling;
// the shipped FBdocs preset adds the wiki's canonical spelling for keywords,
// types, and intrinsics. What is copied verbatim, deliberately:
//   - comments (line, block, doc) and every string literal;
//   - `#...` directive lines and the macro/define bodies they continue
//     (a directive ending in `_` puts the following lines through raw);
//   - whole continuation lines (a line that continues a `_`-terminated
//     logical line) — ContinuationStyle::Leave is the only mode implemented,
//     so the author's escaped-newline alignment is never disturbed;
//   - number literals and identifier spelling (IdentifierCase is Leave-only).
// The indent stack is keyword-driven (fblang's own block tables plus the
// one-line-If rule); the parser's blockRanges join in when SplitSafe and
// one-line block expansion arrive.
//
// Invariants this file must keep (M17 plan): never rewrite string sigils,
// Print separators, metacommand comments, enum trailing commas, Draw/Paint
// strings, or empty graphics argument slots; never insert or remove `_`.

#include "format/engine.h"
#include "format/spelling.h"
#include "format/tokens.h"
#include "language.h"
#include "symbols.h"

#include <algorithm>
#include <string>
#include <vector>

namespace fbformat {
namespace {

// --- token stream shaping -------------------------------------------------

// A physical line: the byte span between newlines plus the indices of its
// significant (non-whitespace) tokens. A block comment swallows its internal
// newlines into one token, so a "line" can span several display rows; its
// bytes are copied verbatim either way.
struct Line {
  size_t begin = 0;
  size_t end = 0;          // exclusive, newline not included
  std::vector<size_t> sig; // token indices
  bool crlf = false;       // the newline that closed this line was CRLF
};

std::vector<Line> splitLines(std::vector<Token> const &toks,
                             std::string const &content) {
  std::vector<Line> lines;
  Line cur;
  cur.begin = 0;
  for (size_t i = 0; i < toks.size(); ++i) {
    Token const &t = toks[i];
    if (t.kind == TokenKind::Newline) {
      cur.end = t.start;
      lines.push_back(std::move(cur));
      cur = Line{};
      cur.begin = t.end;
      cur.crlf = t.text == "\r\n";
      continue;
    }
    if (t.kind != TokenKind::Whitespace) {
      cur.sig.push_back(i);
    }
  }
  if (cur.begin < content.size() || !cur.sig.empty()) {
    cur.end = content.size();
    lines.push_back(std::move(cur));
  }
  return lines;
}

// --- word sets ------------------------------------------------------------

// Words after which a symbolic `-`/`+` is a sign rather than a binary
// operator. Kept explicit: "true"/"false" are reserved words but `True - 1`
// is arithmetic, so "any reserved word" would be wrong.
bool unaryContextWord(std::string_view lower) {
  return lower == "as" || lower == "then" || lower == "to" || lower == "step" ||
         lower == "case" || lower == "else" || lower == "elseif" ||
         lower == "if" || lower == "while" || lower == "until" ||
         lower == "return" || lower == "print" || lower == "lprint" ||
         lower == "using";
}

bool binaryOpWord(std::string_view lower) {
  return lower == "and" || lower == "or" || lower == "xor" || lower == "eqv" ||
         lower == "imp" || lower == "mod" || lower == "shl" || lower == "shr" ||
         lower == "is";
}

// Words that keep a space on both sides even when punctuation abuts them
// (`Case Is > 5`, `For i = 1 To 10`).
bool spacedWord(std::string_view lower) {
  return lower == "as" || lower == "to" || lower == "step" || lower == "then" ||
         lower == "else" || lower == "elseif" || lower == "case" ||
         lower == "is";
}

bool controlParenWord(std::string_view lower) {
  return lower == "if" || lower == "elseif" || lower == "while" ||
         lower == "until";
}

bool declarationStartWord(std::string_view lower) {
  return lower == "sub" || lower == "function" || lower == "property" ||
         lower == "operator" || lower == "constructor" ||
         lower == "destructor" || lower == "declare";
}

// The eight combined assigns the real lexer keeps as one keyword token
// ("and=", "or=", ...); the formatter's tokenizer splits them into word + "=".
bool combinedAssignWord(std::string_view lower) {
  return lower == "and" || lower == "or" || lower == "xor" || lower == "eqv" ||
         lower == "imp" || lower == "mod" || lower == "shl" || lower == "shr";
}

bool suffixSigil(Token const &t) {
  return t.kind == TokenKind::Operator &&
         (t.text == "$" || t.text == "%" || t.text == "&" || t.text == "!" ||
          t.text == "#");
}

bool isSymbolicBinaryOp(std::string const &text) {
  return text == "+" || text == "-" || text == "*" || text == "/" ||
         text == "\\" || text == "^" || text == "&" || text == "=" ||
         text == "<>" || text == "<" || text == ">" || text == "<=" ||
         text == ">=";
}

// --- case transforms ------------------------------------------------------

std::string applyCase(CaseStyle style, std::string const &lower) {
  switch (style) {
  case CaseStyle::lower:
    return lower;
  case CaseStyle::UPPER: {
    std::string out = lower;
    for (char &c : out) {
      if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
      }
    }
    return out;
  }
  case CaseStyle::Title:
    return titleCase(lower);
  case CaseStyle::Canonical: {
    std::string_view canon = canonicalSpelling(lower);
    return canon.empty() ? titleCase(lower) : std::string(canon);
  }
  case CaseStyle::Leave:
    break;
  }
  return lower;
}

// The configured case for one identifier-shaped token. Precedence is
// specificity: boolean literal, then builtin type, then intrinsic, then
// reserved keyword — so `String(n)` picks up TypeNameCase; a config that
// wants the intrinsic spelling sets both to the same value, as every shipped
// preset does. The type catalog is asked *positionally* (isTypeWordAt):
// `sub` is a type only after `as`, a procedure keyword everywhere else.
// Ordinary identifiers never change (IdentifierCase is Leave-only).
std::string caseWord(std::string const &word, std::string const &prevLower,
                     FormatConfig const &config) {
  std::string lower = fblang::toLowerChars(word);
  if (lower == "true" || lower == "false") {
    return config.booleanLiteralCase == CaseStyle::Leave
               ? word
               : applyCase(config.booleanLiteralCase, lower);
  }
  if (fblang::isTypeWordAt(lower, prevLower)) {
    return config.typeNameCase == CaseStyle::Leave
               ? word
               : applyCase(config.typeNameCase, lower);
  }
  if (fblang::intrinsicFor(word) != nullptr) {
    return config.intrinsicCase == CaseStyle::Leave
               ? word
               : applyCase(config.intrinsicCase, lower);
  }
  if (fblang::isReservedWord(lower)) {
    return config.keywordCase == CaseStyle::Leave
               ? word
               : applyCase(config.keywordCase, lower);
  }
  return word;
}

// --- the emitter ----------------------------------------------------------

// Rebuilds one code line from its significant tokens. Holds the gap rules'
// state: the last significant token emitted, the paren-context stack, and
// the line's first word (which tells a call paren from a declaration paren).
struct Emitter {
  FormatConfig const &config;
  std::vector<Token> const &toks;

  bool hasPrev = false;
  TokenKind prevKind = TokenKind::Unknown;
  std::string prevText;
  std::string prevLower;
  bool prevHadWs = false; // whitespace preceded the previous token in source
  bool prevWasUnary = false;
  std::string firstWordLower;

  // What opened each currently-open paren: 'c' control, 'd' declaration
  // parameter list, 'x' call/subscript/other.
  std::vector<char> parenCtx;

  bool prevIsPlainIdent() const {
    return hasPrev && prevKind == TokenKind::Identifier &&
           !fblang::isReservedWord(prevLower);
  }

  char contextForOpenParen() {
    if (hasPrev && prevKind == TokenKind::Identifier) {
      if (controlParenWord(prevLower)) {
        return 'c';
      }
      if (declarationStartWord(firstWordLower) && prevLower != firstWordLower &&
          !fblang::isReservedWord(prevLower)) {
        return 'd';
      }
      if (!fblang::isReservedWord(prevLower)) {
        return 'x';
      }
    }
    return 'x';
  }

  bool declNamePrev() {
    return prevIsPlainIdent() && declarationStartWord(firstWordLower) &&
           prevLower != firstWordLower;
  }

  bool unaryContext() const {
    if (!hasPrev) {
      return true;
    }
    if (prevKind == TokenKind::Operator) {
      return true;
    }
    if (prevKind == TokenKind::Identifier && unaryContextWord(prevLower)) {
      return true;
    }
    return false;
  }

  char parensInnerGap() const {
    char ctx = parenCtx.empty() ? 'x' : parenCtx.back();
    switch (ctx) {
    case 'c':
      return config.spacesInParensInConditionalStatements ? ' ' : '\0';
    case 'd':
      return config.spacesInParensInParameterLists ? ' ' : '\0';
    default:
      return config.spacesInParensInCallParens ? ' ' : '\0';
    }
  }

  // The gap before token `cur` (at index `i` of the line's significant
  // list). `hadWs` is whether whitespace preceded it in the source;
  // `nextSig` is the following significant token, for the sign-before-paren
  // case (`Line -(10,10)`).
  char gapBefore(size_t i, bool hadWs, Token const *nextSig) {
    Token const &cur = toks[i];

    // Type-suffix sigils attach to the preceding word with no space.
    if (suffixSigil(cur) && !hadWs && hasPrev &&
        (prevKind == TokenKind::Identifier || prevKind == TokenKind::Number ||
         prevKind == TokenKind::Operator)) {
      return '\0';
    }
    // A number directly after `#` is the Print/file channel: attached.
    if (cur.kind == TokenKind::Number && hasPrev && prevText == "#") {
      return '\0';
    }
    // "x and= 5": the real lexer keeps the combined assign as one token.
    if (cur.text == "=" && hasPrev && prevKind == TokenKind::Identifier &&
        combinedAssignWord(prevLower) && !hadWs) {
      return '\0';
    }
    // `Step (x,y)` vs `Step(x,y)`: Leave preserves the author's gap.
    if (cur.text == "(" && hasPrev && prevKind == TokenKind::Identifier &&
        prevLower == "step") {
      if (config.stepPrefixSpacing == StepPrefix::Attached) {
        return '\0';
      }
      if (config.stepPrefixSpacing == StepPrefix::Spaced) {
        return ' ';
      }
      return hadWs ? ' ' : '\0';
    }

    // Two adjacent commas are an elided argument slot (graphics, file I/O);
    // the author's spacing there is the slot's visual structure, so it is
    // preserved as written (invariant: never rewrite empty argument slots).
    if (cur.text == "," && hasPrev && prevText == ",") {
      return hadWs ? ' ' : '\0';
    }
    // Comma and colon have their own before/after switches.
    if (cur.text == ",") {
      return config.spaceBeforeComma ? ' ' : '\0';
    }
    if (cur.text == ":") {
      return config.spaceBeforeColon ? ' ' : '\0';
    }
    if (hasPrev && prevText == ",") {
      return config.spaceAfterComma ? ' ' : '\0';
    }
    if (hasPrev && prevText == ":") {
      return config.spaceAfterStatementSeparator ? ' ' : '\0';
    }

    // Print `;` separators stay tight; so do member-access dots.
    if (cur.text == ";" || (hasPrev && prevText == ";")) {
      return '\0';
    }
    if (cur.text == "." || (hasPrev && prevText == ".")) {
      return (config.spaceAroundMemberAccess == SpaceAroundMemberAccess::None ||
              config.spaceAroundMemberAccess == SpaceAroundMemberAccess::Leave)
                 ? '\0'
                 : ' ';
    }

    // Parens: the before-gap is SpaceBeforeParens plus the fine-grained
    // overrides; the after-gap comes from the paren-context stack.
    if (cur.text == "(") {
      char ctx = contextForOpenParen();
      bool before = false;
      switch (config.spaceBeforeParens) {
      case SpaceBeforeParens::Always:
        before = true;
        break;
      case SpaceBeforeParens::ControlStatements:
        before = ctx == 'c';
        break;
      case SpaceBeforeParens::FunctionCall:
        before = ctx == 'x';
        break;
      case SpaceBeforeParens::FunctionDeclaration:
      case SpaceBeforeParens::FunctionDefinition:
        before = ctx == 'd';
        break;
      case SpaceBeforeParens::Never:
        break;
      }
      if (!before && ctx == 'c' && hasPrev && controlParenWord(prevLower) &&
          config.spaceBeforeParensAfterControlStatements) {
        before = true;
      }
      if (!before && ctx == 'x' && prevIsPlainIdent() &&
          config.spaceBeforeParensAfterFunctionCallName) {
        before = true;
      }
      if (!before && ctx == 'd' && declNamePrev() &&
          config.spaceBeforeParensAfterFunctionDeclarationName) {
        before = true;
      }
      return before ? ' ' : '\0';
    }
    if (cur.text == ")") {
      if (hasPrev && prevText == "(") {
        return config.spacesInParensInEmptyParentheses ? ' ' : '\0';
      }
      return parensInnerGap();
    }
    if (hasPrev && prevText == "(") {
      return parensInnerGap();
    }

    // Assignment and binary operators, both symbolic and word-shaped.
    if (cur.text == "=" || cur.text == "+=" || cur.text == "-=" ||
        cur.text == "*=" || cur.text == "/=" || cur.text == "^=" ||
        cur.text == "&=") {
      return config.spaceBeforeAssignmentOperators ? ' ' : '\0';
    }
    if (hasPrev && (prevText == "=" || prevText == "+=" || prevText == "-=" ||
                    prevText == "*=" || prevText == "/=" || prevText == "^=" ||
                    prevText == "&=")) {
      return ' ';
    }
    // The operand of a sign classified as unary stays attached (or spaced,
    // per spaceAfterUnaryOperators). Checked before the binary rules so
    // `= +1` doesn't read the `+` as binary.
    if (hasPrev && (prevText == "-" || prevText == "+") && prevWasUnary) {
      return config.spaceAfterUnaryOperators ? ' ' : '\0';
    }
    // A symbolic -/+ is a sign when it starts the expression or follows an
    // operator or an unclosed paren. The sign keeps a space on its *left*
    // (the operand attaches on the right, via prevWasUnary above); contexts
    // that want them tight — after `(`, `,`, `=` — already returned earlier.
    // Sign directly before a paren is the graphics coordinate-pair form
    // (`Line -(10,10)-(20,20)`): the author's tightness is preserved.
    if ((cur.text == "-" || cur.text == "+") && nextSig != nullptr &&
        nextSig->text == "(" && nextSig->start == cur.end) {
      return hadWs ? ' ' : '\0';
    }
    if ((cur.text == "-" || cur.text == "+") && unaryContext()) {
      return ' ';
    }
    if (cur.kind == TokenKind::Identifier &&
        binaryOpWord(fblang::toLowerChars(cur.text))) {
      return config.spaceAroundBinaryOperators ? ' ' : '\0';
    }
    if (hasPrev && prevKind == TokenKind::Identifier &&
        binaryOpWord(prevLower)) {
      return config.spaceAroundBinaryOperators ? ' ' : '\0';
    }
    if (isSymbolicBinaryOp(cur.text)) {
      return config.spaceAroundBinaryOperators ? ' ' : '\0';
    }
    if (hasPrev && isSymbolicBinaryOp(prevText)) {
      return config.spaceAroundBinaryOperators ? ' ' : '\0';
    }
    if (cur.kind == TokenKind::Identifier &&
        fblang::toLowerChars(cur.text) == "not") {
      return ' ';
    }
    if (hasPrev && prevKind == TokenKind::Identifier && prevLower == "not") {
      return ' ';
    }

    // Spaced keywords keep a space even against punctuation.
    if (cur.kind == TokenKind::Identifier &&
        spacedWord(fblang::toLowerChars(cur.text))) {
      return ' ';
    }
    if (hasPrev && prevKind == TokenKind::Identifier && spacedWord(prevLower)) {
      return ' ';
    }

    // The default between words, and around comments treated as words.
    return ' ';
  }
};

// Is the line's whole shape `label: stmt` — an identifier as first token and
// a bare colon as second?
bool isLabelLine(std::vector<Token> const &toks,
                 std::vector<size_t> const &sig) {
  if (sig.size() < 2) {
    return false;
  }
  Token const &a = toks[sig[0]];
  Token const &b = toks[sig[1]];
  return a.kind == TokenKind::Identifier && b.kind == TokenKind::Operator &&
         b.text == ":";
}

// How a line's leading words shape the block stack (see the classification
// inside the emission loop).
enum class Lead { Plain, EndTwoWord, EndIf, Bare };

} // namespace

std::string formatDocument(const std::string &content,
                           const std::string &filePath,
                           const FormatConfig &config) {
  (void)filePath;
  if (config.disableFormat || content.empty()) {
    return content;
  }

  TokenStream ts(content);
  std::vector<Token> const &toks = ts.tokens();
  std::vector<Line> lines = splitLines(toks, content);

  // One newline style for the whole document unless Leave keeps per-line
  // endings: derived from the source (a file with any CRLF is CRLF).
  bool anyCRLF = content.find("\r\n") != std::string::npos;
  auto eolFor = [&](Line const &l) -> std::string {
    if (config.lineEnding == "LF") {
      return "\n";
    }
    if (config.lineEnding == "CRLF") {
      return "\r\n";
    }
    if (config.lineEnding == "Leave") {
      return l.crlf ? "\r\n" : "\n";
    }
    return anyCRLF ? "\r\n" : "\n";
  };

  // Byte spans of `out` copied verbatim (continuation lines, block-comment
  // blobs); the trailing-whitespace pass must not rewrite their interiors.
  std::string out;
  std::vector<std::pair<size_t, size_t>> verbatimSpans;
  auto appendVerbatim = [&](std::string const &text) {
    size_t begin = out.size();
    out += text;
    verbatimSpans.emplace_back(begin, out.size());
  };

  std::vector<fblang::BlockKind> stack;
  size_t pendingBlank = 0;
  bool prevContinues = false;
  bool emittedAny = false;

  auto flushBlanks = [&](std::string const &eol, bool atFileStart) {
    size_t keep = config.maxEmptyLinesToKeep > 0
                      ? static_cast<size_t>(config.maxEmptyLinesToKeep)
                      : 0;
    size_t n = std::min(pendingBlank, keep);
    if (atFileStart && !config.keepEmptyLinesAtStartOfFile) {
      n = 0;
    }
    for (size_t i = 0; i < n; ++i) {
      out += eol;
    }
    pendingBlank = 0;
  };

  for (Line const &line : lines) {
    std::vector<Token> const &T = toks;
    std::string eol = eolFor(line);

    if (line.sig.empty()) {
      ++pendingBlank;
      continue;
    }

    // A continuation of a `_`-terminated logical line: ContinuationStyle::
    // Leave copies the authored line byte for byte.
    if (prevContinues) {
      flushBlanks(eol, !emittedAny);
      appendVerbatim(content.substr(line.begin, line.end - line.begin));
      out += eol;
      size_t lastSig = line.sig.back();
      prevContinues =
          T[lastSig].kind == TokenKind::Identifier && T[lastSig].text == "_";
      continue;
    }

    Token const &first = T[line.sig.front()];
    std::string w0 = first.kind == TokenKind::Identifier
                         ? fblang::toLowerChars(first.text)
                         : "";
    // The last token that is code, not a trailing comment; structure is read
    // from it (`If x Then ' note` still opens a block).
    size_t lastCode = line.sig.back();
    while (lastCode > line.sig.front() &&
           (T[lastCode].kind == TokenKind::LineComment ||
            T[lastCode].kind == TokenKind::BlockComment)) {
      --lastCode;
    }
    std::string wLast = T[lastCode].kind == TokenKind::Identifier
                            ? fblang::toLowerChars(T[lastCode].text)
                            : "";
    std::string w1 = "";
    if (line.sig.size() > 1) {
      Token const &second = T[line.sig[1]];
      if (second.kind == TokenKind::Identifier) {
        w1 = fblang::toLowerChars(second.text);
      } else if (second.kind == TokenKind::Operator) {
        w1 = second.text;
      }
    }

    // --- how the line leads: closer form or plain statement ---
    // The bare set is fblang's isCloserStatement answer (end, endif, next,
    // wend, loop); blockForCloser alone would also claim every `End <x>`
    // closer *word*, so `Sub` would read as its own closer and the opener
    // path would never run. A closer word followed by `as` is a UDT field
    // named after it (FreeBASIC.md §7, `Next As Node Ptr`), never a closer.
    fblang::BlockCloser closerInfo{};
    Lead lead = Lead::Plain;
    if (w1 != "as") {
      if (w0 == "end") {
        lead = Lead::EndTwoWord;
      } else if (w0 == "endif") {
        lead = Lead::EndIf;
      } else if ((w0 == "next" || w0 == "wend" || w0 == "loop") &&
                 fblang::blockForCloser(w0, &closerInfo)) {
        lead = Lead::Bare;
      }
    }

    // --- indentation depth for this line ---
    int depth = static_cast<int>(stack.size());
    if (lead != Lead::Plain) {
      depth = std::max(0, depth - 1);
    } else if ((w0 == "else" || w0 == "elseif") && !stack.empty() &&
               stack.back() == fblang::BlockKind::If) {
      depth = std::max(0, depth - 1);
    } else if (w0 == "case" && !stack.empty() &&
               stack.back() == fblang::BlockKind::Select) {
      depth = std::max(0, depth - 1);
    }

    bool labelLine = lead == Lead::Plain && isLabelLine(T, line.sig);
    if (labelLine) {
      if (config.indentGotoLabels == IndentGotoLabels::OuterIndent) {
        depth = std::max(0, depth - 1);
      } else if (config.indentGotoLabels == IndentGotoLabels::InnerIndent) {
        // same depth as the statements around it
      } else {
        depth = 0;
      }
    }

    auto indentStr = [&]() -> std::string {
      if (config.useTab == UseTab::Never ||
          config.indentWidth != config.tabWidth) {
        return std::string(static_cast<size_t>(std::max(0, depth)) *
                               static_cast<size_t>(config.indentWidth),
                           ' ');
      }
      return std::string(static_cast<size_t>(std::max(0, depth)), '\t');
    };

    flushBlanks(eol, !emittedAny);

    // --- emit the line ---
    if (first.kind == TokenKind::Directive) {
      // A `#...` line: case the directive word, keep the rest raw.
      std::string raw = content.substr(line.begin, line.end - line.begin);
      size_t trim = raw.find_first_not_of(" \t");
      std::string body =
          trim == std::string::npos ? std::string() : raw.substr(trim);
      std::string lead = config.indentPPDirectives == IndentPPDirectives::None
                             ? std::string()
                             : indentStr();
      size_t hash = body.find('#');
      if (hash == std::string::npos ||
          config.directiveCase == CaseStyle::Leave) {
        out += lead + body;
      } else {
        size_t wordStart = hash + 1;
        size_t wordEnd = wordStart;
        while (wordEnd < body.size() &&
               ((body[wordEnd] >= 'a' && body[wordEnd] <= 'z') ||
                (body[wordEnd] >= 'A' && body[wordEnd] <= 'Z') ||
                body[wordEnd] == '_')) {
          ++wordEnd;
        }
        std::string word = body.substr(wordStart, wordEnd - wordStart);
        if (word.empty()) {
          out += lead + body;
        } else {
          out += lead + body.substr(0, wordStart) +
                 applyCase(config.directiveCase, fblang::toLowerChars(word)) +
                 body.substr(wordEnd);
        }
      }
      out += eol;
      // A directive ending in `_` continues onto the next lines, which are
      // macro-body bytes and go through raw.
      std::string trimmed = raw;
      while (!trimmed.empty() &&
             (trimmed.back() == ' ' || trimmed.back() == '\t')) {
        trimmed.pop_back();
      }
      prevContinues = !trimmed.empty() && trimmed.back() == '_';
      emittedAny = true;
      continue;
    }

    if (first.kind == TokenKind::Metacommand) {
      // A rare line-start `$...` metacommand (the `'$...` comment form is a
      // line comment and stays verbatim like every comment).
      std::string body = first.text;
      size_t trim = body.find_first_not_of(" \t");
      if (trim != std::string::npos) {
        body.erase(0, trim);
      }
      if (config.metacommandCase != CaseStyle::Leave && !body.empty()) {
        size_t wordStart = 1;
        size_t wordEnd = wordStart;
        while (wordEnd < body.size() &&
               ((body[wordEnd] >= 'a' && body[wordEnd] <= 'z') ||
                (body[wordEnd] >= 'A' && body[wordEnd] <= 'Z') ||
                body[wordEnd] == '_')) {
          ++wordEnd;
        }
        if (wordEnd > wordStart) {
          body = body.substr(0, wordStart) +
                 applyCase(config.metacommandCase,
                           fblang::toLowerChars(
                               body.substr(wordStart, wordEnd - wordStart))) +
                 body.substr(wordEnd);
        }
      }
      out += indentStr() + body + eol;
      emittedAny = true;
      continue;
    }

    if (first.kind == TokenKind::LineComment ||
        first.kind == TokenKind::BlockComment) {
      // A comment-only line (including `'$...` metacommand comments): the
      // comment text is never rewritten, only placed at the indent. A block
      // comment blob may span display rows; its bytes go through verbatim.
      std::string raw = content.substr(line.begin, line.end - line.begin);
      size_t trim = raw.find_first_not_of(" \t");
      std::string text =
          trim == std::string::npos ? std::string() : raw.substr(trim);
      out += indentStr();
      appendVerbatim(text);
      out += eol;
      prevContinues = false;
      emittedAny = true;
      continue;
    }

    // A code line: rebuild token by token.
    out += indentStr();
    Emitter em{config, T};
    em.firstWordLower = w0;
    for (size_t si = 0; si < line.sig.size(); ++si) {
      size_t i = line.sig[si];
      Token const &t = T[i];
      bool hadWs = false;
      if (si > 0) {
        size_t prevEnd = T[line.sig[si - 1]].end;
        hadWs = t.start > prevEnd;
      } else {
        hadWs = t.start > line.begin;
      }

      char gap = '\0';
      if (em.hasPrev) {
        Token const *nextSig =
            si + 1 < line.sig.size() ? &T[line.sig[si + 1]] : nullptr;
        gap = em.gapBefore(i, hadWs, nextSig);
      }
      // A trailing comment rides at the configured distance.
      if ((t.kind == TokenKind::LineComment ||
           t.kind == TokenKind::BlockComment) &&
          i > lastCode) {
        int n = config.spacesBeforeTrailingComments > 1
                    ? config.spacesBeforeTrailingComments
                    : 1;
        out += std::string(static_cast<size_t>(n), ' ');
      } else if (gap != '\0') {
        out += gap;
      }

      if (t.kind == TokenKind::BlockComment) {
        appendVerbatim(t.text);
      } else if (t.kind == TokenKind::Identifier) {
        out += caseWord(t.text, em.prevLower, config);
      } else if (t.kind == TokenKind::Operator && t.text == "(") {
        em.parenCtx.push_back(em.contextForOpenParen());
        out += "(";
      } else if (t.kind == TokenKind::Operator && t.text == ")") {
        if (!em.parenCtx.empty()) {
          em.parenCtx.pop_back();
        }
        out += ")";
      } else {
        out += t.text;
      }

      // Track unary -/+ so the operand stays attached when configured to.
      // The sign-before-paren case counts too (`Line -(10,10)`).
      if (t.text == "-" || t.text == "+") {
        Token const *nx =
            si + 1 < line.sig.size() ? &T[line.sig[si + 1]] : nullptr;
        em.prevWasUnary =
            em.unaryContext() ||
            (nx != nullptr && nx->text == "(" && nx->start == t.end);
      } else {
        em.prevWasUnary = false;
      }
      em.hasPrev = true;
      em.prevKind = t.kind;
      em.prevText = t.text;
      em.prevLower = fblang::toLowerChars(t.text);
      em.prevHadWs = hadWs;
    }
    out += eol;

    // A `_` as the last code token keeps the next line a continuation.
    prevContinues =
        T[lastCode].kind == TokenKind::Identifier && T[lastCode].text == "_";

    // --- update the block stack ---
    if (lead == Lead::EndTwoWord) {
      if (!w1.empty()) {
        fblang::BlockCloser c{};
        if (fblang::blockForCloser(w1, &c) && !stack.empty() &&
            stack.back() == c.kind) {
          stack.pop_back();
        }
      } else if (!stack.empty() && stack.back() == fblang::BlockKind::Scope) {
        stack.pop_back();
      }
    } else if (lead == Lead::EndIf) {
      // The merged `EndIf` spelling of `End If`.
      if (!stack.empty() && stack.back() == fblang::BlockKind::If) {
        stack.pop_back();
      }
    } else if (lead == Lead::Bare) {
      // next / wend / loop
      if (!stack.empty() && stack.back() == closerInfo.kind) {
        stack.pop_back();
      }
    } else {
      fblang::BlockCloser opener{};
      if (fblang::blockForOpener(w0, &opener)) {
        bool blockForm = true;
        if (w0 == "if") {
          // `If cond Then` opens a block; `If cond Then stmt` is one line.
          blockForm = wLast == "then";
        }
        if (blockForm) {
          stack.push_back(opener.kind);
        }
      }
    }

    emittedAny = true;
  }

  // --- document tail ---
  auto lineStartsInsideVerbatim = [&](size_t pos) {
    for (auto const &span : verbatimSpans) {
      if (pos >= span.first && pos < span.second) {
        return true;
      }
    }
    return false;
  };
  if (config.trimTrailingWhitespace) {
    size_t start = 0;
    std::string trimmed;
    while (start <= out.size()) {
      size_t nl = out.find('\n', start);
      size_t lineEnd = nl == std::string::npos ? out.size() : nl;
      size_t e = lineEnd;
      while (e > start && (out[e - 1] == ' ' || out[e - 1] == '\t')) {
        --e;
      }
      // Verbatim interiors (block comments, continuation runs) are never
      // rewritten: a line starting inside one keeps every byte.
      if (lineStartsInsideVerbatim(start) && e < lineEnd) {
        e = lineEnd;
      }
      trimmed.append(out, start, e - start);
      if (nl == std::string::npos) {
        break;
      }
      trimmed += '\n';
      start = nl + 1;
    }
    out = std::move(trimmed);
  }
  if (config.trimFinalNewlines) {
    size_t end = out.size();
    while (end > 0 && (out[end - 1] == '\n' || out[end - 1] == '\r')) {
      --end;
    }
    out.resize(end);
    if (config.insertNewlineAtEOF && end > 0) {
      out += anyCRLF ? "\r\n" : "\n";
    }
  } else if (config.insertNewlineAtEOF &&
             (out.empty() || (out.back() != '\n' && out.back() != '\r'))) {
    out += anyCRLF ? "\r\n" : "\n";
  }
  return out;
}

} // namespace fbformat
