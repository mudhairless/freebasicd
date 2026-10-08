/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "parser.h"

#include "i18n.h"
#include "language.h"
#include "lexer.h"
#include "symbols.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

// Hard cap on how many tokens the parser peeks ahead when scanning for the
// end of a line or a one-line construct.
#define MAX_LINE_PEEK 512
// Expected token count of a one-line `IF` statement's tail (reserve hint).
#define IF_TAIL_RESERVE 32

namespace fblang {
namespace {

using TokenKind = fblang::TokenKind;

std::string uppercase(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
    out.push_back(c);
  }
  return out;
}

struct Block {
  BlockKind kind;
  std::string_view
      close; // closer keyword for display, lowercase ("#endif" may lead with #)
  bool needsEnd = false; // closer is "END <close>"
  Symbol *sym =
      nullptr; // decl container (SUB, TYPE, ...), or null for control blocks
  uint32_t begOpen = 0; // opener keyword range
  uint32_t endOpen = 0;
  // Arm for fbc's `error 238` — only ever set on a TYPE/UNION block: the body
  // held something a plain record may not (a `Declare`, a member procedure, a
  // `Static` field, a `Const`, or a nested record/enum), so every conditional
  // field name in it is refused when the body closes (FreeBASIC.md §7).
  bool armed = false;
};

struct Container {
  Symbol *sym;                          // null for module scope
  std::unordered_set<std::string> keys; // dedupe for names at this level
  // Current access section inside a TYPE body (`Private:`/`Public:`/
  // `Protected:`, FreeBASIC.md §7 Access sections): gates every member
  // captured into this container until the next section keyword. Defaults
  // Public; only a TYPE container ever changes it (Union bodies reject
  // sections, and everything else — module scope, procedures, enums — stays
  // Public).
  Access access = Access::Public;
  explicit Container(Symbol *s) : sym(s) {}
};

class Parser {
public:
  explicit Parser(std::string_view src) : src_(src), lex_(src) {}

  ParseResult run() {
    containers_.push_back(Container(nullptr));
    advance();

    for (;;) {
      if (cur_.kind == TokenKind::Eof) {
        break;
      }
      switch (cur_.kind) {
      case TokenKind::Newline:
        advance();
        continue;
      case TokenKind::Comment:
        checkMetaLang();
        resetDoc();
        advance();
        continue;
      case TokenKind::DocComment:
        checkMetaLang();
        collectDoc();
        if (!cur_.text().empty() && cur_.text()[0] == '/') {
          addDiagnostic(cur_.beg, cur_.end, Severity::Information, "doc-slash",
                        trf("/// is not a %s comment; use '' for doc comments",
                            "FreeBASIC"));
        }
        advance();
        continue;
      case TokenKind::Preprocessor:
        handlePreprocessor();
        advance();
        continue;
      case TokenKind::Meta:
        addDiagnostic(
            cur_.beg, cur_.end, Severity::Information, "meta-directive",
            // TRANSLATORS: %s = the proper noun "FreeBASIC" (never
            // translated); the second %s is an example directive kept
            // verbatim.
            trf("bare '$' is not a valid metacommand; %s metacommands are "
                "written as comments, e.g. %s",
                "FreeBASIC", "'$LANG: \"qb\"'"));
        advance();
        continue;
      case TokenKind::Symbol:
        if (cur_.text() == ":") {
          advance();
          continue;
        }
        handleStatement();
        continue;
      default:
        handleStatement();
        continue;
      }
    }

    // Unterminated blocks, innermost first.
    for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it) {
      addDiagnostic(it->begOpen, it->endOpen, Severity::Error,
                    "unterminated-block",
                    trf("Expected '%s'", displayFor(*it)));
    }
    // Close the leftover blocks at EOF (innermost first) so their symbols get
    // sane ranges extending to the end of the source. Without this, a block
    // the user is still typing in keeps range.end == 0 and containment checks
    // (deepestNesting/innermostScope) reject every offset inside it, so hover,
    // completion, and definition find nothing while the block is unclosed.
    while (!blocks_.empty()) {
      closeBlock(cur_.end);
    }
    return std::move(out_);
  }

private:
  std::string_view src_;
  Lexer lex_;
  Token cur_;
  ParseResult out_;
  std::vector<Block> blocks_;
  std::vector<Container> containers_;
  std::string docPending_;
  bool langWarned_ = false;

  void advance() {
    if (cur_.kind == TokenKind::String && !cur_.terminated) {
      addDiagnostic(cur_.beg, cur_.end, Severity::Warning,
                    "unterminated-string", tr("unterminated string literal"));
    }
    cur_ = lex_.next();
  }

  // `$`-metacommand dialect detection inside a comment body. Metacommands are
  // written as comments in FreeBASIC (`'$LANG: "qb"`, `rem $LANG: "qb"`).
  void checkMetaLang() {
    LangMode m;
    if (langFromMetaDirective(cur_.text(), &m)) {
      applyLangDirective(m, cur_.beg, cur_.end);
    }
  }

  void applyLangDirective(LangMode m, uint32_t beg, uint32_t end) {
    out_.lang = langName(m);
    if (m != LangMode::Fb && !langWarned_) {
      langWarned_ = true;
      addDiagnostic(beg, end, Severity::Information, "lang-mode",
                    trf("dialect '%s' is not supported yet; parsing in '%s' "
                        "mode",
                        langName(m), "fb"));
    }
  }

  static bool isDeclOpenerWord(const std::string &w) {
    return w == "sub" || w == "function" || w == "property" ||
           w == "operator" || w == "constructor" || w == "destructor" ||
           w == "union" || w == "enum" || w == "namespace";
  }

  static bool isControlOpenerWord(const std::string &w) {
    return w == "scope" || w == "select" || w == "with" || w == "extern" ||
           w == "asm" || w == "for" || w == "while" || w == "do";
  }

  // Whether a control block also declares a lexical scope: a `Dim` inside it
  // is local to the block, shadows the enclosing scope, and dies at the
  // closer. fbc-probed (FreeBASIC.md §8): every statement block does —
  // `scope`, `for`, `while`, `do`, `if`, `select`, `with`. `extern` does not
  // (a `Dim` there lives on in module scope) and `asm` bodies are raw
  // assembly, so neither scopes its declarations.
  static bool isScopeBlock(BlockKind k) {
    switch (k) {
    case BlockKind::Scope:
    case BlockKind::For:
    case BlockKind::While:
    case BlockKind::Do:
    case BlockKind::If:
    case BlockKind::Select:
    case BlockKind::With:
      return true;
    default:
      return false;
    }
  }

  // A record/enum body: a member list rather than a statement list
  // (FreeBASIC.md §7), which is why it is the one block kind with a boundary
  // the *content* decides rather than a closer.
  static bool isMemberBodyKind(BlockKind k) {
    return k == BlockKind::Type || k == BlockKind::Union ||
           k == BlockKind::Enum;
  }

  // Inside a TYPE/UNION body, which is where a *field* name can be a reserved
  // word at all. An enum body is excluded on purpose: its member names have a
  // different probed rule, and it never reaches the declaration paths that ask.
  bool inRecordBody() const {
    return !blocks_.empty() && isMemberBodyKind(blocks_.back().kind) &&
           blocks_.back().kind != BlockKind::Enum;
  }

  static SymbolKind declKindFor(const std::string &w) {
    if (w == "sub") {
      return SymbolKind::Sub;
    }
    if (w == "function") {
      return SymbolKind::Function;
    }
    if (w == "property") {
      return SymbolKind::Property;
    }
    if (w == "operator") {
      return SymbolKind::Operator;
    }
    if (w == "constructor") {
      return SymbolKind::Constructor;
    }
    if (w == "destructor") {
      return SymbolKind::Destructor;
    }
    if (w == "union") {
      return SymbolKind::Union;
    }
    if (w == "enum") {
      return SymbolKind::Enum;
    }
    return SymbolKind::Namespace;
  }

  static std::string displayFor(const Block &b) {
    BlockCloser c;
    c.kind = b.kind;
    c.closeWord = b.close;
    c.needsEnd = b.needsEnd;
    return closerDisplay(c);
  }

