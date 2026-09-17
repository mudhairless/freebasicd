#include "semantic_tokens.h"

#include "language.h"
#include "lexer.h"
#include "symbols.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// UTF-8 lead-byte thresholds and payload masks; a non-BMP code point occupies
// two UTF-16 units. Mirrors utf16.cpp so the lang layer stays byte-exact with
// the session boundary without linking LspCpp.
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

// 3.17 standard SemanticTokenTypes we produce from lexer/parser evidence.
// Keep as a superset of what the classifier emits so later refinement never
// has to grow the legend. Index == position in this list.
constexpr char const *const kTokenTypes[] = {
    "keyword",  "string",     "number",    "comment",  "macro", "operator",
    "variable", "function",   "method",    "property", "type",  "class",
    "enum",     "enumMember", "parameter", "namespace"};

constexpr char const *const kTokenModifiers[] = {"declaration", "readonly"};

// Legend indices; `modifiers` bits are 1 << index.
enum Type : std::uint32_t {
  kKeyword = 0,
  kString,
  kNumber,
  kComment,
  kMacro,
  kOperator,
  kVariable,
  kFunction,
  kMethod, // reserved: never emitted today
  kProperty,
  kType, // reserved: never emitted today
  kClass,
  kEnum,
  kEnumMember,
  kParameter,
  kNamespace,
};

enum Modifier : std::uint32_t {
  kModDeclaration = 1U << 0,
  kModReadonly = 1U << 1,
};

// Width in UTF-16 code units of the code point starting at text[i]. Advances
// i past the whole code point (or past a single malformed byte). Identical in
// behavior to utf16.cpp's helper.
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

// UTF-16 code units spanned by [beg, end) in `text`.
std::uint32_t utf16Column(std::string_view text, std::size_t beg,
                          std::size_t end) {
  std::uint32_t units = 0;
  for (std::size_t i = beg; i < end;) {
    units += static_cast<std::uint32_t>(utf16UnitsOfCodePoint(text, i));
  }
  return units;
}

bool isDeclNameToken(Token const &t, Symbol const &decl) {
  return t.beg == decl.selection.beg && t.end == decl.selection.end;
}

// Classification result: whether to emit, and the legend type + modifiers.
struct Classification {
  bool emit = false;
  std::uint32_t type = 0;
  std::uint32_t modifiers = 0;
};

Classification classify(Token const &t, AnalyzedDoc const &doc) {
  switch (t.kind) {
  case TokenKind::Keyword:
    if (isCombinedAssignKeyword(toLowerChars(t.text()))) {
      return {true, kOperator, 0};
    }
    return {true, kKeyword, 0};
  case TokenKind::String:
    return {true, kString, 0};
  case TokenKind::Number:
    return {true, kNumber, 0};
  case TokenKind::Comment:
  case TokenKind::DocComment:
    return {true, kComment, 0};
  case TokenKind::Preprocessor:
  case TokenKind::Meta:
    // Whole-line tokens: #-directives and $-metacommands are `macro`.
    return {true, kMacro, 0};
  case TokenKind::Symbol: {
    std::string_view const s = t.text();
    for (std::string_view const op : symbolOperators()) {
      if (s == op) {
        return {true, kOperator, 0};
      }
    }
    return {}; // pure punctuation: default foreground, not emitted
  }
  case TokenKind::Identifier: {
    Symbol const *const decl = resolveAt(doc, t.beg);
    if (decl == nullptr) {
      return {true, kVariable, 0}; // unresolved identifier fallback
    }
    std::uint32_t mods = 0;
    if (isDeclNameToken(t, *decl)) {
      mods = kModDeclaration;
    }
    switch (decl->kind) {
    case SymbolKind::Sub:
    case SymbolKind::Function:
    case SymbolKind::Constructor:
    case SymbolKind::Destructor:
    case SymbolKind::Operator:
      return {true, kFunction, mods};
    case SymbolKind::Property:
      return {true, kProperty, mods};
    case SymbolKind::Type:
    case SymbolKind::Union:
      return {true, kClass, mods};
    case SymbolKind::Enum:
      return {true, kEnum, mods};
    case SymbolKind::Namespace:
      return {true, kNamespace, mods};
    case SymbolKind::Parameter:
      return {true, kParameter, mods};
    case SymbolKind::Const: {
      // Enum members are Const children of their Enum root; everything else
      // is a statement-level Const (readonly variable).
      Symbol const *const parent = parentOf(doc.parse, decl);
      if (parent != nullptr && parent->kind == SymbolKind::Enum) {
        return {true, kEnumMember, mods};
      }
      if (mods != 0) {
        mods |= kModReadonly;
      }
      return {true, kVariable, mods};
    }
    default:
      return {true, kVariable, mods}; // Dim, Label, Variable, Scope
    }
  }
  default:
    return {}; // Newline, Eof
  }
}

} // namespace

