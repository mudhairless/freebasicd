#include "resolve.h"

#include "index.h"
#include "lexer.h"
#include "parser.h"
#include "symbols.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fblang {

namespace {

// A declaration's cross-snapshot identity: its owning file plus the selection
// range of its name token. Unique per declaration: a (path, selection) pair
// pins one Symbol across index snapshots and fresh parses of the same file.
struct DeclIdentity {
  std::string path;
  std::uint32_t beg = 0;
  std::uint32_t end = 0;
};

DeclIdentity identityOf(CrossDecl const &d, std::string const &fallbackPath) {
  return {d.file ? d.file->path : fallbackPath, d.decl->selection.beg,
          d.decl->selection.end};
}

// When the target is a module-scope root of the requesting file (found in-file,
// `file == nullptr`), give it its index identity (`fileAt(normalizedPath)`)
// so a same declaration re-resolved through tier 2 by a different file's
// closure matches on identity — the declaration's index copy and its fresh
// in-file parse share the same (path, selection).
CrossDecl canonicalTarget(CrossDecl const &target,
                          std::string const &normalizedPath,
                          WorkspaceIndex const *index) {
  if (target.decl == nullptr || target.file || index == nullptr ||
      !target.decl->moduleScope) {
    return target;
  }
  std::shared_ptr<IndexedFile const> const file = index->fileAt(normalizedPath);
  if (!file) {
    return target;
  }
  for (Symbol const &root : file->roots) {
    if (root.key == target.decl->key &&
        root.selection.beg == target.decl->selection.beg &&
        root.selection.end == target.decl->selection.end) {
      return CrossDecl{file, &root};
    }
  }
  return target;
}

bool sameIdentity(DeclIdentity const &a, DeclIdentity const &b) {
  return a.path == b.path && a.beg == b.beg && a.end == b.end;
}

// The declaration a token in `d` resolves to, expressed as a CrossDecl:
// in-file for the declaration's own file (the storage gate applies), across
// the workspace otherwise. `nullptr index` resolves in-file only.
CrossDecl resolveIn(AnalyzedDoc const &d, std::string const &fpath,
                    std::uint32_t off, WorkspaceIndex const *index,
                    std::string const &declPath) {
  if (index == nullptr || fpath == declPath) {
    if (Symbol const *const local = resolveAt(d, off)) {
      return CrossDecl{nullptr, local};
    }
    return {};
  }
  return resolveAcross(d, fpath, off, *index);
}

// Deepest node of `sym` (inclusive, so a cursor on the closer token still
// lands inside the block) that contains `off`, or nullptr.
Symbol const *deepestNesting(Symbol const &sym, std::uint32_t off) {
  if (off < sym.range.beg || off > sym.range.end) {
    return nullptr;
  }
  for (auto const &c : sym.children) {
    if (Symbol const *hit = deepestNesting(c, off)) {
      return hit;
    }
  }
  return &sym;
}

bool isScopeKind(SymbolKind kind) {
  switch (kind) {
  case SymbolKind::Sub:
  case SymbolKind::Function:
  case SymbolKind::Property:
  case SymbolKind::Constructor:
  case SymbolKind::Destructor:
  case SymbolKind::Operator:
  case SymbolKind::Type:
  case SymbolKind::Union:
  case SymbolKind::Enum:
  case SymbolKind::Namespace:
  case SymbolKind::Scope:
    return true;
  case SymbolKind::Const:
  case SymbolKind::Dim:
  case SymbolKind::Label:
  case SymbolKind::Parameter:
  case SymbolKind::Variable:
    return false;
  }
  return false;
}

Symbol const *findParent(Symbol const &cur, Symbol const *node) {
  for (auto const &c : cur.children) {
    if (&c == node) {
      return &cur;
    }
    if (Symbol const *p = findParent(c, node)) {
      return p;
    }
  }
  return nullptr;
}

// Name token (identifier, or a reserved word used as a name) at `off`, or
// nullptr. A cursor between two characters is considered inside a token that
// spans it. Reserved words are name-like because FreeBASIC lets most of them
// become member names (`as string name`) and, by extension, other declared
// names; scope markers carry no key, so keyword usage resolution only ever
// matches a real keyword-named declaration (FreeBASIC.md §2, fbc-verified).
Token const *tokenAt(std::vector<Token> const &tokens, std::uint32_t off) {
  for (auto const &t : tokens) {
    if ((t.kind == TokenKind::Identifier || t.kind == TokenKind::Keyword) &&
        t.beg <= off && off <= t.end) {
      return &t;
    }
  }
  return nullptr;
}

SourceRange rangeOf(Token const &t) { return {t.beg, t.end}; }

// Module-level name candidates of `roots`, in a fixed order: every root
// (declaration order), then the members of each *non-explicit* Enum. A plain
// `enum color ... end enum` publishes its members as module-scope constants
// (FreeBASIC.md §8, KeyPgEnum, fbc-verified: bare `green` resolves at module
// level, in procedures, and across the include closure); `enum <name>
// explicit` restricts its members to qualified `Name.member` access, so they
// are not candidates. Members trail all roots so a same-keyed module Dim
// (`dim green` after the enum member `green`) wins — fbc accepts that dim and
// the later declaration shadows the member.
std::vector<Symbol const *>
moduleLevelCandidates(std::vector<Symbol> const &roots) {
  std::vector<Symbol const *> out;
  out.reserve(roots.size() + 8);
  for (Symbol const &r : roots) {
    out.push_back(&r);
  }
  for (Symbol const &r : roots) {
    if (r.kind != SymbolKind::Enum || r.explicitEnum) {
      continue;
    }
    for (Symbol const &m : r.children) {
      out.push_back(&m);
    }
  }
  return out;
}

// The declaration a usage at `off` resolves to, over a pre-lexed stream. This
// is the single resolution walk shared by analyze's occurrence sweep and the
// on-demand resolution API (which used to re-lex per call).
//
// Honors the §12.2 storage gate: a usage inside a procedure body (its control
// blocks included) may only match a module-scope Dim-kind declaration carrying
// the `Shared` modifier; at module level, and inside module-level control
// blocks (scope/for/if/...), every root is visible. Procedure/type/enum/const
// roots are never gated.
Symbol const *declAt(ParseResult const &parse, std::vector<Token> const &tokens,
                     std::uint32_t off) {
  Token const *tok = tokenAt(tokens, off);
  if (tok == nullptr) {
    return nullptr;
  }
  std::string const key = toLowerChars(tok->text());
  Symbol const *const siteScope = innermostScope(parse, off);
  bool const storageGated = insideProcedureBody(parse, siteScope);
  for (Symbol const *cur = siteScope;;) {
    if (cur != nullptr) {
      for (auto const &c : cur->children) {
        if (c.key.empty() || c.key != key) {
          continue;
        }
        return &c;
      }
    } else {
      // Module level: the roots plus the members of non-explicit enums
      // (module-scope constants, FreeBASIC.md §8).
      for (Symbol const *c : moduleLevelCandidates(parse.roots)) {
        if (c->key.empty() || c->key != key) {
          continue;
        }
        bool const gated =
            storageGated && c->kind == SymbolKind::Dim && !c->shared;
        if (!gated) {
          return c;
        }
      }
      return nullptr;
    }
    cur = parentOf(parse, cur);
  }
}

// Fill `occurrences` on every Symbol of `parse` (file roots tagged
// moduleScope) with every usage that resolves to it, in source order.
// `const_cast` is safe here: the walk is read-only and the decl pointer is,
// by construction, into the tree we are filling.
void attachOccurrences(ParseResult &parse, std::vector<Token> const &tokens) {
  for (Symbol &root : parse.roots) {
    // Scope nodes are structure, not declarations: a module-level control
    // block must never look like a file-root decl (index projection skips
    // empty keys; be explicit anyway) or leak its block-local dims to module
    // scope.
    if (root.kind != SymbolKind::Scope) {
      root.moduleScope = true;
    }
  }
  for (Token const &t : tokens) {
    if (t.kind != TokenKind::Identifier) {
      continue;
    }
    Symbol const *const decl = declAt(parse, tokens, t.beg);
    if (decl == nullptr ||
        (t.beg == decl->selection.beg && t.end == decl->selection.end)) {
      continue;
    }
    bool const siteModuleScope = innermostScope(parse, t.beg) == nullptr;
    const_cast<Symbol *>(decl)->occurrences.push_back(
        {rangeOf(t), siteModuleScope});
  }
}

bool isDirectiveWordChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// A `#pragma once` Preprocessor line anywhere in the source. Independent of
// the include sweep (also over Preprocessor tokens; a single pass over the
// few directive lines keeps both walks cheap and independent).
bool detectPragmaOnce(std::string_view source,
                      std::vector<Token> const &tokens) {
  for (Token const &t : tokens) {
    if (t.kind != TokenKind::Preprocessor) {
      continue;
    }
    std::string_view const line =
        source.substr(t.beg, t.end - t.beg); // starts at '#'
    std::size_t i = 1;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
      ++i;
    }
    std::size_t const wbeg = i;
    while (i < line.size() && isDirectiveWordChar(line[i])) {
      ++i;
    }
    if (toLowerChars(line.substr(wbeg, i - wbeg)) != "pragma") {
      continue;
    }
    std::size_t r = i;
    while (r < line.size() && (line[r] == ' ' || line[r] == '\t')) {
      ++r;
    }
    std::size_t const vbeg = r;
    while (r < line.size() && isDirectiveWordChar(line[r])) {
      ++r;
    }
    if (r > vbeg && toLowerChars(line.substr(vbeg, r - vbeg)) == "once") {
      return true;
    }
  }
  return false;
}