  std::string headerText(const Token &openTok) {
    uint32_t endOff = currentLineEnd();
    endOff = std::max(endOff, openTok.beg);
    std::string s = std::string(src_.substr(openTok.beg, endOff - openTok.beg));
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t')) {
      --e;
    }
    s.resize(e);
    return s;
  }

  uint32_t currentLineEnd() {
    // If the lexer already sits on the line's newline (the opener's name is
    // the last token on its line, e.g. `enum color`), the peek-based scan
    // below would skip past it into the next line and swallow the first body
    // line into the signature (`enum color` + `red = 1`). The current
    // position *is* the line end then.
    if (cur_.kind == TokenKind::Newline || cur_.kind == TokenKind::Eof) {
      return cur_.beg;
    }
    for (int i = 0; i < MAX_LINE_PEEK; ++i) {
      Token const t = lex_.peek(i);
      if (t.kind == TokenKind::Newline) {
        return t.beg;
      }
      if (t.kind == TokenKind::Eof) {
        return t.beg;
      }
    }
    return static_cast<uint32_t>(src_.size());
  }

  void addDiagnostic(uint32_t beg, uint32_t end, Severity sev, const char *code,
                     std::string msg) {
    Diagnostic d;
    d.range.beg = beg;
    d.range.end = end;
    d.severity = sev;
    d.code = code;
    d.message = std::move(msg);
    out_.diagnostics.push_back(std::move(d));
  }

  void collectDoc() {
    std::string_view const t = cur_.text();
    size_t skip = 0;
    if (t.size() >= 2 && t[0] == '\'' && t[1] == '\'') {
      skip = 2;
    } else if (t.size() >= 3 && t[0] == '/' && t[1] == '/' && t[2] == '/') {
      skip = 3;
    }
    std::string_view const rest = t.substr(skip);
    size_t e = rest.size();
    while (e > 0 && (rest[e - 1] == ' ' || rest[e - 1] == '\t')) {
      --e;
    }
    if (!docPending_.empty()) {
      docPending_.push_back('\n');
    }
    docPending_.append(rest.substr(0, e));
  }

  void resetDoc() { docPending_.clear(); }

  std::string takeDoc() {
    std::string d = docPending_;
    docPending_.clear();
    return d;
  }

  Symbol *addSymbol(Symbol &&s) {
    // Type-member visibility: capture the current access section of the
    // enclosing container. Module scope and non-type containers keep the
    // default Public, so this is a no-op outside a TYPE body.
    s.access = containers_.empty() ? Access::Public : containers_.back().access;
    bool const dedupe =
        s.kind == SymbolKind::Dim || s.kind == SymbolKind::Const ||
        s.kind == SymbolKind::Variable || s.kind == SymbolKind::Label ||
        s.kind == SymbolKind::Type || s.kind == SymbolKind::Union ||
        s.kind == SymbolKind::Enum || s.kind == SymbolKind::Namespace;
    if (dedupe && !containers_.empty() && !s.key.empty()) {
      // Empty key = an unnamed declaration (a bare `enum`). Only the
      // *declaration's own* key is skipped: every anonymous enum used to
      // collide with the first on `''` — drd/temp/inc/raylib.bi warned
      // `duplicate definition: ''` twenty times at 1:1 on that alone. The
      // member keys inside it are ordinary non-empty keys in the enum's own
      // container, so the one shape fbc does call a duplicate — the same
      // member named twice *in one* enum, `error 4` (probed) — still reports,
      // while the shapes fbc calls clean (same member in two separate enums,
      // anonymous or not; against a module `dim`/`const`/`sub`) never reach
      // this set from a second container anyway.
      auto &set = containers_.back().keys;
      if (set.count(s.key) != 0) {
        addDiagnostic(s.selection.beg, s.selection.end, Severity::Warning,
                      "duplicate-definition",
                      trf("duplicate definition: '%s'", s.name));
      }
      set.insert(s.key);
    }
    if (containers_.empty()) {
      out_.roots.push_back(std::move(s));
      return &out_.roots.back();
    }
    Container const &c = containers_.back();
    if (c.sym == nullptr) {
      out_.roots.push_back(std::move(s));
      return &out_.roots.back();
    }
    c.sym->children.push_back(std::move(s));
    return &c.sym->children.back();
  }

  // Arm the TYPE/UNION body on top for fbc's `error 238`. Every trigger is
  // probed — a `Declare`, a member procedure with its body, a `Static` field, a
  // `Const`, and a nested record/enum — with a plain `Dim` field and an access
  // section as the negative controls (FreeBASIC.md §7,
  // tools/probe_member_names.sh). The arm happens where the trigger is *read*
  // and the refusal where the body *closes*: fbc reports it on `end type` and
  // accepts the field on either side of the trigger, so a check at capture
  // would only catch one of the two orders the probe compiles.
  void armRecordBody() {
    if (inRecordBody()) {
      blocks_.back().armed = true;
    }
  }

  // The refusal itself: an armed record may not hold a conditional field name.
  // fbc anchors its report on `end type` and its text says "member functions"
  // for *every* trigger (a `Static` field arms it the same way and is still
  // told about member functions — probed, tools/probe_member_names.sh); this
  // keeps the wording searchable against fbc's own, while the anchor is the
  // field word, which is where the rename goes and where the other two
  // `invalid-member-name` reports already sit. The member is dropped, as fbc
  // drops it: completion and hover must not offer a field that does not exist.
  void dropArmedConditionalFields(Symbol &record) {
    std::vector<Symbol> &kids = record.children;
    for (std::size_t i = kids.size(); i-- > 0;) {
      Symbol const &m = kids[i];
      bool const field = m.kind == SymbolKind::Variable ||
                         m.kind == SymbolKind::Dim ||
                         m.kind == SymbolKind::Const;
      if (!field || !isConditionalFieldName(m.key)) {
        continue;
      }
      addDiagnostic(m.selection.beg, m.selection.end, Severity::Error,
                    "invalid-member-name",
                    trf("'%s' is a reserved word and cannot be a field name in "
                        "a type with member functions",
                        m.name));
      kids.erase(kids.begin() + static_cast<std::ptrdiff_t>(i));
    }
  }

  void closeBlock(uint32_t end) {
    Block const b = blocks_.back();
    if (b.armed && b.sym != nullptr) {
      dropArmedConditionalFields(*b.sym);
    }
    blocks_.pop_back();
    out_.blockRanges.push_back({b.begOpen, end});
    if (b.sym != nullptr) {
      b.sym->range.end = end;
      unwindBranchScopes(b.sym, end);
      if (!containers_.empty() && containers_.back().sym == b.sym) {
        containers_.pop_back();
      }
    }
  }

  // Close the innermost block at `end` because the source says it ends there,
  // not because the buffer ran out, and report the missing closer. The report
  // stays anchored at the opener (the squiggle means "you forgot to close
  // this"), while `closerAt` carries the offset the closer belongs at — the
  // `unterminated-block` fix inserts there. Distinct from the EOF path in
  // run(), which has no evidence and so sets no `closerAt`.
  void closeBlockUnterminated(uint32_t end) {
    Block const b = blocks_.back();
    closeBlock(end);
    Diagnostic d;
    d.range = SourceRange{b.begOpen, b.endOpen};
    d.severity = Severity::Error;
    d.code = "unterminated-block";
    d.message = trf("Expected '%s'", displayFor(b));
    d.closerAt = end;
    out_.diagnostics.push_back(std::move(d));
  }

  // Close the innermost block because the statement at `cur_` cannot belong to
  // its body, and hand the statement back for parsing in the enclosing scope.
  // This is the evidence fbc anchors `error 19` / `error 74` on (FreeBASIC.md
  // §7): a record or enum body is a member list, so the first statement its
  // grammar rejects is where the closer belongs. Loop, because nested records
  // nest the failure — `type a / type b / <statement>` needs both `END TYPE`s,
  // and the outer one is just as unterminated as the inner.
  bool closeBodyAtBoundary() {
    if (blocks_.empty()) {
      return false;
    }
    Block const top = blocks_.back();
    if (!isMemberBodyKind(top.kind)) {
      return false;
    }
    // The record's own key, for the one member the language cannot have: a
    // field of the record declaring it (fbc's `error 88`). "" for a record
    // block whose name the parse has not taken yet, and then no statement can
    // be self-referential.
    std::string_view const key = top.sym != nullptr
                                     ? std::string_view(top.sym->key)
                                     : std::string_view();
    std::vector<Token> const stmt = statementTokens();
    if (acceptsBodyMember(top.kind, stmt, key)) {
      return false;
    }
    // An enum body rejected this because its first token is a reserved word
    // fbc will not accept as a member name. The `unterminated-block` that
    // follows says where the closer belongs but not why the body ended here,
    // and for this case the why is the whole error — fbc's own is `error 3:
    // Expected End-of-Line` on the name, not a missing closer. So name it.
    if (top.kind == BlockKind::Enum && !stmt.empty() &&
        stmt.front().kind == TokenKind::Keyword &&
        !isLegalEnumMemberName(toLowerChars(stmt.front().text()))) {
      addDiagnostic(stmt.front().beg, stmt.front().end, Severity::Error,
                    "invalid-member-name",
                    trf("'%s' is a reserved word and cannot be an enum member "
                        "name",
                        stmt.front().text()));
    }
    closeBlockUnterminated(cur_.beg);
    return true;
  }

  // What ends a statement: a newline, a comment, a doc comment, or a `:`.
  // Shared so the member-capture path below can ask the same question the line
  // peek asks.
  static bool endsStatement(Token const &t) {
    return t.kind == TokenKind::Newline || t.kind == TokenKind::Eof ||
           t.kind == TokenKind::Comment || t.kind == TokenKind::DocComment ||
           (t.kind == TokenKind::Symbol && t.text() == ":");
  }

  // Can `t` continue a type chain after `As`? A builtin type word (`ptr`,
  // `pointer`, `integer`, `zstring`, …) or the `const` modifier: everything
  // `as integer ptr the_data` puts between the type and the field name
  // (probed; fbc compiles those chains).
  static bool isTypeChainWord(Token const &t) {
    if (t.kind != TokenKind::Keyword) {
      return false;
    }
    std::string const w = toLowerChars(t.text());
    return isBuiltinType(w) || w == "const";
  }

  // The tokens of the statement starting at `cur_`, up to its statement end.
  // Same stop set as skipStatement — a newline, a comment, or a `:` separator —
  // so a `:`-separated statement is judged on its own tokens, and the lexer has
  // already merged `_` continuation lines into one logical line.
  std::vector<Token> statementTokens() {
    std::vector<Token> out;
    if (endsStatement(cur_)) {
      return out;
    }
    out.push_back(cur_);
    for (int i = 0; i < MAX_LINE_PEEK && out.size() < MAX_LINE_PEEK; ++i) {
      Token const t = lex_.peek(i);
      if (endsStatement(t)) {
        break;
      }
      out.push_back(t);
    }
    return out;
  }

  void skipStatement(bool suppressMemberCapture = false) {
    bool captureMember = false;
    bool inRecord = false; // inside a TYPE/UNION body: capture field members
    SymbolKind mk = SymbolKind::Variable;
    if (!suppressMemberCapture && !blocks_.empty()) {
      BlockKind const bk = blocks_.back().kind;
      if (bk == BlockKind::Type || bk == BlockKind::Union) {
        captureMember = true;
        inRecord = true;
      } else if (bk == BlockKind::Enum) {
        captureMember = true;
        mk = SymbolKind::Const;
      }
    }
    Token const stmtStart = cur_;
    int parenDepth = 0;
    int braceDepth = 0;
    for (;;) {
      TokenKind const k = cur_.kind;
      if (k == TokenKind::Newline || k == TokenKind::Eof ||
          k == TokenKind::Comment || k == TokenKind::DocComment) {
        return;
      }
      if (k == TokenKind::Symbol && cur_.text() == ":") {
        return;
      }
      if (k == TokenKind::Symbol && cur_.text() == "_") {
        addDiagnostic(cur_.beg, cur_.end, Severity::Error, "bad-continuation",
                      tr("expected a newline after '_'"));
      }
      if (inRecord && k == TokenKind::Symbol && cur_.text() == "(") {
        ++parenDepth;
      } else if (inRecord && k == TokenKind::Symbol && cur_.text() == ")") {
        if (parenDepth > 0) {
          --parenDepth;
        }
      } else if (inRecord && k == TokenKind::Symbol && cur_.text() == "{") {
        ++braceDepth;
      } else if (inRecord && k == TokenKind::Symbol && cur_.text() == "}") {
        if (braceDepth > 0) {
          --braceDepth;
        }
      } else if (inRecord && k == TokenKind::Symbol && cur_.text() == "," &&
                 parenDepth == 0 && braceDepth == 0) {
        // `as integer a, b` declares a whole list of members on one line.
        // A comma inside initializer braces separates elements, not members
        // (`= { lgt.x, lgt.y }` must not re-arm the capture per element).
        captureMember = true;
      }
      if (captureMember && k == TokenKind::Keyword &&
          toLowerChars(cur_.text()) == "as") {
        // `as integer as` names a field `as` — fbc accepts it (probed;
        // FreeBASIC.md §7, tools/probe_member_names.sh), and without this the
        // type-skipping below read the name as a second type-introducer and
        // registered nothing at all. An `as` with no token behind it can only
        // be the name: the line has no room for a type and then none for a
        // name.
        if (!endsStatement(lex_.peek(0))) {
          // Type-first member: `as <type> name`. Skip the type name (builtin
          // keyword or user-defined type) so the *member* is captured, not the
          // type — drd/temp/inc/world.bi's `as Wall walls(MAX_WALLS - 1)` used
          // to register `wall`. The member's signature still covers the full
          // line so the declared type survives for hover/resolve.
          advance();
          // The type is one name token; a keyword is consumed as part of the
          // type only while a member name still follows, so `as integer name`
          // keeps `name` as the member while `as name n` (`name` is a keyword
          // *type* name) keeps `n`. Reserved words are valid field names
          // (fbc-verified): `as string name`.
          if (cur_.kind == TokenKind::Identifier) {
            advance();
          } else if (cur_.kind == TokenKind::Keyword) {
            Token const nxt = lex_.peek(0);
            bool const memberFollows = nxt.kind == TokenKind::Identifier ||
                                       (nxt.kind == TokenKind::Keyword &&
                                        toLowerChars(nxt.text()) != "as");
            if (isBuiltinType(toLowerChars(cur_.text())) || memberFollows) {
              advance();
            }
          }
          // The type half is a chain, not one word: `as integer ptr p`,
          // `as integer const ptr c`, `as const integer c`, `as udt ptr ptr m`
          // (probed; fbc compiles every one). Keep consuming type words while
          // a member name still follows, or `ptr` stops here and reaches the
          // never-field report below as though it were the field name — the
          // field is `p`/`c`/`m`, and `ptr` is only its modifier. A chain
          // word with no name behind it is left alone: `as integer ptr` is
          // `error 14: Expected identifier` in fbc, and the report on the
          // dangling word is the closest this parser gets to that error.
          while (isTypeChainWord(cur_)) {
            Token const nxt = lex_.peek(0);
            bool const memberFollows = nxt.kind == TokenKind::Identifier ||
                                       (nxt.kind == TokenKind::Keyword &&
                                        toLowerChars(nxt.text()) != "as");
            if (!memberFollows) {
              break;
            }
            advance();
          }
          continue;
        }
      }
      if (captureMember && k == TokenKind::Keyword &&
          isNeverFieldName(toLowerChars(cur_.text()))) {
        // A reserved word fbc refuses as a field name outright: `as integer
        // and` is its `error 14`, and it declares no such field, so nothing is
        // registered here — completion must not offer a member fbc rejects
        // (probed; FreeBASIC.md §7, tools/probe_member_names.sh).
        addDiagnostic(cur_.beg, cur_.end, Severity::Error,
                      "invalid-member-name",
                      trf("'%s' is a reserved word and cannot be a field name",
                          cur_.text()));
        captureMember = false;
        advance();
        continue;
      }
      if (captureMember &&
          (k == TokenKind::Identifier || k == TokenKind::Keyword)) {
        // The other 349 reserved words *are* legal field names (`as string
        // name`, `as integer next`), so a keyword is captured exactly like an
        // identifier. An enum body's 230 legal names arrive by this same route;
        // its 135 illegal ones never reach it, being a boundary instead — and
        // so does `rem`, which the lexer has already called a comment.
        Symbol m;
        m.kind = mk;
        m.name = std::string(cur_.text());
        m.key = toLowerChars(m.name);
        m.selection.beg = m.range.beg = cur_.beg;
        m.selection.end = m.range.end = cur_.end;
        m.signature =
            headerText(stmtStart); // full field line: carries the type
        m.doc = takeDoc();
        addSymbol(std::move(m));
        captureMember = false;
      }
      advance();
    }
  }

  void handleStatement() {
    // A record/enum body is a member list, not a statement list: close it at
    // the first statement its grammar cannot accept, before anything below
    // reads it as a field or a member. Re-enter so the statement is parsed in
    // the enclosing scope, and keep going while records nest the failure.
    while (closeBodyAtBoundary()) {
      // One pass closes one body and re-judges the same statement against the
      // one now on top, which is what makes nested records nest the failure.
    }

    // Line label: Identifier directly followed by ':'.
    if (cur_.kind == TokenKind::Identifier) {
      Token const nxt = lex_.peek(0);
      if (nxt.kind == TokenKind::Symbol && nxt.text() == ":") {
        Symbol s;
        s.kind = SymbolKind::Label;
        s.name = std::string(cur_.text());
        s.key = toLowerChars(s.name);
        s.selection.beg = s.range.beg = cur_.beg;
        s.selection.end = s.range.end = cur_.end;
        s.doc = takeDoc();
        addSymbol(std::move(s));
        advance();
        advance();
        return;
      }
    }
    if (cur_.kind != TokenKind::Keyword) {
      resetDoc();
      skipStatement();
      return;
    }

    std::string const w = toLowerChars(cur_.text());

    // Access section: `Private:`, `Public:`, `Protected:`. Inside a TYPE body
    // it gates every member declaration after it until the next section
    // (FreeBASIC.md §7 Access sections; fbc 1.10.2:
    // section-colon syntax only, and only inside a Type — a Union rejects it
    // with a syntax error — and only in `-lang fb`). The section declares
    // nothing; stamps are applied by addSymbol from the container gate.
    // Elsewhere (module level, a Union body) the colon still makes the line an
    // empty statement, so consume the pair regardless of the enclosing block —
    // the keyword must never be captured as a phantom member.
    if ((w == "private" || w == "public" || w == "protected") &&
        lex_.peek(0).kind == TokenKind::Symbol && lex_.peek(0).text() == ":") {
      if (!blocks_.empty() && blocks_.back().kind == BlockKind::Type) {
        containers_.back().access = w == "private"     ? Access::Private
                                    : w == "protected" ? Access::Protected
                                                       : Access::Public;
      }
      resetDoc();
      advance(); // the section keyword
      advance(); // the ':'
      return;
    }

    if (w == "private" || w == "public" || w == "export" || w == "static") {
      Token const nxt = lex_.peek(0);
      if (nxt.kind == TokenKind::Keyword &&
          (isDeclOpenerWord(toLowerChars(nxt.text())) ||
           toLowerChars(nxt.text()) == "type")) {
        advance();
        handleStatement();
        return;
      }
      // `Static x` and `Static Shared x` are both var declarations; the
      // two-token form needs the shared keyword routed here (the old
      // code required an identifier and let `static shared` fall into
      // skipStatement).
      if (w == "static" && (nxt.kind == TokenKind::Identifier ||
                            (nxt.kind == TokenKind::Keyword &&
                             toLowerChars(nxt.text()) == "shared"))) {
        // A `Static` field arms the record body for `error 238`; a plain `Dim`
        // field is the control that does not (probed,
        // tools/probe_member_names.sh).
        armRecordBody();
        handleVarDecls(SymbolKind::Dim);
        return;
      }
      resetDoc();
      skipStatement();
      return;
    }

    // A closer *word* is not a closer statement: `next as node ptr` is a
    // field named `next` (FreeBASIC.md §7, reserved-word field names), and
    // routing it here would skip the field and then read the line as evidence
    // that the enclosing record ends above it.
    if (isCloserStatement(statementTokens())) {
      if (w == "end") {
        handleEnd();
        return;
      }
      BlockCloser c;
      blockForCloser(w, &c);
      handlePlainCloser(c);
      return;
    }
    if (isDeclOpenerWord(w)) {
      handleDeclBlock(w, declKindFor(w));
      return;
    }
    if (w == "type") {
      handleType();
      return;
    }
    if (w == "declare") {
      handleDeclare();
      return;
    }
    if (w == "dim" || w == "redim" || w == "var" || w == "local" ||
        w == "common" || w == "const") {
      if (!blocks_.empty() && blocks_.back().kind == BlockKind::Enum) {
        // In an enum body every word that got this far is a name the body may
        // hold — the boundary check above closed the body on the ones it may
        // not — so this is a member name and not an opener. `redim` and `local`
        // are legal enum members and fbc creates them; the var-decl handler
        // instead swallowed the line, so completion and hover could not offer a
        // member that exists.
        skipStatement();
        return;
      }
      if (w == "const") {
        // `Const` is a trigger too; `Dim`, `Redim`, `Var`, `Local` and `Common`
        // are not (a plain `Dim` field is the probed control that arms
        // nothing).
        armRecordBody();
        handleVarDecls(SymbolKind::Const);
        return;
      }
      handleVarDecls(SymbolKind::Dim);
      return;
    }
    if (w == "if") {
      handleIf();
      return;
    }
    if (w == "else" || w == "elseif") {
      if (blocks_.empty() || blocks_.back().kind != BlockKind::If) {
        addDiagnostic(cur_.beg, cur_.end, Severity::Error, "stray-closer",
                      trf("%s without %s", uppercase(w), "IF"));
      } else {
        openBranchScope(w);
      }
      resetDoc();
      skipStatement();
      return;
    }
    if (w == "case") {
      if (blocks_.empty() || blocks_.back().kind != BlockKind::Select) {
        addDiagnostic(cur_.beg, cur_.end, Severity::Error, "stray-closer",
                      trf("%s without %s", "CASE", "SELECT"));
      } else {
        openBranchScope("case");
      }
      resetDoc();
      skipStatement();
      return;
    }
    if (w == "exit" || w == "continue") {
      resetDoc();
      skipStatement();
      return;
    }
    if (w == "for") {
      handleFor();
      return;
    }
    if (isControlOpenerWord(w)) {
      pushBlockFromOpener(w);
      resetDoc();
      advance();
      skipStatement();
      return;
    }
    resetDoc();
    skipStatement();
  }

  void pushBlockFromOpener(const std::string &w) {
    BlockCloser c;
    blockForOpener(w, &c);
    Block b;
    b.kind = c.kind;
    b.close = c.closeWord;
    b.needsEnd = c.needsEnd;
    b.begOpen = cur_.beg;
    b.endOpen = cur_.end;
    attachScopeBlock(&b, w);
    blocks_.push_back(b);
  }

  // Give a freshly opened control block its own declaration scope: a Scope
  // symbol for the enclosing scope to adopt (a file root at module level, a
  // procedure's child inside one) plus a dedupe container. Declarations inside
  // the block then shadow the enclosing scope instead of tripping
  // "duplicate definition" against it, and this is what resolution walks so an
  // in-block `Dim` shadows the outer name and is gone after the closer
  // (FreeBASIC.md §8, fbc-probed). Scope nodes are structure, not symbols:
  // they carry no key, so they never index, resolve, or autocomplete.
  void attachScopeBlock(Block *b, std::string const &name) {
    if (!isScopeBlock(b->kind)) {
      return;
    }
    Symbol s;
    s.kind = SymbolKind::Scope;
    s.name = name;
    s.key.clear();
    s.selection.beg = s.range.beg = b->begOpen;
    s.selection.end = s.range.end = b->endOpen;
    s.signature = name;
    b->sym = addSymbol(std::move(s));
    if (b->sym != nullptr) {
      containers_.push_back(Container(b->sym));
    }
  }

  // Pop the declaration scopes stacked *above* `target`'s own container — the
  // branch scopes `openBranchScope` opens — closing each popped range at
  // `end`. The closed range is what keeps a position inside the branch
  // nesting into that scope after the pop (resolution walks ranges, not the
  // container stack). Leaves `target`'s container on top; if the stack never
  // held it, touches nothing (a desync must not drain the whole stack).
  void unwindBranchScopes(Symbol *target, uint32_t end) {
    if (target == nullptr || containers_.empty()) {
      return;
    }
    bool found = false;
    for (auto it = containers_.rbegin(); it != containers_.rend(); ++it) {
      if (it->sym == target) {
        found = true;
        break;
      }
    }
    if (!found) {
      return;
    }
    while (!containers_.empty() && containers_.back().sym != target) {
      Symbol *branch = containers_.back().sym;
      if (branch != nullptr) {
        branch->range.end = end;
      }
      containers_.pop_back();
    }
  }

  // Each branch of a control block is its own declaration scope — fbc-probed
  // (FreeBASIC.md §8): `dim p` in an `if` branch and again in its `elseif`/
  // `else` compiles (twice in *one* branch is still error 4), and no branch
  // can see a sibling's name across the split (error 42), the same for every
  // `case` of a `select`. So open a Scope child under the block's own and
  // push it as the declaration container: declarations land in this branch,
  // the next branch unwinds back to the block's container here (closing this
  // branch's range at the split), and the block's closer unwinds the last
  // one. Requires `blocks_.back()` to be the owning control block.
  void openBranchScope(std::string const &name) {
    Block const &b = blocks_.back();
    unwindBranchScopes(b.sym, cur_.beg);
    if (b.sym == nullptr || containers_.empty() ||
        containers_.back().sym != b.sym) {
      return; // no scope container to branch from: leave the stack alone
    }
    Symbol s;
    s.kind = SymbolKind::Scope;
    // A branch carries the construct it belongs to: the block's own scope is
    // spliced out of the outline (it declares nothing directly — see
    // session.cpp `appendOutline`), so a bare `then` would sit there with no
    // `if` anywhere above it. Which block opened a branch is knowledge only
    // the parser has, so the label is built here — `if..then`, `if..else`,
    // `select..case` — and the display layer only decorates it.
    s.name = b.sym->name + ".." + name;
    s.key.clear();
    s.selection.beg = s.range.beg = cur_.beg;
    s.selection.end = s.range.end = cur_.end;
    s.signature = s.name;
    containers_.push_back(Container(addSymbol(std::move(s))));
  }

  // `for`-loop header: `for <counter> [as <type>] = min [to max [step n]]`.
  // `for each ...` is not a FreeBASIC form, so the first identifier after
  // `for` is always the counter. fbc ground truth (FreeBASIC.md §8): a header
  // with `as` declares a NEW counter scoped to the loop (invisible after
  // `next`); a header without `as` reuses an already-declared variable
  // (undeclared is error 42, fbc does not auto-declare it), so nothing is
  // registered and resolution finds the outer declaration naturally.
  void handleFor() {
    Token const openTok = cur_;
    pushBlockFromOpener("for");
    // Peek the header while the lexer still sits on `for` (peek is relative
    // to the token after cur_): peek(0) is the counter, peek(1) the `as`.
    registerForCounter(openTok);
    advance(); // past `for`
    resetDoc();
    skipStatement();
  }

  void registerForCounter(Token const &openTok) {
    // Counter candidate must be an identifier immediately followed by `as`;
    // a header without `as` reuses an outer declaration and registers nothing.
    Token const nameTok = lex_.peek(0);
    if (nameTok.kind != TokenKind::Identifier) {
      return;
    }
    Token const asTok = lex_.peek(1);
    if (asTok.kind != TokenKind::Keyword ||
        toLowerChars(asTok.text()) != "as") {
      return;
    }
    Symbol s;
    s.kind = SymbolKind::Dim;
    s.name = std::string(nameTok.text());
    s.key = toLowerChars(s.name);
    s.selection.beg = s.range.beg = nameTok.beg;
    s.selection.end = s.range.end = nameTok.end;
    s.signature = headerText(openTok); // carries the `as <type>` clause
    s.doc = takeDoc();
    s.loopVar = true;
    addSymbol(std::move(s));
  }

  // `type derived extends base` / `union u extends a`: record the base's lookup
  // key and consume both tokens, so skipStatement does not capture the
  // `extends` keyword as a spurious Variable field of the type (it is a
  // Keyword, and the TYPE/UNION member-capture path takes a bare keyword token
  // as a field name). `extends object` is the same edge — `Object` is a
  // keyword, and that is how a UDT gets a VMT — so the base is accepted as an
  // Identifier *or* a Keyword. Extends is FreeBASIC's only inheritance form:
  // there is no `Type : base` and no `interface`. A malformed clause (no name
  // after `extends`) is left for skipStatement to run over, which keeps the
  // existing behavior rather than inventing a diagnostic here.
  void takeExtendsClause(Symbol &s) {
    if (s.kind != SymbolKind::Type && s.kind != SymbolKind::Union) {
      return;
    }
    if (cur_.kind != TokenKind::Keyword ||
        toLowerChars(cur_.text()) != "extends") {
      return;
    }
    advance();
    if (cur_.kind != TokenKind::Identifier && cur_.kind != TokenKind::Keyword) {
      return;
    }
    s.extendsKey = toLowerChars(cur_.text());
    advance();
  }

  // `sub t.go()` / `function t.val()` / `property Screen.w()` at module level
  // is the *implementation* of a member procedure `declare`d inside `type t`
  // (FreeBASIC.md §7: fbc error 17 rejects a definition inside the type body).
  // Re-key the symbol by the member so it stops claiming the type's key, and
  // record the owner as the other half of that edge. Only these three kinds
  // take the form: a Constructor/Destructor implementation is spelled
  // `constructor t()` with no dot, indistinguishable here from a module
  // constructor, so it is left alone rather than guessed at.
  void takeMemberImplementation(Symbol &s) {
    if (s.kind != SymbolKind::Sub && s.kind != SymbolKind::Function &&
        s.kind != SymbolKind::Property) {
      return;
    }
    if (s.name.empty() || cur_.kind != TokenKind::Symbol ||
        cur_.text() != ".") {
      return;
    }
    Token const nxt = lex_.peek(0);
    if (nxt.kind != TokenKind::Identifier && nxt.kind != TokenKind::Keyword) {
      return;
    }
    s.ownerKey = s.key;
    s.ownerName = s.name; // the qualifier as written, for the outline
    advance();            // '.'
    s.name = std::string(cur_.text());
    s.key = toLowerChars(s.name);
    s.selection.beg = cur_.beg;
    s.selection.end = cur_.end;
    advance();
  }

  // The outline name of a declaration block written without one: a bare
  // `enum`, an anonymous `union` nested in a TYPE, a nameless record `type`
  // inside a UNION (all three are legal spellings — ProPgTypeUnion's own
  // examples are written this way, and fbc calls the construct "anonymous" in
  // its diagnostics). Only `name` is filled: `key` must stay empty, because an
  // empty key is this parser's unnamed-declaration signal for dedupe, the
  // index, resolution, completion, and rename.
  static std::string anonymousDeclName(const std::string &openWord) {
    return "<anonymous " + toLowerChars(openWord) + ">";
  }

  // The declaration container for `psym`. An *anonymous* block keeps the
  // access section in force where it is written: fbc gates the fields of a
  // nested anonymous union by the enclosing `Private:` (probed: error 202),
  // and an access section *inside* an anonymous union is itself rejected
  // (probed: error 17), so the section is inherited rather than restarted.
  Container containerFor(Symbol *psym) {
    Container c(psym);
    if (psym != nullptr && psym->key.empty() && !containers_.empty()) {
      c.access = containers_.back().access;
    }
    return c;
  }

  void handleDeclBlock(const std::string &openWord, SymbolKind k) {
    Token const openTok = cur_;
    // Arm the enclosing record before the push: a member procedure body or a
    // nested record/enum inside a TYPE/UNION is one of the five `error 238`
    // triggers, and the block this call opens is not the body being armed.
    armRecordBody();
    BlockCloser closer;
    blockForOpener(openWord, &closer);
    advance();

    Symbol s;
    s.kind = k;
    s.range.beg = openTok.beg;
    // Default the selection to the opener, because a block written without a
    // name still needs a position inside its own range: LSP requires
    // selectionRange ⊆ range, and a client binds the outline click to it. A
    // name token below overwrites this.
    s.selection.beg = openTok.beg;
    s.selection.end = openTok.end;
    if (cur_.kind == TokenKind::Identifier || cur_.kind == TokenKind::Keyword ||
        (k == SymbolKind::Operator && cur_.kind == TokenKind::Symbol)) {
      s.name = std::string(cur_.text());
      s.key = toLowerChars(s.name);
      s.selection.beg = cur_.beg;
      s.selection.end = cur_.end;
      advance();
    } else {
      s.name = anonymousDeclName(openWord);
    }
    // `Enum <name> explicit`: the optional `Explicit` keyword gates the members
    // behind qualified `Name.member` access (FreeBASIC.md §8, fbc-verified).
    // Consume it on the header line so skipStatement below does not register it
    // as a spurious member.
    if (k == SymbolKind::Enum && cur_.kind == TokenKind::Keyword &&
        toLowerChars(cur_.text()) == "explicit") {
      s.explicitEnum = true;
      advance();
    }
    takeMemberImplementation(s);
    takeExtendsClause(s);
    s.signature = headerText(openTok);
    s.doc = takeDoc();

    if (cur_.kind == TokenKind::Symbol && cur_.text() == "(") {
      populateParams(s);
    }

    Symbol *psym = addSymbol(std::move(s));

    Block b;
    b.kind = closer.kind;
    b.close = closer.closeWord;
    b.needsEnd = closer.needsEnd;
    b.sym = psym;
    b.begOpen = openTok.beg;
    b.endOpen = openTok.end;
    blocks_.push_back(b);
    if (psym != nullptr) {
      containers_.push_back(containerFor(psym));
    }
    skipStatement();
  }

  void populateParams(Symbol &s) {
    advance(); // '('
    std::vector<Token> entry;
    int depth = 1;
    for (;;) {
      TokenKind const k = cur_.kind;
      if (k == TokenKind::Eof || k == TokenKind::Newline) {
        break;
      }
      if (k == TokenKind::Symbol) {
        std::string_view const t = cur_.text();
        if (t == "(") {
          entry.push_back(cur_);
          ++depth;
          advance();
          continue;
        }
        if (t == ")") {
          if (depth == 1) {
            addParam(s, entry);
            advance();
            return;
          }
          --depth;
          advance();
          continue;
        }
        if (depth == 1 && t == ",") {
          addParam(s, entry);
          entry.clear();
          advance();
          continue;
        }
      }
      entry.push_back(cur_);
      advance();
    }
    addParam(s, entry);
  }

  static void addParam(Symbol &s, const std::vector<Token> &entry) {
    for (const Token &t : entry) {
      if (t.kind == TokenKind::Identifier) {
        Symbol p;
        p.kind = SymbolKind::Parameter;
        p.name = std::string(t.text());
        p.key = toLowerChars(p.name);
        p.selection.beg = p.range.beg = t.beg;
        p.selection.end = p.range.end = t.end;
        // Signature carries the full parameter text (`byref map as map_struct`)
        // so hover/member-access can recover the declared type.
        std::string sig;
        for (const Token &e : entry) {
          if (!sig.empty()) {
            sig += " ";
          }
          sig.append(e.text());
        }
        p.signature = std::move(sig);
        s.children.push_back(std::move(p));
        break;
      }
    }
  }

  void handleType() {
    Token const openTok = cur_;
    // `TYPE AS <type> ...` never introduces a record: the token after the
    // keyword is the alias/field introducer, not a name. fbc 1.10.2 probed:
    //   - inside a record body, `type as ulong` is a FIELD named `type` whose
    //     type follows the `as` (nothing may follow it — `type as integer x`
    //     is `error 3: Expected End-of-Line`);
    //   - outside one, `type as rAudioBuffer rAudioBuffer_` is an alias with
    //     the name *after* the type, and `type as long` with no name behind it
    //     is `error 14: Expected identifier`.
    // Reading `as` as the name instead pushed a record body no `end type`
    // belonged to, and every statement below it was then parsed as a member
    // list — drd/temp/inc/raylib.bi reported dozens of phantom
    // invalid-member-name / duplicate-definition / unterminated-block errors
    // starting at its first `type as <type> <name>` alias.
    bool const asFirst = lex_.peek(0).kind == TokenKind::Keyword &&
                         toLowerChars(lex_.peek(0).text()) == "as";
    if (asFirst) {
      if (inRecordBody()) {
        // The field form: capture `type` through the same route as any plain
        // field line (signature, doc, armed-238-drop all follow), then let
        // skipStatement swallow the `as <type>` tail with capture off — there
        // is no name after the type on this form for it to be hunting.
        skipStatement();
        return;
      }
      advance(); // the `as`
      // Alias whose name trails the type: the last identifier at paren depth
      // 0 that sits at least one token past the `as`. The gap is what tells
      // name from type — `type as MyUdt` ends in the type itself and has no
      // name (fbc errors), while `type as MyUdt ptr p` ends in the real one.
      // A comma list (`type as integer a1, a2`, which fbc compiles) names only
      // its last element here; that records one alias and skips the other,
      // which is quiet on both sides — lean-ctx: upgrade to per-segment names
      // if a false report ever hangs on it.
      std::string name;
      uint32_t nameBeg = 0;
      uint32_t nameEnd = 0;
      int depth = 0;
      std::vector<Token> const toks = statementTokens();
      for (std::size_t i = 1; i < toks.size(); ++i) {
        Token const &t = toks[i];
        if (t.kind == TokenKind::Symbol && t.text() == "(") {
          ++depth;
          continue;
        }
        if (t.kind == TokenKind::Symbol && t.text() == ")") {
          if (depth > 0) {
            --depth;
          }
          continue;
        }
        if (depth == 0 && i >= 2 && t.kind == TokenKind::Identifier) {
          name = std::string(t.text());
          nameBeg = t.beg;
          nameEnd = t.end;
        }
      }
      if (!name.empty()) {
        Symbol s;
        s.kind = SymbolKind::Type;
        s.name = name;
        s.key = toLowerChars(name);
        s.selection.beg = nameBeg;
        s.selection.end = nameEnd;
        s.range.beg = openTok.beg;
        s.range.end = currentLineEnd();
        s.signature = headerText(openTok);
        s.doc = takeDoc();
        addSymbol(std::move(s));
      } else {
        // fbc rejects the nameless spelling; register nothing and above all
        // open nothing — the report is its job, not ours.
        resetDoc();
      }
      skipStatement(/*suppressMemberCapture=*/true);
      return;
    }

    // A nested record is the fifth trigger; arm the enclosing one first, before
    // this record's own block is pushed (FreeBASIC.md §7). The arm covers the
    // nested record *and* the in-body alias `type f1 as long` (probed: that
    // spelling arms 238 too, though fbc never makes it a member); the field
    // form above deliberately does not arm (probed: a plain `type as long`
    // field leaves the gate shut).
    armRecordBody();
    BlockCloser closer;
    blockForOpener("type", &closer);
    advance();

    Symbol s;
    s.kind = SymbolKind::Type;
    s.range.beg = openTok.beg;
    // Same default selection as handleDeclBlock: a nameless record body gets
    // the opener's position, so its selectionRange stays inside its range.
    s.selection.beg = openTok.beg;
    s.selection.end = openTok.end;
    bool hasName = false;
    if (cur_.kind == TokenKind::Identifier || cur_.kind == TokenKind::Keyword) {
      hasName = true;
      s.name = std::string(cur_.text());
      s.key = toLowerChars(s.name);
      s.selection.beg = cur_.beg;
      s.selection.end = cur_.end;
      advance();
    } else {
      // A nameless record body is legal only as UNION's inner Type
      // (ProPgTypeUnion), but it must still get a node of its own: its fields
      // belong to *it*, and an outline that let them float up to the parent
      // container would show a flat member list for a nested structure. fbc
      // rejects the spelling outside a union; a lenient parse keeps the fields
      // where the source put them.
      s.name = anonymousDeclName("type");
    }
    // Before the alias lookahead: `type derived extends base` has neither a
    // `:` nor an `as` on the line, so the scan below would leave alias false
    // anyway — but consuming the clause here is what keeps `extends` out of the
    // member list, and it has to happen before signature/doc are read so they
    // still cover the whole opener line.
    takeExtendsClause(s);
    s.signature = headerText(openTok);
    s.doc = takeDoc();

    // Alias form: `TYPE name AS type` (rest of line, no ':' before 'as').
    bool alias = false;
    if (hasName) {
      if (cur_.kind == TokenKind::Newline || cur_.kind == TokenKind::Eof ||
          (cur_.kind == TokenKind::Symbol && cur_.text() == ":")) {
        // name alone on the line, or one-line `TYPE name : ... : END TYPE`
      } else if (cur_.kind == TokenKind::Keyword &&
                 toLowerChars(cur_.text()) == "as") {
        alias = true;
      } else {
        for (int i = 0; i < MAX_LINE_PEEK; ++i) {
          Token const t = lex_.peek(i);
          if (t.kind == TokenKind::Newline || t.kind == TokenKind::Eof ||
              t.kind == TokenKind::Comment) {
            break;
          }
          if (t.kind == TokenKind::Symbol && t.text() == ":") {
            break;
          }
          if (t.kind == TokenKind::Keyword && toLowerChars(t.text()) == "as") {
            alias = true;
            break;
          }
        }
      }
    }

    if (alias) {
      s.range.end = currentLineEnd();
      addSymbol(std::move(s));
      // The tail of an alias line is only its type expression, never a member
      // — but with the alias sitting inside a record body (probed: legal, and
      // not a member), capture-on read `type cb as sub(byval a as long)`'s
      // parameter list as fields and reported `byval`.
      skipStatement(/*suppressMemberCapture=*/true);
      return;
    }

    Symbol *psym = addSymbol(std::move(s));
    Block b;
    b.kind = BlockKind::Type;
    b.close = closer.closeWord;
    b.needsEnd = closer.needsEnd;
    b.sym = psym;
    b.begOpen = openTok.beg;
    b.endOpen = openTok.end;
    blocks_.push_back(b);
    if (psym != nullptr) {
      containers_.push_back(containerFor(psym));
    }
    skipStatement();
  }

  void handleDeclare() {
    Token const openTok = cur_;
    // Any `Declare` in a record body is the member-procedure trigger, including
    // the ones this handler does not model (`declare constructor()`,
    // `declare operator`): fbc arms on them all — probed,
    // tools/probe_member_names.sh.
    armRecordBody();
    advance(); // past DECLARE
    if (cur_.kind != TokenKind::Keyword) {
      resetDoc();
      // A prototype line (`declare constructor(...)`, `declare operator...`)
      // inside a TYPE body declares no fields; its parameter identifiers must
      // not be captured as members (raymath.bi's Matrix/Vector2 declare their
      // constructors before their fields).
      skipStatement(/*suppressMemberCapture=*/true);
      return;
    }
    std::string const w = toLowerChars(cur_.text());
    if (w != "sub" && w != "function" && w != "property") {
      resetDoc();
      skipStatement(/*suppressMemberCapture=*/true);
      return;
    }
    Symbol s;
    if (w == "sub") {
      s.kind = SymbolKind::Sub;
    } else if (w == "function") {
      s.kind = SymbolKind::Function;
    } else {
      s.kind = SymbolKind::Property;
    }
    advance();
    if (cur_.kind == TokenKind::Identifier || cur_.kind == TokenKind::Keyword) {
      s.name = std::string(cur_.text());
      s.key = toLowerChars(s.name);
      s.selection.beg = cur_.beg;
      s.selection.end = cur_.end;
      advance();
    }
    s.range.beg = openTok.beg;
    s.range.end = currentLineEnd();
    s.signature = headerText(openTok);
    s.doc = takeDoc();
    if (cur_.kind == TokenKind::Symbol && cur_.text() == "(") {
      populateParams(s);
    }
    addSymbol(std::move(s));
    // After the signature the line holds only the return type and the
    // `lib`/`alias` clauses — a type expression, never a member. Capture-on
    // used to hunt a field name in `declare function f() as const zstring
    // ptr` and report `ptr` as one (drd/temp/inc/raylib.bi).
    skipStatement(/*suppressMemberCapture=*/true);
  }

  void handleVarDecls(SymbolKind k) {
    Token const openTok = cur_;
    std::string const doc = takeDoc();
    advance();
    bool atName = true;
    bool first = true;
    bool seenShared = false;
    int parenDepth = 0;
    int braceDepth = 0;
    for (;;) {
      TokenKind const tk = cur_.kind;
      if (tk == TokenKind::Newline || tk == TokenKind::Eof) {
        break;
      }
      if (tk == TokenKind::Symbol && cur_.text() == ":") {
        break;
      }
      if (tk == TokenKind::Symbol && cur_.text() == ",") {
        // A comma splits a declaration list (`DIM a = 1, b = 2`) only at
        // paren depth 0. Inside a parenthesized initializer it is an argument
        // separator (`type(x, .sectors(i).h, y)`, `Foo(a, b)`); treating it as
        // a declaration separator registered `.sectors` and `y` as fake
        // definitions, tripping false "duplicate definition" warnings
        // (drd/temp/src/engine.bas). The same holds for an initializer's
        // braces: `{ lgt.x, lgt.y, lgt.z }` separates *elements*, not names,
        // and each comma used to re-arm the name scan so `lgt` was registered
        // again per element (drd/temp/inc/rlights.bi).
        if (parenDepth == 0 && braceDepth == 0) {
          atName = true;
        }
        advance();
        continue;
      }
      if (tk == TokenKind::Symbol && cur_.text() == "(") {
        ++parenDepth;
        advance();
        continue;
      }
      if (tk == TokenKind::Symbol && cur_.text() == ")") {
        if (parenDepth > 0) {
          --parenDepth;
        }
        advance();
        continue;
      }
      if (tk == TokenKind::Symbol && cur_.text() == "{") {
        ++braceDepth;
        advance();
        continue;
      }
      if (tk == TokenKind::Symbol && cur_.text() == "}") {
        if (braceDepth > 0) {
          --braceDepth;
        }
        advance();
        continue;
      }
      if (atName) {
        if (tk == TokenKind::Identifier) {
          Symbol s;
          s.kind = k;
          s.name = std::string(cur_.text());
          s.key = toLowerChars(s.name);
          s.selection.beg = s.range.beg = cur_.beg;
          s.selection.end = s.range.end = cur_.end;
          // Storage tagging (§12.2 gate): the `Shared` modifier marks
          // a module-level var decl as visible inside procedures.
          // Honored only at module level (FreeBASIC.md §8): Shared
          // inside scope blocks is not supported, and const decls are
          // storage-less and always visible.
          s.shared = seenShared && blocks_.empty() && k == SymbolKind::Dim;
          s.signature = headerText(openTok);
          if (first) {
            s.doc = doc;
            first = false;
          }
          addSymbol(std::move(s));
          atName = false;
        } else if (tk == TokenKind::Keyword &&
                   toLowerChars(cur_.text()) == "as") {
          advance();
          // Type-first form: `DIM AS <type> name`. Skip the whole type
          // *chain* before the name, not one word: `dim as integer ptr a, b`
          // and `dim x as integer ptr` both end the type in `ptr`, which is
          // the pointer modifier and never the declared name (probed; fbc
          // compiles both). Stopping on `ptr` used to report it as a
          // reserved word used as a field name — the field is `a`/`b`/`x`.
          // Words are skipped while a name still follows; when the name
          // came first (`dim x as ...`) the rest of the line *is* type, so
          // every chain word goes, except a dangling `const`: fbc refuses
          // that (`error 273: Expected 'PTR' or 'POINTER'`) and the report
          // on the word is what says so.
          if (cur_.kind == TokenKind::Identifier) {
            advance();
          }
          while (isTypeChainWord(cur_)) {
            bool const danglingConst = !atName &&
                                       toLowerChars(cur_.text()) == "const" &&
                                       !isTypeChainWord(lex_.peek(0));
            if (danglingConst || (atName && endsStatement(lex_.peek(0)))) {
              break;
            }
            advance();
          }
          continue;
        } else if (tk == TokenKind::Keyword &&
                   toLowerChars(cur_.text()) == "shared") {
          seenShared = true;
          advance();
          continue;
        } else {
          // A reserved word where a declared name belongs. Only a record body
          // has a probed answer to give, and only the 16 fbc refuses as a field
          // name at all are reported — `dim next as integer` is a legal field,
          // and this path registers no name for it either way (FreeBASIC.md §7,
          // tools/probe_member_names.sh).
          if (tk == TokenKind::Keyword && inRecordBody() &&
              isNeverFieldName(toLowerChars(cur_.text()))) {
            addDiagnostic(cur_.beg, cur_.end, Severity::Error,
                          "invalid-member-name",
                          trf("'%s' is a reserved word and cannot be a field "
                              "name",
                              cur_.text()));
          }
          advance();
        }
      } else {
        advance();
      }
    }
  }

  void handleIf() {
    Token const ifTok = cur_;
    std::vector<Token> tail;
    tail.reserve(IF_TAIL_RESERVE);
    for (int i = 0; i < MAX_LINE_PEEK; ++i) {
      Token const t = lex_.peek(i);
      if (t.kind == TokenKind::Newline || t.kind == TokenKind::Eof ||
          t.kind == TokenKind::Comment || t.kind == TokenKind::DocComment) {
        break;
      }
      tail.push_back(t);
    }

    size_t idxThen = tail.size();
    for (size_t i = 0; i < tail.size(); ++i) {
      if (tail[i].kind == TokenKind::Keyword &&
          toLowerChars(tail[i].text()) == "then") {
        idxThen = i;
        break;
      }
    }

    bool singleLine = false;
    if (idxThen + 1 < tail.size()) {
      Token const firstAfter = tail[idxThen + 1];
      bool const colon =
          firstAfter.kind == TokenKind::Symbol && firstAfter.text() == ":";
      bool inlineEndIf = false;
      for (size_t i = idxThen + 1; i + 1 < tail.size(); ++i) {
        if (tail[i].kind == TokenKind::Keyword &&
            tail[i + 1].kind == TokenKind::Keyword &&
            toLowerChars(tail[i].text()) == "end" &&
            toLowerChars(tail[i + 1].text()) == "if") {
          inlineEndIf = true;
          break;
        }
      }
      singleLine = !colon && !inlineEndIf;
    }

    if (singleLine) {
      resetDoc();
      advance();
      skipStatement();
      return;
    }

    Block b;
    b.kind = BlockKind::If;
    b.close = "if";
    b.needsEnd = true;
    b.begOpen = ifTok.beg;
    b.endOpen = ifTok.end;
    attachScopeBlock(&b, "if");
    blocks_.push_back(b);
    // The then-branch is a branch like any other: its own scope, sibling to
    // the elseif/else ones, so no branch can resolve a sibling's declarations
    // through the block's container.
    openBranchScope("then");
    resetDoc();
    advance();
    skipStatement();
  }

  void handleEnd() {
    Token const endTok = cur_;
    advance(); // past END

    if (cur_.kind != TokenKind::Keyword) {
      resetDoc();
      skipStatement();
      return;
    }
    std::string const w = toLowerChars(cur_.text());

    if (w == "for" || w == "while") {
      std::string const expected = w == "for" ? "NEXT" : "WEND";
      addDiagnostic(cur_.beg, cur_.end, Severity::Error, "invalid-end",
                    trf("Expected '%s'", expected));
      // No block can be closed by `END FOR` / `END WHILE` (fbc rejects both
      // with `error 33: Illegal 'END'`), so this token is itself the evidence
      // that the innermost block ends before it — fbc's `error 13: Expected
      // 'NEXT' in 'end for'`. Close it there, then consume: an illegal closer
      // has no closer of its own to match.
      if (!blocks_.empty()) {
        closeBlockUnterminated(endTok.beg);
      }
      resetDoc();
      skipStatement();
      return;
    }

    BlockCloser c;
    if (!blockForCloser(w, &c) || !c.needsEnd) {
      resetDoc();
      skipStatement();
      return;
    }

    if (blocks_.empty()) {
      addDiagnostic(endTok.beg, cur_.end, Severity::Error, "stray-closer",
                    trf("%s without %s", "END " + uppercase(w), uppercase(w)));
      resetDoc();
      skipStatement();
      return;
    }

    Block const &top = blocks_.back();
    if (top.kind == c.kind && top.needsEnd) {
      closeBlock(cur_.end);
      advance();
      resetDoc();
      skipStatement();
      return;
    }

    closeBeforeMismatchedCloser(endTok.beg, top, c);
    if (!topClosesWith(c)) {
      resetDoc();
      skipStatement();
      return;
    }
    closeBlock(cur_.end);
    advance();
    resetDoc();
    skipStatement();
  }

  // The innermost block does not match this closer, so it ends *before* it —
  // the evidence a missing closer needs, and exactly where fbc puts its own
  // ("error 13: Expected 'NEXT', found 'end' in 'end sub'"). Closing it there
  // is what fixes the quick fix's insertion point, so the caller must then
  // re-try the closer against the block now on top.
  //
  // A record/enum body reports no separate `closer-mismatch`: a stray `END SUB`
  // in a `TYPE` body is just a statement that body cannot accept, which is
  // fbc's `error 19`, and the `unterminated-block` already names the expected
  // closer.
  void closeBeforeMismatchedCloser(uint32_t at, Block const &top,
                                   BlockCloser const &c) {
    if (!isMemberBodyKind(top.kind)) {
      addDiagnostic(at, cur_.end, Severity::Error, "closer-mismatch",
                    trf("Expected '%s'", displayFor(top)));
    }
    closeBlockUnterminated(at);
  }

  // Does the innermost block take this closer? The caller's mismatch path has
  // just closed one block, so this answers for the one below it: `end sub`
  // closing the `SUB` under a `FOR` it does not belong to is how a nested
  // unterminated block still nests correctly after the fix is applied.
  bool topClosesWith(BlockCloser const &c) {
    if (blocks_.empty()) {
      return false;
    }
    Block const &top = blocks_.back();
    return top.kind == c.kind && top.needsEnd == c.needsEnd;
  }

  void handlePlainCloser(const BlockCloser &c) {
    Token const closerTok = cur_;
    if (blocks_.empty()) {
      std::string_view openerName = "DO";
      if (c.kind == BlockKind::For) {
        openerName = "FOR";
      } else if (c.kind == BlockKind::While) {
        openerName = "WHILE";
      }
      addDiagnostic(closerTok.beg, closerTok.end, Severity::Error,
                    "stray-closer",
                    trf("%s without %s", uppercase(c.closeWord), openerName));
      resetDoc();
      skipStatement();
      return;
    }
    Block const &top = blocks_.back();
    if (top.kind == c.kind && !top.needsEnd) {
      closeBlock(closerTok.end);
      advance();
      resetDoc();
      skipStatement();
      return;
    }
    closeBeforeMismatchedCloser(closerTok.beg, top, c);
    if (!topClosesWith(c)) {
      resetDoc();
      skipStatement();
      return;
    }
    closeBlock(closerTok.end);
    advance();
    resetDoc();
    skipStatement();
  }

  void handlePreprocessor() {
    std::string const w = toLowerChars(preprocessorWord(cur_.text()));
    if (w == "if" || w == "ifdef" || w == "ifndef") {
      Block b;
      b.kind = BlockKind::PreprocIf;
      b.close = "#endif";
      b.needsEnd = false;
      b.begOpen = cur_.beg;
      b.endOpen = cur_.end;
      blocks_.push_back(b);
    } else if (w == "macro") {
      Block b;
      b.kind = BlockKind::PreprocMacro;
      b.close = "#endmacro";
      b.needsEnd = false;
      b.begOpen = cur_.beg;
      b.endOpen = cur_.end;
      blocks_.push_back(b);
    } else if (w == "endif") {
      if (!blocks_.empty() && blocks_.back().kind == BlockKind::PreprocIf) {
        closeBlock(cur_.end);
      } else {
        addDiagnostic(cur_.beg, cur_.end, Severity::Error, "stray-closer",
                      trf("%s without %s", "#" + uppercase(w), "#IF"));
      }
    } else if (w == "endmacro") {
      if (!blocks_.empty() && blocks_.back().kind == BlockKind::PreprocMacro) {
        closeBlock(cur_.end);
      } else {
        addDiagnostic(cur_.beg, cur_.end, Severity::Error, "stray-closer",
                      trf("%s without %s", "#" + uppercase(w), "#MACRO"));
      }
    } else if (w == "lang") {
      LangMode m;
      if (langFromDirective(cur_.text(), &m)) {
        applyLangDirective(m, cur_.beg, cur_.end);
      }
    }
  }
};

} // namespace

ParseResult parseDocument(std::string_view source) {
  return Parser(source).run();
}

} // namespace fblang