std::vector<std::string> semanticTokenTypes() {
  return {std::begin(kTokenTypes), std::end(kTokenTypes)};
}

std::vector<std::string> semanticTokenModifiers() {
  return {std::begin(kTokenModifiers), std::end(kTokenModifiers)};
}

std::vector<SemanticTokenEntry> semanticTokens(AnalyzedDoc const &doc,
                                               std::string_view content) {
  // Physical line starts (a line starts after every '\n', matching the
  // session's utf16Position so continuation-merged logical lines still report
  // their physical coordinates to the client).
  std::vector<std::size_t> lineStarts;
  lineStarts.push_back(0);
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] == '\n') {
      lineStarts.push_back(i + 1);
    }
  }

  std::vector<SemanticTokenEntry> out;
  std::size_t lineIdx = 0;
  for (Token const &t : doc.tokens) {
    if (t.kind == TokenKind::Newline || t.kind == TokenKind::Eof) {
      continue;
    }
    Classification const c = classify(t, doc);
    if (!c.emit) {
      continue;
    }
    while (lineIdx + 1 < lineStarts.size() &&
           lineStarts[lineIdx + 1] <= t.beg) {
      ++lineIdx;
    }
    SemanticTokenEntry e;
    e.line = static_cast<std::uint32_t>(lineIdx);
    e.startChar = utf16Column(content, lineStarts[lineIdx], t.beg);
    e.length = utf16Column(content, t.beg, t.end);
    e.type = c.type;
    e.modifiers = c.modifiers;
    out.push_back(e);
  }
  return out;
}

std::vector<std::int32_t>
encodeTokenData(std::vector<SemanticTokenEntry> const &tokens) {
  std::vector<std::int32_t> out;
  out.reserve(tokens.size() * 5);
  std::uint32_t prevLine = 0;
  std::uint32_t prevStart = 0;
  for (SemanticTokenEntry const &e : tokens) {
    out.push_back(static_cast<std::int32_t>(e.line - prevLine));
    out.push_back(e.line == prevLine
                      ? static_cast<std::int32_t>(e.startChar - prevStart)
                      : static_cast<std::int32_t>(e.startChar));
    out.push_back(static_cast<std::int32_t>(e.length));
    out.push_back(static_cast<std::int32_t>(e.type));
    out.push_back(static_cast<std::int32_t>(e.modifiers));
    prevLine = e.line;
    prevStart = e.startChar;
  }
  return out;
}

std::vector<SemanticTokenEntry>
filterTokens(std::vector<SemanticTokenEntry> const &tokens,
             std::uint32_t begLine, std::uint32_t endLine) {
  std::vector<SemanticTokenEntry> out;
  out.reserve(tokens.size());
  for (SemanticTokenEntry const &e : tokens) {
    if (e.line >= begLine && e.line <= endLine) {
      out.push_back(e);
    }
  }
  return out;
}

} // namespace fblang