// Extract `#include [once] ["]literal["]` directives from the whole-line
// Preprocessor tokens. Ranges are byte offsets into `source`.
void collectIncludes(std::string_view source, std::vector<Token> const &tokens,
                     std::vector<IncludeDirective> *out) {
  for (Token const &t : tokens) {
    if (t.kind != TokenKind::Preprocessor) {
      continue;
    }
    std::string_view const line =
        source.substr(t.beg, t.end - t.beg); // starts at '#'
    std::size_t i = 1;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
      ++i;
    }
    std::size_t const wbeg = i;
    while (i < line.size() && isDirectiveWordChar(line[i])) {
      ++i;
    }
    if (toLowerChars(line.substr(wbeg, i - wbeg)) != "include") {
      continue;
    }
    IncludeDirective inc;
    inc.line.beg = t.beg;
    inc.line.end = t.end;
    std::size_t r = i;
    while (r < line.size() && (line[r] == ' ' || line[r] == '\t')) {
      ++r;
    }
    std::size_t const onceLen = 4;
    if (r + onceLen <= line.size() &&
        toLowerChars(line.substr(r, onceLen)) == "once" &&
        (r + onceLen == line.size() || line[r + onceLen] == ' ' ||
         line[r + onceLen] == '\t')) {
      inc.once = true;
      r += onceLen;
      while (r < line.size() && (line[r] == ' ' || line[r] == '\t')) {
        ++r;
      }
    }
    if (r >= line.size()) {
      out->push_back(std::move(inc));
      continue;
    }
    std::uint32_t const base = t.beg;
    if (line[r] == '"') {
      std::size_t q = r + 1;
      while (q < line.size() && line[q] != '"') {
        ++q;
      }
      inc.literal = std::string(line.substr(r + 1, q - r - 1));
      inc.target.beg = base + static_cast<std::uint32_t>(r + 1);
      inc.target.end = base + static_cast<std::uint32_t>(q);
    } else {
      std::size_t k = r;
      while (k < line.size() && line[k] != ' ' && line[k] != '\t') {
        ++k;
      }
      inc.literal = std::string(line.substr(r, k - r));
      inc.target.beg = base + static_cast<std::uint32_t>(r);
      inc.target.end = base + static_cast<std::uint32_t>(k);
    }
    out->push_back(std::move(inc));
  }
}

} // namespace

