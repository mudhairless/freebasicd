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
    if (dedupe && !containers_.empty()) {
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

  void closeBlock(uint32_t end) {
    Block const b = blocks_.back();
    blocks_.pop_back();
    out_.blockRanges.push_back({b.begOpen, end});
    if (b.sym != nullptr) {
      b.sym->range.end = end;
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
    BlockKind const kind = blocks_.back().kind;
    if (kind != BlockKind::Type && kind != BlockKind::Union &&
        kind != BlockKind::Enum) {
      return false;
    }
    if (acceptsBodyMember(kind, statementTokens())) {
      return false;
    }
    closeBlockUnterminated(cur_.beg);
    return true;
  }

  // The tokens of the statement starting at `cur_`, up to its statement end.
  // Same stop set as skipStatement — a newline, a comment, or a `:` separator —
  // so a `:`-separated statement is judged on its own tokens, and the lexer has
  // already merged `_` continuation lines into one logical line.
  std::vector<Token> statementTokens() {
    std::vector<Token> out;
    auto endsStatement = [](Token const &t) {
      return t.kind == TokenKind::Newline || t.kind == TokenKind::Eof ||
             t.kind == TokenKind::Comment || t.kind == TokenKind::DocComment ||
             (t.kind == TokenKind::Symbol && t.text() == ":");
    };
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
      } else if (inRecord && k == TokenKind::Symbol && cur_.text() == "," &&
                 parenDepth == 0) {
        // `as integer a, b` declares a whole list of members on one line.
        captureMember = true;
      }
      if (captureMember && k == TokenKind::Keyword &&
          toLowerChars(cur_.text()) == "as") {
        // Type-first member: `as <type> name`. Skip the type name (builtin
        // keyword or user-defined type) so the *member* is captured, not the
        // type — drd/temp/inc/world.bi's `as Wall walls(MAX_WALLS - 1)` used
        // to register `wall`. The member's signature still covers the full
        // line so the declared type survives for hover/resolve.
        advance();
        // The type is one name token; a keyword is consumed as part of the
        // type only while a member name still follows, so `as integer name`
        // keeps `name` as the member while `as name n` (`name` is a keyword
        // *type* name) and `as integer ptr p` keep `n`/`p`. Reserved words
        // are valid field names (fbc-verified): `as string name`.
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
        continue;
      }
      if (captureMember &&
          (k == TokenKind::Identifier ||
           (k == TokenKind::Keyword && toLowerChars(cur_.text()) != "as" &&
            toLowerChars(cur_.text()) != "ptr"))) {
        // Reserved-word member names (`as string name`) are captured like
        // identifiers. `ptr`/`const` are always type modifiers, never field
        // names (fbc rejects `as integer ptr` with a bare `ptr` member).
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
        handleVarDecls(SymbolKind::Dim);
        return;
      }
      resetDoc();
      skipStatement();
      return;
    }

    if (w == "end") {
      handleEnd();
      return;
    }
    if (w == "next" || w == "wend" || w == "loop") {
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
      handleVarDecls(w == "const" ? SymbolKind::Const : SymbolKind::Dim);
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
      }
      resetDoc();
      skipStatement();
      return;
    }
    if (w == "case") {
      if (blocks_.empty() || blocks_.back().kind != BlockKind::Select) {
        addDiagnostic(cur_.beg, cur_.end, Severity::Error, "stray-closer",
                      trf("%s without %s", "CASE", "SELECT"));
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
    advance(); // '.'
    s.name = std::string(cur_.text());
    s.key = toLowerChars(s.name);
    s.selection.beg = cur_.beg;
    s.selection.end = cur_.end;
    advance();
  }

  void handleDeclBlock(const std::string &openWord, SymbolKind k) {
    Token const openTok = cur_;
    BlockCloser closer;
    blockForOpener(openWord, &closer);
    advance();

    Symbol s;
    s.kind = k;
    s.range.beg = openTok.beg;
    if (cur_.kind == TokenKind::Identifier || cur_.kind == TokenKind::Keyword ||
        (k == SymbolKind::Operator && cur_.kind == TokenKind::Symbol)) {
      s.name = std::string(cur_.text());
      s.key = toLowerChars(s.name);
      s.selection.beg = cur_.beg;
      s.selection.end = cur_.end;
      advance();
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
      containers_.push_back(Container(psym));
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
    BlockCloser closer;
    blockForOpener("type", &closer);
    advance();

    Symbol s;
    s.kind = SymbolKind::Type;
    s.range.beg = openTok.beg;
    bool hasName = false;
    if (cur_.kind == TokenKind::Identifier || cur_.kind == TokenKind::Keyword) {
      hasName = true;
      s.name = std::string(cur_.text());
      s.key = toLowerChars(s.name);
      s.selection.beg = cur_.beg;
      s.selection.end = cur_.end;
      advance();
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
      skipStatement();
      return;
    }

    Symbol *psym = hasName ? addSymbol(std::move(s)) : nullptr;
    Block b;
    b.kind = BlockKind::Type;
    b.close = closer.closeWord;
    b.needsEnd = closer.needsEnd;
    b.sym = psym;
    b.begOpen = openTok.beg;
    b.endOpen = openTok.end;
    blocks_.push_back(b);
    if (psym != nullptr) {
      containers_.push_back(Container(psym));
    }
    skipStatement();
  }

  void handleDeclare() {
    Token const openTok = cur_;
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
    skipStatement();
  }

  void handleVarDecls(SymbolKind k) {
    Token const openTok = cur_;
    std::string const doc = takeDoc();
    advance();
    bool atName = true;
    bool first = true;
    bool seenShared = false;
    int parenDepth = 0;
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
        // (drd/temp/src/engine.bas).
        if (parenDepth == 0) {
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
          // Type-first form: `DIM AS <type> name`. Skip the type
          // (builtin keyword or user-defined type) before the name.
          if ((cur_.kind == TokenKind::Keyword &&
               isBuiltinType(toLowerChars(cur_.text()))) ||
              cur_.kind == TokenKind::Identifier) {
            advance();
          }
          continue;
        } else if (tk == TokenKind::Keyword &&
                   toLowerChars(cur_.text()) == "shared") {
          seenShared = true;
          advance();
          continue;
        } else {
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