// Exported (resolve.h): parent of `node` in the parse symbol tree (the
// declaration block or module root owning it), or nullptr for a file root.
// Enum members are Const children of their Enum root — use this to tell an
// enum member from a statement-level Const (semantic token classification).
Symbol const *parentOf(ParseResult const &parse, Symbol const *node) {
  return parentOf(parse.roots, node);
}

// Overload over a bare root list: cross-file resolution may land on a decl
// owned by an IndexedFile (workspace snapshot), which stores roots without a
// ParseResult wrapper.
Symbol const *parentOf(std::vector<Symbol> const &roots, Symbol const *node) {
  for (auto const &root : roots) {
    if (Symbol const *p = findParent(root, node)) {
      return p;
    }
  }
  return nullptr;
}

Symbol const *innermostScope(ParseResult const &parse, std::uint32_t off) {
  Symbol const *best = nullptr;
  for (auto const &root : parse.roots) {
    if (Symbol const *d = deepestNesting(root, off)) {
      best = d;
    }
  }
  while (best != nullptr && !isScopeKind(best->kind)) {
    best = parentOf(parse, best);
  }
  return best;
}

bool insideProcedureBody(ParseResult const &parse, Symbol const *siteScope) {
  for (Symbol const *cur = siteScope; cur != nullptr;
       cur = parentOf(parse, cur)) {
    switch (cur->kind) {
    case SymbolKind::Sub:
    case SymbolKind::Function:
    case SymbolKind::Property:
    case SymbolKind::Constructor:
    case SymbolKind::Destructor:
    case SymbolKind::Operator:
      return true;
    default:
      break;
    }
  }
  return false;
}

AnalyzedDoc analyze(std::string_view source) {
  AnalyzedDoc doc;
  doc.parse = parseDocument(source);
  Lexer lx(source);
  for (;;) {
    Token const t = lx.next();
    doc.tokens.push_back(t);
    if (t.kind == TokenKind::Eof) {
      break;
    }
  }
  attachOccurrences(doc.parse, doc.tokens);
  collectIncludes(source, doc.tokens, &doc.includes);
  doc.pragmaOnce = detectPragmaOnce(source, doc.tokens);
  return doc;
}

Symbol const *resolveAt(AnalyzedDoc const &doc, std::uint32_t off) {
  return declAt(doc.parse, doc.tokens, off);
}

std::vector<Occurrence> occurrencesOf(AnalyzedDoc const &doc,
                                      Symbol const &decl) {
  (void)doc; // precondition: `decl` points into doc.parse's tree
  return decl.occurrences;
}

SourceRange tokenRangeAt(AnalyzedDoc const &doc, std::uint32_t off) {
  if (Token const *const tok = tokenAt(doc.tokens, off)) {
    return {tok->beg, tok->end};
  }
  return {};
}

CrossDecl resolveAcross(AnalyzedDoc const &doc,
                        std::string const &normalizedPath, std::uint32_t off,
                        WorkspaceIndex const &index) {
  Token const *const tok = tokenAt(doc.tokens, off);
  if (tok == nullptr) {
    return {};
  }

  // Tier 1: in-file scopes, shadowing wins.
  if (Symbol const *const local = declAt(doc.parse, doc.tokens, off)) {
    return CrossDecl{nullptr, local};
  }

  std::string const key = toLowerChars(tok->text());
  bool const storageGated =
      insideProcedureBody(doc.parse, innermostScope(doc.parse, off));
  auto gated = [storageGated](Symbol const &root) {
    return storageGated && root.kind == SymbolKind::Dim && !root.shared;
  };

  // Tier 2: module scope of each closure file, textual include pre-order,
  // first key match. The closure is treated as one textual module (FreeBASIC
  // .md §9): the same storage gate applies to its roots as to the requesting
  // file's own module level, and non-explicit enum members resolve like module
  // constants regardless of the gate (probe-verified: bare `green` from a
  // `.bi` enum resolves in the includer).
  for (std::string const &closurePath :
       index.transitiveIncludes(normalizedPath)) {
    std::shared_ptr<IndexedFile const> const file = index.fileAt(closurePath);
    if (!file) {
      continue;
    }
    for (Symbol const *c : moduleLevelCandidates(file->roots)) {
      if (c->key == key && !gated(*c)) {
        return CrossDecl{file, c};
      }
    }
  }

  // Tier 3: lenient `byKey` workspace fallback for still-unincluded headers
  // (recorded divergence, FreeBASIC.md §12). byKey indexes file roots only,
  // so a procedure-local name can never resolve here.
  for (KeyedDecl const &kd : index.byKey(key)) {
    if (!gated(*kd.decl)) {
      return CrossDecl{kd.file, kd.decl};
    }
  }
  return {};
}

std::vector<OccurrenceSite> occurrencesAcross(AnalyzedDoc const &doc,
                                              std::string const &normalizedPath,
                                              std::uint32_t off,
                                              WorkspaceIndex const *index,
                                              ContentProvider const &content) {
  std::vector<OccurrenceSite> out;
  if (tokenAt(doc.tokens, off) == nullptr) {
    return out;
  }

  CrossDecl target;
  if (index != nullptr) {
    target = resolveAcross(doc, normalizedPath, off, *index);
  } else if (Symbol const *const local = resolveAt(doc, off)) {
    target = CrossDecl{nullptr, local};
  }
  if (target.decl == nullptr) {
    return out;
  }

  CrossDecl const canon = canonicalTarget(target, normalizedPath, index);
  DeclIdentity const self = identityOf(canon, normalizedPath);
  std::string const declPath = canon.file ? canon.file->path : normalizedPath;

  // Candidate files: the requesting file, its forward include closure, the
  // declaration's own file (a tier-3 byKey hit can land outside the closure),
  // and reverse reachability — every file whose own closure reaches the
  // declaration's file, so a rename at a header declaration covers all
  // includers. Insertion order keeps the requesting file first, the closure
  // textual-pre-order next (matches resolveAcross), and reverse files after.
  std::vector<std::string> files;
  auto addFile = [&files](std::string const &f) {
    if (std::find(files.begin(), files.end(), f) == files.end()) {
      files.push_back(f);
    }
  };
  addFile(normalizedPath);
  if (index != nullptr) {
    for (std::string const &p : index->transitiveIncludes(normalizedPath)) {
      addFile(p);
    }
    addFile(declPath);
    for (auto const &f : index->snapshot()) {
      for (std::string const &p : index->transitiveIncludes(f->path)) {
        if (p == declPath) {
          addFile(f->path);
          break;
        }
      }
    }
  }

  for (std::string const &fpath : files) {
    std::shared_ptr<DocumentContent const> const dc = content(fpath);
    if (!dc) {
      continue;
    }
    AnalyzedDoc const &d = dc->analysis;
    for (Token const &t : d.tokens) {
      if (t.kind != TokenKind::Identifier ||
          toLowerChars(t.text()) != target.decl->key) {
        continue;
      }
      CrossDecl const r = resolveIn(d, fpath, t.beg, index, declPath);
      if (r.decl == nullptr) {
        continue;
      }
      if (sameIdentity(identityOf(r, fpath), self)) {
        out.push_back(OccurrenceSite{fpath, {t.beg, t.end}});
      }
    }
  }

  std::sort(out.begin(), out.end(),
            [](OccurrenceSite const &a, OccurrenceSite const &b) {
              if (a.file != b.file) {
                return a.file < b.file;
              }
              return a.range.beg < b.range.beg;
            });
  return out;
}

std::vector<Symbol const *> visibleSymbols(AnalyzedDoc const &doc,
                                           std::uint32_t off) {
  ParseResult const &parse = doc.parse;
  std::vector<Symbol const *> out;
  Symbol const *const siteScope = innermostScope(parse, off);
  bool const storageGated = insideProcedureBody(parse, siteScope);
  for (Symbol const *cur = siteScope;;) {
    if (cur != nullptr) {
      for (auto const &c : cur->children) {
        if (c.key.empty()) {
          continue;
        }
        out.push_back(&c);
      }
    } else {
      for (Symbol const *c : moduleLevelCandidates(parse.roots)) {
        if (c->key.empty()) {
          continue;
        }
        // §12.2 gate: from inside a procedure body, module-level Dim-kind
        // names require the Shared modifier; at module level (control blocks
        // included) everything shows.
        if (storageGated && c->kind == SymbolKind::Dim && !c->shared) {
          continue;
        }
        out.push_back(c);
      }
    }
    if (cur == nullptr) {
      break;
    }
    cur = parentOf(parse, cur);
  }
  return out;
}

std::string declaredTypeName(Symbol const &decl) {
  std::string_view const sig = decl.signature;
  size_t i = 0;
  while (i < sig.size()) {
    while (i < sig.size() && (sig[i] == ' ' || sig[i] == '\t')) {
      ++i;
    }
    size_t const wb = i;
    while (i < sig.size() &&
           ((sig[i] >= 'a' && sig[i] <= 'z') ||
            (sig[i] >= 'A' && sig[i] <= 'Z') || sig[i] == '_' ||
            (sig[i] >= '0' && sig[i] <= '9'))) {
      ++i;
    }
    std::string_view const w = sig.substr(wb, i - wb);
    if (toLowerChars(w) == "as") {
      while (i < sig.size() && (sig[i] == ' ' || sig[i] == '\t')) {
        ++i;
      }
      size_t const tb = i;
      while (i < sig.size() &&
             ((sig[i] >= 'a' && sig[i] <= 'z') ||
              (sig[i] >= 'A' && sig[i] <= 'Z') || sig[i] == '_' ||
              (sig[i] >= '0' && sig[i] <= '9'))) {
        ++i;
      }
      std::string_view const t = sig.substr(tb, i - tb);
      std::string const tl = toLowerChars(t);
      // `as const integer x`, `as byref UDT x`: drop a leading type modifier
      // so the type name itself is returned.
      if (tl == "const" || tl == "byref" || tl == "byval" || tl == "shared" ||
          tl == "static" || tl == "export") {
        while (i < sig.size() && (sig[i] == ' ' || sig[i] == '\t')) {
          ++i;
        }
        size_t const tb2 = i;
        while (i < sig.size() &&
               ((sig[i] >= 'a' && sig[i] <= 'z') ||
                (sig[i] >= 'A' && sig[i] <= 'Z') || sig[i] == '_' ||
                (sig[i] >= '0' && sig[i] <= '9'))) {
          ++i;
        }
        if (tb2 < i) {
          return std::string(sig.substr(tb2, i - tb2));
        }
      }
      if (tb < i) {
        return std::string(sig.substr(tb, i - tb));
      }
      return {};
    }
  }
  return {};
}

Symbol const *findMember(Symbol const &typeDecl, std::string const &memberKey) {
  for (Symbol const &c : typeDecl.children) {
    if (!c.key.empty() && c.key == memberKey) {
      return &c;
    }
  }
  return nullptr;
}

namespace {

// The Type/Union root of `roots` whose key equals `typeKey`, or nullptr.
Symbol const *typeRootIn(std::vector<Symbol> const &roots,
                         std::string const &typeKey) {
  for (Symbol const &r : roots) {
    if ((r.kind == SymbolKind::Type || r.kind == SymbolKind::Union) &&
        !r.key.empty() && r.key == typeKey) {
      return &r;
    }
  }
  return nullptr;
}

// Name token at `off` (identifier or keyword used as a name) and its index in
// `tokens`, or (nullptr, 0). Reserved words can be member names, so the
// member-access harness must find them (FreeBASIC.md §2).
std::pair<Token const *, size_t> tokenAndIndex(std::vector<Token> const &tokens,
                                               std::uint32_t off) {
  for (size_t i = 0; i < tokens.size(); ++i) {
    Token const &t = tokens[i];
    if ((t.kind == TokenKind::Identifier || t.kind == TokenKind::Keyword) &&
        t.beg <= off && off <= t.end) {
      return {&t, i};
    }
  }
  return {nullptr, 0};
}

// The `with`-target identifier of the innermost WITH block containing `off`,
// or nullptr when no enclosing `with` exists or its target is not a plain
// identifier.
Token const *withTargetOf(ParseResult const &parse,
                          std::vector<Token> const &tokens, std::uint32_t off) {
  Symbol const *scope = innermostScope(parse, off);
  while (scope != nullptr) {
    if (scope->kind == SymbolKind::Scope && scope->name == "with") {
      // `scope->range.beg` is the `with` keyword; the target is the first
      // identifier on that line.
      for (Token const &t : tokens) {
        if (t.beg < scope->range.beg) {
          continue;
        }
        if (t.kind == TokenKind::Identifier) {
          return &t;
        }
        if (t.kind == TokenKind::Newline) {
          return nullptr;
        }
      }
      return nullptr;
    }
    scope = parentOf(parse, scope);
  }
  return nullptr;
}

} // namespace

CrossDecl findTypeDecl(AnalyzedDoc const &doc,
                       std::string const &normalizedPath,
                       std::string const &typeKey,
                       WorkspaceIndex const *index) {
  if (Symbol const *const t = typeRootIn(doc.parse.roots, typeKey)) {
    return CrossDecl{nullptr, t};
  }
  if (index == nullptr) {
    return {};
  }
  for (std::string const &p : index->transitiveIncludes(normalizedPath)) {
    std::shared_ptr<IndexedFile const> const file = index->fileAt(p);
    if (!file) {
      continue;
    }
    if (Symbol const *const t = typeRootIn(file->roots, typeKey)) {
      return CrossDecl{file, t};
    }
  }
  // Tier 3, mirroring resolveAcross (§12.2): a type the include closure does
  // not declare still resolves to a workspace root of the same key — a header
  // that is not included yet. Keeps `expr.member` hover/definition on the same
  // footing as usage resolution when the declaring header sits outside the
  // requesting file's closure (e.g. an unresolvable include path).
  for (KeyedDecl const &kd : index->byKey(typeKey)) {
    if (kd.decl != nullptr && !kd.decl->key.empty() &&
        (kd.decl->kind == SymbolKind::Type ||
         kd.decl->kind == SymbolKind::Union)) {
      return CrossDecl{kd.file, kd.decl};
    }
  }
  return {};
}

namespace {

// Hop over a balanced `(...)` / `[...]` group whose closing token sits at
// `closeTokIdx`, landing on the identifier (or operator-preceded reserved
// word) that names the receiver (`arr` in `.arr(i).field`). Returns the
// receiver token index, or npos when none exists — an intrinsic call like
// `upper(s).x` never opens a chain and bottoms out implicit.
size_t hopReceiver(std::vector<Token> const &tokens, size_t closeTokIdx) {
  int depth = 1;
  size_t j = closeTokIdx;
  while (j > 0 && depth > 0) {
    --j;
    TokenKind const jk = tokens[j].kind;
    if (jk == TokenKind::Symbol &&
        (tokens[j].text() == ")" || tokens[j].text() == "]")) {
      ++depth;
    } else if (jk == TokenKind::Symbol &&
               (tokens[j].text() == "(" || tokens[j].text() == "[")) {
      --depth;
    }
  }
  if (j > 0 && tokens[j - 1].kind == TokenKind::Identifier) {
    return j - 1;
  }
  if (j > 1 && tokens[j - 1].kind == TokenKind::Keyword &&
      tokens[j - 2].kind == TokenKind::Symbol &&
      (tokens[j - 2].text() == "." || tokens[j - 2].text() == "->")) {
    return j - 1;
  }
  return std::string::npos;
}

// One link of a `.`/`->` member chain.
struct ChainSeg {
  std::string name; // lowercased lookup key; empty for the virtual completing
                    // member (the cursor is right after an operator)
  size_t tokIdx = 0;
};

// The shared leftward chain walk: pushes the segment for the name token at
// `memberTokIdx`, then keeps walking as long as segments alternate name,
// operator, name. A reserved word can name a member, but only mid-chain
// (`v.name.x`); a bottoming keyword is recorded in `keywordTokIdx` so the
// caller can try it as an enum-name root (`enum color` — `color` is the
// graphics intrinsic). A `)`/`]` receiver hops to the identifier that opens
// the call/array. `implicit` marks a chain whose base is not a plain variable:
// a leading dot (base = `with` target), or a bottom after `=` / `(` / a
// keyword / a parenthesized call with no name receiver.
void collectChainWalk(std::vector<Token> const &tokens, size_t memberTokIdx,
                      std::vector<ChainSeg> *segs, bool *implicit,
                      size_t *keywordTokIdx) {
  size_t i = memberTokIdx;
  for (;;) {
    segs->push_back({toLowerChars(tokens[i].text()), i});
    if (i == 0) {
      break; // leftmost segment is a variable
    }
    Token const &beforeOp = tokens[i - 1];
    if (beforeOp.kind != TokenKind::Symbol ||
        (beforeOp.text() != "." && beforeOp.text() != "->")) {
      break; // this segment is a plain variable; chain ends
    }
    if (i < 2) {
      *implicit = true; // leading `.member`
      break;
    }
    Token const &lhs = tokens[i - 2];
    if (lhs.kind == TokenKind::Identifier) {
      i = i - 2; // `a.b.c`: keep walking
      continue;
    }
    if (lhs.kind == TokenKind::Keyword && i >= 3 &&
        tokens[i - 3].kind == TokenKind::Symbol &&
        (tokens[i - 3].text() == "." || tokens[i - 3].text() == "->")) {
      // Reserved words can name members, but only mid-chain (`v.name.x`).
      i = i - 2;
      continue;
    }
    if (lhs.kind == TokenKind::Symbol &&
        (lhs.text() == ")" || lhs.text() == "]")) {
      // Indexed/called member: `.arr(i).field`. Hop over the call to the
      // identifier that opens it.
      size_t const r = hopReceiver(tokens, i - 2);
      if (r != std::string::npos) {
        i = r;
        continue;
      }
      *implicit = true;
      break;
    }
    if (lhs.kind == TokenKind::Keyword) {
      // `= color . green`: the chain bottomed out on a keyword. A keyword is
      // never a plain variable (fbc rejects `dim name`), but an *enum name*
      // may be one (fbc-verified: `enum color` compiles even though `color`
      // is the graphics intrinsic). Record the keyword so the base handling
      // below can try it as an enum name root before the `with` target.
      *keywordTokIdx = i - 2;
    }
    *implicit = true; // `.member` after `=`, `(`, a keyword, ...
    break;
  }
}

// Collect the member chain ending at `memberTokIdx`, root-first on return.
// Hover mode (`virtualMember == false`): `memberTokIdx` is a member name token
// preceded by the `.`/`->` operator. Completion mode (`true`): `memberTokIdx`
// is the operator token itself — the member being typed has no token, so the
// collection pushes a virtual empty-key segment first and anchors the walk at
// the operator's left-hand side (a name, a reserved-word member, a `)`-opened
// call, or nothing at all for a leading dot).
void collectMemberChain(std::vector<Token> const &tokens, size_t memberTokIdx,
                        bool virtualMember, std::vector<ChainSeg> *segs,
                        bool *implicit, size_t *keywordTokIdx) {
  if (!virtualMember) {
    collectChainWalk(tokens, memberTokIdx, segs, implicit, keywordTokIdx);
    std::reverse(segs->begin(), segs->end());
    return;
  }
  segs->push_back({std::string(), memberTokIdx}); // the completing member
  if (memberTokIdx == 0) {
    *implicit = true; // `.<cursor>` at line start: base is the `with` target
  } else {
    Token const &lhs = tokens[memberTokIdx - 1];
    size_t receiver = std::string::npos;
    if (lhs.kind == TokenKind::Identifier) {
      receiver = memberTokIdx - 1;
    } else if (lhs.kind == TokenKind::Keyword && memberTokIdx >= 2 &&
               tokens[memberTokIdx - 2].kind == TokenKind::Symbol &&
               (tokens[memberTokIdx - 2].text() == "." ||
                tokens[memberTokIdx - 2].text() == "->")) {
      // Reserved-word member receiver (`v.name.`).
      receiver = memberTokIdx - 1;
    } else if (lhs.kind == TokenKind::Symbol &&
               (lhs.text() == ")" || lhs.text() == "]")) {
      receiver = hopReceiver(tokens, memberTokIdx - 1);
    } else if (lhs.kind == TokenKind::Keyword) {
      // `= color . <cursor>`: the operator's lhs is a keyword. A keyword is
      // never a plain variable, but *enum names* may be reserved words, so
      // hand it to the base handling as a potential enum-name root.
      *keywordTokIdx = memberTokIdx - 1;
    }
    if (receiver != std::string::npos) {
      collectChainWalk(tokens, receiver, segs, implicit, keywordTokIdx);
    } else {
      *implicit = true;
    }
  }
  std::reverse(segs->begin(), segs->end());
}

enum class ChainBase { None, NoOwner, Resolved };

// Resolves the chain base (`segs`) into the Type/Union/Enum "owner"
// declaration whose members a hover (or member completion) should report.
// Mirrors the historic `resolveMemberAccess` base block so hover's soft
// fallback is preserved exactly:
//   - None: no variable or `with` target anchors the chain (not a member
//     access at all).
//   - NoOwner: the base variable resolved, but its declared type is unknown —
//     the access is still a member access (the caller sets `memberAccess`).
//   - Resolved: `owner` holds the owning Type/Union (or the Enum root for
//     qualified `EnumName.member` chains) and `first` is the index of the
//     first segment whose member type still needs a walk. `baseName` and
//     `enumMember` are filled for display.
ChainBase chainOwner(AnalyzedDoc const &doc, std::string const &normalizedPath,
                     WorkspaceIndex const *index, std::uint32_t off,
                     std::vector<Token> const &tokens,
                     std::vector<ChainSeg> *segs, bool implicit,
                     size_t keywordTokIdx, CrossDecl *owner, size_t *first,
                     std::string *baseName, bool *enumMember) {
  auto const resolveVar = [&](std::uint32_t voff) -> CrossDecl {
    if (index != nullptr) {
      return resolveAcross(doc, normalizedPath, voff, *index);
    }
    if (Symbol const *const d = resolveAt(doc, voff)) {
      return CrossDecl{nullptr, d};
    }
    return {};
  };

  if (implicit) {
    if (keywordTokIdx != std::string::npos) {
      CrossDecl const kw = resolveVar(tokens[keywordTokIdx].beg);
      if (kw.decl != nullptr && kw.decl->kind == SymbolKind::Enum) {
        // Splice the keyword back in as the leftmost segment; the enum's own
        // members follow the dot.
        segs->insert(segs->begin(),
                     {toLowerChars(kw.decl->name), keywordTokIdx});
        *baseName = std::string(kw.decl->name);
        *enumMember = true;
        *owner = kw;
        *first = 1;
        return ChainBase::Resolved;
      }
    }
    Token const *const target = withTargetOf(doc.parse, tokens, off);
    if (target == nullptr) {
      return ChainBase::None;
    }
    CrossDecl const base = resolveVar(target->beg);
    if (base.decl == nullptr) {
      return ChainBase::None;
    }
    *baseName = std::string(base.decl->name);
    std::string const tn = declaredTypeName(*base.decl);
    if (tn.empty()) {
      return ChainBase::NoOwner;
    }
    *owner = findTypeDecl(doc, normalizedPath, toLowerChars(tn), index);
    if (owner->decl == nullptr) {
      return ChainBase::NoOwner;
    }
    // The `with` target is not a chain segment: its member chain starts at
    // segment 0 (`.sectors(i).floorHeight` → segs[0] == sectors).
    *first = 0;
    return ChainBase::Resolved;
  }

  ChainSeg const &root = segs->front();
  CrossDecl const base = resolveVar(tokens[root.tokIdx].beg);
  if (base.decl == nullptr) {
    return ChainBase::None;
  }
  *baseName = std::string(base.decl->name);
  if (base.decl->kind == SymbolKind::Enum) {
    // `EnumName.member`: the enum itself is the owner type and its Const
    // children are the members. Valid for plain and `Explicit` enums alike
    // (KeyPgEnum, FreeBASIC.md §8).
    *owner = base;
    *enumMember = true;
    *first = 1;
    return ChainBase::Resolved;
  }
  std::string const tn = declaredTypeName(*base.decl);
  if (tn.empty()) {
    return ChainBase::NoOwner;
  }
  *owner = findTypeDecl(doc, normalizedPath, toLowerChars(tn), index);
  if (owner->decl == nullptr) {
    return ChainBase::NoOwner;
  }
  *first = 1;
  return ChainBase::Resolved;
}

// Walks intermediate chain segments [first, segs.size()) so a chained access
// (`w.wallColor.a`) resolves through each declared type. Stops one short of
// the rightmost segment in hover mode (the hovered member is the target); in
// completion mode the rightmost segment is the virtual member, so the walk
// covers every real segment. Returns false when any link's declared type is
// unknown — the caller soft-falls back with `memberAccess` set.
bool walkIntermediateMembers(AnalyzedDoc const &doc,
                             std::string const &normalizedPath,
                             WorkspaceIndex const *index,
                             std::vector<ChainSeg> const &segs, size_t first,
                             CrossDecl *typeDecl) {
  for (size_t k = first; k + 1 < segs.size(); ++k) {
    Symbol const *const m = findMember(*typeDecl->decl, segs[k].name);
    if (m == nullptr) {
      return false;
    }
    std::string const tn = declaredTypeName(*m);
    if (tn.empty()) {
      return false;
    }
    *typeDecl = findTypeDecl(doc, normalizedPath, toLowerChars(tn), index);
    if (typeDecl->decl == nullptr) {
      return false;
    }
  }
  return true;
}

// Whether `off` sits inside a member procedure implementation of the type
// whose key is `ownerKey` — a file root registered as `sub Type.name(...)`
// (root key = the type name, `<type>.` in its header signature). fbc
// (FreeBASIC.md §4, KeyPgVisPrivate/Protected): only from inside the type's
// own member procedures are private/protected members reachable; every
// outside path (`.`/`->`/`with` at module level or in a foreign procedure) is
// error 202. The parser models no inheritance, so `Extends`-derived access is
// deliberately not granted (FreeBASIC.md §12).
bool completionInsideOwnerType(ParseResult const &parse, std::uint32_t off,
                               std::string const &ownerKey) {
  Symbol const *const site = innermostScope(parse, off);
  for (Symbol const *cur = site; cur != nullptr; cur = parentOf(parse, cur)) {
    switch (cur->kind) {
    case SymbolKind::Sub:
    case SymbolKind::Function:
    case SymbolKind::Property:
    case SymbolKind::Constructor:
    case SymbolKind::Destructor:
    case SymbolKind::Operator:
      if (cur->key != ownerKey) {
        return false;
      }
      return toLowerChars(std::string(cur->signature)).find(ownerKey + ".") !=
             std::string::npos;
    default:
      break;
    }
  }
  return false;
}

} // namespace

MemberAccess resolveMemberAccess(AnalyzedDoc const &doc,
                                 std::string const &normalizedPath,
                                 std::uint32_t off,
                                 WorkspaceIndex const *index) {
  MemberAccess out;
  std::vector<Token> const &tokens = doc.tokens;

  auto const hover = tokenAndIndex(tokens, off);
  if (hover.first == nullptr || hover.second == 0) {
    return out;
  }
  Token const &op = tokens[hover.second - 1];
  if (op.kind != TokenKind::Symbol || (op.text() != "." && op.text() != "->")) {
    return out;
  }

  std::vector<ChainSeg> segs;
  bool implicit = false;
  size_t keywordTokIdx = std::string::npos;
  collectMemberChain(tokens, hover.second, /*virtualMember=*/false, &segs,
                     &implicit, &keywordTokIdx);
  if (segs.empty()) {
    return out;
  }

  CrossDecl typeDecl;
  size_t first = 0;
  switch (chainOwner(doc, normalizedPath, index, off, tokens, &segs, implicit,
                     keywordTokIdx, &typeDecl, &first, &out.baseName,
                     &out.enumMember)) {
  case ChainBase::None:
    return out;
  case ChainBase::NoOwner:
    out.memberAccess = true;
    return out;
  case ChainBase::Resolved:
    out.memberAccess = true;
    break;
  }
  if (!walkIntermediateMembers(doc, normalizedPath, index, segs, first,
                               &typeDecl)) {
    return out;
  }
  out.ownerTypeName = std::string(typeDecl.decl->name);
  out.member = findMember(*typeDecl.decl, segs.back().name);
  if (out.member == nullptr) {
    return out;
  }
  out.direct = (first + 1 == segs.size());
  return out;
}

MemberCompletion resolveMemberCompletion(AnalyzedDoc const &doc,
                                         std::string const &normalizedPath,
                                         std::uint32_t off,
                                         WorkspaceIndex const *index) {
  MemberCompletion mc;
  std::vector<Token> const &tokens = doc.tokens;

  auto const hover = tokenAndIndex(tokens, off);
  bool virtualMember = false;
  size_t memberTokIdx = 0;
  if (hover.first != nullptr) {
    if (hover.second == 0) {
      return mc;
    }
    Token const &op = tokens[hover.second - 1];
    if (op.kind != TokenKind::Symbol ||
        (op.text() != "." && op.text() != "->")) {
      return mc; // not a member access: the ordinary completion path
    }
    memberTokIdx = hover.second;
  } else {
    // Cursor not on a name token: the operator itself may be the last token
    // typed (`position.`, `map->`). Find it; only then is this a member
    // access. The token ending at/before `off` is the last one before the
    // cursor (no name token straddles it, or tokenAndIndex would have hit).
    // Newlines / EOF / comments in between are whitespace, not program
    // tokens, so a trailing line break or comment never masks an operator.
    virtualMember = true;
    memberTokIdx = std::string::npos;
    for (size_t ti = tokens.size(); ti > 0; --ti) {
      Token const &t = tokens[ti - 1];
      if (t.end > off) {
        continue; // token starts after the cursor
      }
      if (t.kind == TokenKind::Newline || t.kind == TokenKind::Eof ||
          t.kind == TokenKind::Comment || t.kind == TokenKind::DocComment) {
        continue; // not a program token
      }
      if (t.kind == TokenKind::Symbol &&
          (t.text() == "." || t.text() == "->")) {
        memberTokIdx = ti - 1;
      }
      break;
    }
    if (memberTokIdx == std::string::npos) {
      return mc;
    }
  }

  std::vector<ChainSeg> segs;
  bool implicit = false;
  size_t keywordTokIdx = std::string::npos;
  collectMemberChain(tokens, memberTokIdx, virtualMember, &segs, &implicit,
                     &keywordTokIdx);
  if (segs.empty()) {
    return mc;
  }

  CrossDecl typeDecl;
  size_t first = 0;
  switch (chainOwner(doc, normalizedPath, index, off, tokens, &segs, implicit,
                     keywordTokIdx, &typeDecl, &first, &mc.baseName,
                     &mc.enumMember)) {
  case ChainBase::None:
    return mc;
  case ChainBase::NoOwner:
    mc.memberAccess = true;
    return mc;
  case ChainBase::Resolved:
    mc.memberAccess = true;
    break;
  }
  if (!walkIntermediateMembers(doc, normalizedPath, index, segs, first,
                               &typeDecl)) {
    return mc;
  }
  mc.owner = typeDecl;
  mc.ownerTypeName = std::string(typeDecl.decl->name);

  // Access filter (FreeBASIC.md §4, fbc 1.10.2): public members always;
  // private/protected additionally inside the owner type's own member
  // procedures. The owner of a qualified enum chain is an Enum root whose
  // Const children are always public, so only Type members ever get gated.
  bool const insideOwner =
      (typeDecl.decl->kind == SymbolKind::Type ||
       typeDecl.decl->kind == SymbolKind::Union) &&
      completionInsideOwnerType(doc.parse, off, typeDecl.decl->key);
  for (Symbol const &m : typeDecl.decl->children) {
    if (m.key.empty()) {
      continue; // scope/unused markers never complete as members
    }
    if (m.access != Access::Public && !insideOwner) {
      continue;
    }
    mc.members.push_back(&m);
  }
  return mc;
}

} // namespace fblang
