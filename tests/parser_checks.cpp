/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Parser checks: symbol tree, block matching, diagnostics. Byte-offset.

#include <cstdio>
#include <string>

#include "language.h"
#include "parser.h"
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

#define CHECK_MSG(cond, msg)                                                   \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, msg);    \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static int diagnosticCount(const ParseResult &r, const char *code) {
  int n = 0;
  for (const auto &d : r.diagnostics) {
    if (d.code == code) {
      ++n;
    }
  }
  return n;
}

// The fbc identity carried by the first diagnostic with this routing code, or
// `None` when the parse reported none (or reported one fbc has no identity
// for).
static FbcDiagId fbcErrorFor(const ParseResult &r, const char *code) {
  for (const auto &d : r.diagnostics) {
    if (d.code == code) {
      return d.fbcId;
    }
  }
  return FbcDiagId::None;
}

static const Symbol *find(const std::vector<Symbol> &scope,
                          const std::string &key, SymbolKind k) {
  for (const auto &s : scope) {
    if (s.key == key && s.kind == k) {
      return &s;
    }
  }
  return nullptr;
}

// Does the parse declare a member with this name? Any kind: a member fbc
// creates is a member, and the question here is whether it exists at all, not
// which of the body's spellings produced it.
static bool declares(ParseResult const &r, const std::string &key) {
  for (const auto &s : r.roots) {
    for (const auto &c : s.children) {
      if (c.key == key) {
        return true;
      }
    }
  }
  return false;
}

// An unnamed declaration block still needs a name the outline can show and a
// selection that lies inside its own range: LSP requires selectionRange ⊆
// range, and a client binds the outline click to it. `key` stays empty — that
// is this parser's unnamed-declaration signal for dedupe, the index, and
// resolution — so only `name` carries `<anonymous ...>`.
static void checkShapeInvariants(const std::vector<Symbol> &scope) {
  for (const Symbol &s : scope) {
    CHECK(!s.name.empty());
    CHECK(s.selection.beg >= s.range.beg && s.selection.end <= s.range.end &&
          s.selection.beg <= s.selection.end);
    checkShapeInvariants(s.children);
  }
}

static void TestKeywordSuffixWarning() {
  // A suffix directly attached to a reserved word is ignored (fbc warning 44)
  // and the token stays the bare keyword: the block form with a suffixed
  // opener and closer still parses as a block, with one warning per suffix and
  // no structure error.
  ParseResult const block = parseDocument("if% 1 then\nend% if\n");
  CHECK(diagnosticCount(block, "keyword-suffix") == 2);
  CHECK(diagnosticCount(block, "stray-closer") == 0);
  CHECK(diagnosticCount(block, "unterminated-block") == 0);
  // The warning sits on the suffix char alone, which is exactly what the
  // `keyword-suffix` quick fix deletes, and the message names the word the
  // way fbc prints it.
  if (block.diagnostics.size() == 2) {
    CHECK(block.diagnostics.front().range.beg == 2);
    CHECK(block.diagnostics.front().range.end == 3);
    CHECK(block.diagnostics.front().message == "Suffix ignored in 'if%'");
  }

  // A suffix on a variable is ignored for *identity* in `fb` (`x%` and `x`
  // are the same variable, canonicalKey), but it is still not a reserved
  // word, so no keyword warning.
  ParseResult const ident = parseDocument("dim x% as integer\nx% = 1\n");
  CHECK(diagnosticCount(ident, "keyword-suffix") == 0);

  // `#` is never a keyword suffix: PRINT#1 is a file channel, and an
  // identifier's `#` folds in, so neither warns.
  ParseResult const hash = parseDocument("print#1,\ndone# = 1\n");
  CHECK(diagnosticCount(hash, "keyword-suffix") == 0);

  // A skipped `#if` body is never parsed code, so its suffixed words are
  // inert and do not warn.
  ParseResult const pp = parseDocument("#if 0\nprint%\n#endif\n");
  CHECK(diagnosticCount(pp, "keyword-suffix") == 0);
}

// In `fb`, fbc ignores a trailing type-suffix char on an identifier (warning
// 44) and aliases `foo`/`foo$` to one symbol, so the canonical key drops it
// (canonicalKey, language.h). The qb/fblite/deprecated dialects treat the
// suffix as significant, so their keys keep it. This is a parser-level check
// of the key; resolution identity is pinned in resolve_checks.
static void TestIdentifierSuffixIdentity() {
  // A variable: the key is suffix-free while the name keeps the spelling, and
  // the suffixed lookup key no longer exists.
  {
    ParseResult const r = parseDocument("dim foo$ as string\n"
                                        "foo = \"x\"\n");
    const Symbol *d = find(r.roots, "foo", SymbolKind::Dim);
    CHECK(d != nullptr && d->name == "foo$");
    CHECK(find(r.roots, "foo$", SymbolKind::Dim) == nullptr);
  }
  // A record field and a parameter share the rule.
  {
    ParseResult const r = parseDocument("type rec\n"
                                        "  field% as integer\n"
                                        "end type\n"
                                        "sub emit(s$ as string)\n"
                                        "end sub\n");
    const Symbol *rec = find(r.roots, "rec", SymbolKind::Type);
    CHECK(rec != nullptr);
    CHECK(find(rec->children, "field", SymbolKind::Variable) != nullptr);
    CHECK(find(rec->children, "field%", SymbolKind::Variable) == nullptr);
    const Symbol *emit = find(r.roots, "emit", SymbolKind::Sub);
    CHECK(emit != nullptr && emit->children.size() == 1);
    CHECK(emit != nullptr && emit->children[0].key == "s");
  }
  // A non-`fb` dialect keeps the suffix in the key: `foo` and `foo$` are two
  // symbols there.
  {
    ParseResult const r = parseDocument("#lang \"qb\"\ndim foo$ as string\n");
    CHECK(r.lang == "qb");
    CHECK(find(r.roots, "foo$", SymbolKind::Dim) != nullptr);
    CHECK(find(r.roots, "foo", SymbolKind::Dim) == nullptr);
  }
}

static int errorDiagnosticCount(const ParseResult &r) {
  int n = 0;
  for (const auto &d : r.diagnostics) {
    if (d.severity == Severity::Error) {
      ++n;
    }
  }
  return n;
}

// Step 2b false-positive clearing, verified against fbc 1.10.2. Every shape
// here previously produced an error-level diagnostic on code fbc accepts;
// v8 is the control proving the dedupe still catches a real duplicate.
static void TestStep2bParsingFixes() {
  // Bare `endif` closes an `if` block.
  {
    ParseResult const r = parseDocument("if 1 then\n"
                                        "  print 1\n"
                                        "endif\n");
    CHECK(r.diagnostics.empty());
  }
  // The one-line `if` with `else`/`end if` all on one line; a suffix on the
  // opener is warning 44, not a structure error.
  {
    ParseResult const r = parseDocument("if 1 then print else print end if\n");
    CHECK(r.diagnostics.empty());
  }
  {
    ParseResult const r = parseDocument("if% 1 then print else print end if\n");
    CHECK(errorDiagnosticCount(r) == 0);
    CHECK(diagnosticCount(r, "keyword-suffix") == 1);
  }
  // A member line with the object's own type as a `byref` parameter, and the
  // `virtual` / `const` Declare modifiers (the class body that follows).
  {
    ParseResult const r = parseDocument("type T\n"
                                        "  x as integer\n"
                                        "  declare sub s(byref v as T)\n"
                                        "  declare virtual sub sv()\n"
                                        "  declare const sub sc()\n"
                                        "end type\n");
    CHECK(r.diagnostics.empty());
  }
  // The qualified module-scope `dim` of an object member (`dim T.x`) is a
  // declaration fbc accepts, not a second definition of T.
  {
    ParseResult const r = parseDocument("type T\n"
                                        "  x as integer\n"
                                        "end type\n"
                                        "dim T.x as integer\n");
    CHECK(r.diagnostics.empty());
  }
  // A module-scope `extern` and a `dim` of a different name are both plain
  // declarations.
  {
    ParseResult const r = parseDocument("extern x as integer\n"
                                        "dim y as integer\n");
    CHECK(r.diagnostics.empty());
  }
  // Only one branch of `#if a / #else` is active: the same name in each is
  // not a duplicate. (The inactive copy is not parsed code at all.)
  {
    ParseResult const r = parseDocument("#if a\n"
                                        "  dim x as integer\n"
                                        "#else\n"
                                        "  dim x as integer\n"
                                        "#endif\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 0);
  }
  // A record member's bit width is a numeric literal after the `:` — the
  // colon ends the member, not the record body.
  {
    ParseResult const r = parseDocument("type W\n"
                                        "  a : 7 as ulong\n"
                                        "  b : 3 as ulong\n"
                                        "end type\n");
    CHECK(r.diagnostics.empty());
  }
  // The control: what fbc *does* reject — the same name twice at one scope —
  // still warns, so the dedupe above is not a silence-all.
  {
    ParseResult const r = parseDocument("dim a as integer\n"
                                        "dim a as integer\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 1);
  }
  // Names that fbc allows to recur: an `extern` matches its later `dim`, a
  // `type <name> as <type>` alias may be redeclared, a `const` may be
  // redefined, and re-opening a `namespace` is not a duplicate.
  {
    ParseResult const r = parseDocument("extern z as integer\n"
                                        "dim z as integer\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 0);
  }
  // The converse order fbc rejects (probed; FreeBASIC.md §8): an `Extern` may
  // not follow a definition of the same name — `extern x` declares a name for
  // another module's definition, it is not a re-declaration of one. Both a
  // plain `Dim` and a `Dim Shared` trigger `error 4`, while `extern` repeated
  // stays legal (it is declaration-only).
  {
    ParseResult const r = parseDocument("dim z as integer\n"
                                        "extern z as integer\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 1);
  }
  {
    ParseResult const r = parseDocument("dim shared z as integer\n"
                                        "extern z as integer\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 1);
  }
  {
    ParseResult const r = parseDocument("extern z as integer\n"
                                        "extern z as integer\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 0);
  }
  // `Common` is a definition like `Dim` (fbc `error 4`, probed): repeated,
  // and in collision with a `Dim` or a later `Extern`. Only `Redim` may repeat
  // itself (`redim x(3)` + `redim x(4)` compiles), because it re-dimensions.
  {
    ParseResult const r = parseDocument("common c as integer\n"
                                        "common c as integer\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 1);
  }
  {
    ParseResult const r = parseDocument("common c as integer\n"
                                        "extern c as integer\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 1);
  }
  {
    ParseResult const r = parseDocument("redim q(3) as integer\n"
                                        "redim q(4) as integer\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 0);
  }
  {
    ParseResult const r = parseDocument("type mybyte as byte\n"
                                        "type mybyte as byte\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 0);
  }
  {
    ParseResult const r = parseDocument("const c = 1\n"
                                        "const c = 1\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 0);
  }
  {
    ParseResult const r = parseDocument("namespace n\n"
                                        "end namespace\n"
                                        "namespace n\n"
                                        "end namespace\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 0);
    CHECK(find(r.roots, "n", SymbolKind::Namespace) != nullptr);
  }
  // A suffixed constructor *call* inside a constructor body is not a second
  // definition — `constructor%()` calls, `property% =` assigns the result.
  // fbc compiles this whole namespace; warning 44 is the accurate residue.
  {
    ParseResult const r = parseDocument("namespace parser_proccall\n"
                                        "  type Q\n"
                                        "    n as integer\n"
                                        "    declare constructor()\n"
                                        "  end type\n"
                                        "  type T extends Q\n"
                                        "    x as Q\n"
                                        "    declare constructor()\n"
                                        "    declare property P() as integer\n"
                                        "    declare function F() as integer\n"
                                        "  end type\n"
                                        "  constructor Q()\n"
                                        "    constructor%()\n"
                                        "  end constructor\n"
                                        "  constructor T()\n"
                                        "    base%()\n"
                                        "    x.constructor%()\n"
                                        "    base%.constructor%()\n"
                                        "  end constructor\n"
                                        "  property T.P() as integer\n"
                                        "    property% = 1\n"
                                        "  end property\n"
                                        "  function T.F() as integer\n"
                                        "    function% = 1\n"
                                        "  end function\n"
                                        "  operator not( byref x as T ) as T\n"
                                        "    operator% = x\n"
                                        "  end operator\n"
                                        "end namespace\n");
    CHECK(errorDiagnosticCount(r) == 0);
    CHECK(diagnosticCount(r, "keyword-suffix") == 8);
  }
  // A macro invocation in a record body is not a field member: fbc expands
  // the macro away before parsing (`#macro m` + `m(1)` in the body), so the
  // line must not be captured or rejected.
  {
    ParseResult const r = parseDocument("#macro m(x)\n"
                                        "  dim as integer x\n"
                                        "#endmacro\n"
                                        "type T\n"
                                        "  m(1)\n"
                                        "  as integer y\n"
                                        "end type\n");
    CHECK(r.diagnostics.empty());
  }
}

static void TestQualifiedTypeFirstVarDecls() {
  // A type-first declaration whose type is a dot-qualified name: the whole
  // path is the type, and the variable is the identifier after it. The
  // skipper used to stop at the dot and register the type's leaf as the
  // declared name, so the second declaration reported a false duplicate.
  std::string const repro = "namespace gfx\n"
                            "    type sprite\n"
                            "        w as integer\n"
                            "    end type\n"
                            "end namespace\n"
                            "\n"
                            "dim as gfx.sprite ptr firstImage\n"
                            "dim as gfx.sprite ptr secondImage\n";
  ParseResult const r = parseDocument(repro);
  CHECK(r.diagnostics.empty());
  CHECK(diagnosticCount(r, "duplicate-definition") == 0);
  Symbol const *first = find(r.roots, "firstimage", SymbolKind::Dim);
  Symbol const *second = find(r.roots, "secondimage", SymbolKind::Dim);
  CHECK(first != nullptr);
  CHECK(second != nullptr);
  CHECK(find(r.roots, "sprite", SymbolKind::Dim) == nullptr);
  if (first != nullptr) {
    CHECK(first->name == "firstImage");
    CHECK(first->selection.beg == repro.find("firstImage"));
    CHECK(first->selection.end == repro.find("firstImage") + 10);
  }
  if (second != nullptr) {
    CHECK(second->name == "secondImage");
    CHECK(second->selection.beg == repro.find("secondImage"));
    CHECK(second->selection.end == repro.find("secondImage") + 11);
  }

  // The same leaf name from two namespaces: both declarations name their own
  // variable, and neither type leaf becomes a variable.
  {
    ParseResult const two = parseDocument("namespace a\n"
                                          "    type sprite\n"
                                          "        x as integer\n"
                                          "    end type\n"
                                          "end namespace\n"
                                          "namespace b\n"
                                          "    type sprite\n"
                                          "        y as integer\n"
                                          "    end type\n"
                                          "end namespace\n"
                                          "dim as a.sprite ptr lhs\n"
                                          "dim as b.sprite ptr rhs\n");
    CHECK(two.diagnostics.empty());
    CHECK(find(two.roots, "lhs", SymbolKind::Dim) != nullptr);
    CHECK(find(two.roots, "rhs", SymbolKind::Dim) != nullptr);
    CHECK(find(two.roots, "sprite", SymbolKind::Dim) == nullptr);
  }

  // `const` may sit before the qualified type in the type-first form.
  {
    ParseResult const q = parseDocument("namespace gfx\n"
                                        "    type sprite\n"
                                        "        w as integer\n"
                                        "    end type\n"
                                        "end namespace\n"
                                        "dim as const gfx.sprite ptr image\n");
    CHECK(q.diagnostics.empty());
    CHECK(find(q.roots, "image", SymbolKind::Dim) != nullptr);
    CHECK(find(q.roots, "sprite", SymbolKind::Dim) == nullptr);
  }

  // The same type-first form as a record field: the field is the name after
  // the qualified type, not the type's leaf.
  {
    ParseResult const field = parseDocument("namespace gfx\n"
                                            "    type sprite\n"
                                            "        w as integer\n"
                                            "    end type\n"
                                            "end namespace\n"
                                            "type holder\n"
                                            "    as gfx.sprite ptr image\n"
                                            "end type\n");
    CHECK(field.diagnostics.empty());
    Symbol const *holder = find(field.roots, "holder", SymbolKind::Type);
    CHECK(holder != nullptr);
    CHECK(holder != nullptr &&
          find(holder->children, "image", SymbolKind::Variable) != nullptr);
    CHECK(holder != nullptr &&
          find(holder->children, "sprite", SymbolKind::Variable) == nullptr);
  }

  // And a field line whose qualified type follows `const`.
  {
    ParseResult const field =
        parseDocument("namespace gfx\n"
                      "    type sprite\n"
                      "        w as integer\n"
                      "    end type\n"
                      "end namespace\n"
                      "type holder\n"
                      "    as const gfx.sprite ptr image\n"
                      "end type\n");
    CHECK(field.diagnostics.empty());
    Symbol const *holder = find(field.roots, "holder", SymbolKind::Type);
    CHECK(holder != nullptr);
    CHECK(holder != nullptr &&
          find(holder->children, "image", SymbolKind::Variable) != nullptr);
    CHECK(holder != nullptr &&
          find(holder->children, "sprite", SymbolKind::Variable) == nullptr &&
          find(holder->children, "gfx", SymbolKind::Variable) == nullptr);
  }

  // The documented chain cases are untouched: a type-first chain still ends
  // at `ptr`, `as const t p` still names `p`, and a dangling `const` still
  // reports on the word.
  {
    ParseResult const chain = parseDocument("type t\n"
                                            "  x as integer\n"
                                            "end type\n"
                                            "type holder\n"
                                            "  as const t p\n"
                                            "end type\n"
                                            "dim as integer ptr a, b\n");
    CHECK(chain.diagnostics.empty());
    CHECK(declares(chain, "p"));
    CHECK(find(chain.roots, "a", SymbolKind::Dim) != nullptr);
    CHECK(find(chain.roots, "b", SymbolKind::Dim) != nullptr);
    CHECK(!declares(chain, "ptr"));
  }
  {
    ParseResult const dangling = parseDocument("type t\n"
                                               "  as integer const\n"
                                               "end type\n");
    CHECK(diagnosticCount(dangling, "invalid-member-name") == 1);
    CHECK(!declares(dangling, "const"));
  }
}

static void TestAnonymousDeclarationsNameAndSelect() {
  // The ProPgTypeUnion nesting: a named type, an anonymous union, and the
  // anonymous type inside that union (fbc compiles exactly this spelling).
  std::string const nested = "enum\n"
                             "  red = 1\n"
                             "end enum\n"
                             "type T\n"
                             "  union\n"
                             "    type\n"
                             "      dim b1 as byte\n"
                             "    end type\n"
                             "  end union\n"
                             "end type\n";
  ParseResult const r = parseDocument(nested);
  CHECK(r.diagnostics.empty());
  checkShapeInvariants(r.roots);

  Symbol const *en = find(r.roots, "", SymbolKind::Enum);
  CHECK_MSG(en != nullptr && en->name == "<anonymous enum>" && en->key.empty(),
            "a bare `enum` is named for the outline but stays unnamed to the "
            "parser");
  CHECK_MSG(en != nullptr && en->selection.beg == nested.find("enum") &&
                en->selection.end == nested.find("enum") + 4,
            "its selection is the `enum` keyword itself");

  Symbol const *t = find(r.roots, "t", SymbolKind::Type);
  Symbol const *anonUnion =
      t != nullptr ? find(t->children, "", SymbolKind::Union) : nullptr;
  Symbol const *anonType = anonUnion != nullptr
                               ? find(anonUnion->children, "", SymbolKind::Type)
                               : nullptr;
  CHECK_MSG(anonUnion != nullptr && anonUnion->name == "<anonymous union>",
            "an unnamed `union` gets a name of its own");
  CHECK_MSG(anonType != nullptr && anonType->name == "<anonymous type>",
            "the unnamed record body inside it gets one too");
  CHECK_MSG(anonType != nullptr &&
                find(anonType->children, "b1", SymbolKind::Dim) != nullptr,
            "its field nests under it instead of floating up");
  CHECK_MSG(t == nullptr || find(t->children, "b1", SymbolKind::Dim) == nullptr,
            "the field did not leak into the enclosing type");

  // Two anonymous enums in one file are two declarations, not a duplicate of
  // each other (the `''`-key collision used to warn once per anonymous enum).
  ParseResult const two = parseDocument("enum\n  a = 1\nend enum\n"
                                        "enum\n  b = 2\nend enum\n");
  CHECK(two.diagnostics.empty());
  CHECK(diagnosticCount(two, "duplicate-definition") == 0);

  // An anonymous block inherits the access section in force where it is
  // written: fbc gates the fields of a nested anonymous union by the
  // enclosing `Private:` (probed: error 202), and an access section *inside*
  // an anonymous union is rejected (probed: error 17).
  ParseResult const gated = parseDocument("type t\n"
                                          "  private:\n"
                                          "    union\n"
                                          "      dim a as long\n"
                                          "    end union\n"
                                          "  public:\n"
                                          "    dim b as long\n"
                                          "end type\n");
  CHECK(gated.diagnostics.empty());
  Symbol const *gt = find(gated.roots, "t", SymbolKind::Type);
  Symbol const *gu =
      gt != nullptr ? find(gt->children, "", SymbolKind::Union) : nullptr;
  Symbol const *ga =
      gu != nullptr ? find(gu->children, "a", SymbolKind::Dim) : nullptr;
  CHECK_MSG(ga != nullptr && ga->access == Access::Private,
            "the anonymous union's field keeps the section it was written in");
}

// Preprocessor: `#define`/`#macro` publish as Define symbols, conditional
// branches parse with the live arm only, and the positional window
// (redefinition / `#undef`) truncates a define's reachable region.
static void TestPreprocessorSymbolsAndBranches() {
  // A reachable `#define` publishes as a Define root carrying everything hover
  // and completion need: original-case name, lowercase key, the directive's
  // first line as the signature, the selection on the name token, and an open
  // (UINT32_MAX) positional window.
  {
    ParseResult const r = parseDocument("#define MAX_CACHED_TEXTURES 19\n"
                                        "print MAX_CACHED_TEXTURES\n");
    CHECK(r.diagnostics.empty());
    const Symbol *d = find(r.roots, "max_cached_textures", SymbolKind::Define);
    CHECK(d != nullptr);
    if (d != nullptr) {
      CHECK(d->name == "MAX_CACHED_TEXTURES");
      CHECK(d->key == "max_cached_textures");
      CHECK(d->signature == "#define MAX_CACHED_TEXTURES 19");
      CHECK(!d->isMacro);
      CHECK(d->defineEnd == UINT32_MAX); // open until #undef/redefinition
      CHECK(d->selection.beg == 8);      // `#define ` is eight bytes
      CHECK(d->selection.end == 8 + 19);
    }
  }

  // A `''` doc comment above a define becomes its hover text, exactly as for
  // procedure declarations.
  {
    ParseResult const r = parseDocument("'' Max cached textures\n"
                                        "#define MAX_CACHED_TEXTURES 19\n");
    const Symbol *d = find(r.roots, "max_cached_textures", SymbolKind::Define);
    CHECK(d != nullptr && d->doc == " Max cached textures");
  }

  // A `#macro` publishes with isMacro and a signature holding only the opener
  // line — the parameters, never the body.
  {
    ParseResult const r = parseDocument("#macro say_it(w)\n"
                                        "  print w\n"
                                        "#endmacro\n"
                                        "say_it(\"hi\")\n");
    CHECK(r.diagnostics.empty());
    const Symbol *d = find(r.roots, "say_it", SymbolKind::Define);
    CHECK(d != nullptr);
    if (d != nullptr) {
      CHECK(d->isMacro);
      CHECK(d->signature == "#macro say_it(w)");
      CHECK(std::string(d->signature).find("print") == std::string::npos);
      CHECK(d->selection.beg == 7); // `#macro ` is seven bytes
      CHECK(d->selection.end == 7 + 6);
    }
  }

  // A decidable condition activates exactly the live arm's symbols: `#if 1`
  // parses the then-arm, `#if 0` parses the else-arm.
  {
    ParseResult const r = parseDocument("#if 1\n"
                                        "  dim x as integer\n"
                                        "#else\n"
                                        "  dim y as integer\n"
                                        "#endif\n");
    CHECK(r.diagnostics.empty());
    CHECK(find(r.roots, "x", SymbolKind::Dim) != nullptr);
    CHECK(find(r.roots, "y", SymbolKind::Dim) == nullptr);
  }
  {
    ParseResult const r = parseDocument("#if 0\n"
                                        "  dim x as integer\n"
                                        "#else\n"
                                        "  dim y as integer\n"
                                        "#endif\n");
    CHECK(r.diagnostics.empty());
    CHECK(find(r.roots, "x", SymbolKind::Dim) == nullptr);
    CHECK(find(r.roots, "y", SymbolKind::Dim) != nullptr);
  }

  // Inactive arms are skipped wholesale: their content neither declares
  // symbols nor reports diagnostics, however hostile it is (an unterminated
  // string, a collision that would duplicate, a suffixed reserved word).
  {
    ParseResult const r = parseDocument("#if 0\n"
                                        "  print \"unterminated\n"
                                        "  dim dim dim as\n"
                                        "  print%\n"
                                        "#else\n"
                                        "  dim y as integer\n"
                                        "#endif\n");
    CHECK(r.diagnostics.empty());
    CHECK(find(r.roots, "x", SymbolKind::Dim) == nullptr);
    CHECK(find(r.roots, "y", SymbolKind::Dim) != nullptr);
  }
  {
    ParseResult const r = parseDocument("#macro opaque\n"
                                        "  if if if\n"
                                        "  dim dim dim as\n"
                                        "#endmacro\n"
                                        "print 1\n");
    CHECK(r.diagnostics.empty());
  }

  // An undecidable condition skips the whole chain — neither arm is
  // reachable, by the documented divergence (fbc folds unknowns to 0 and runs
  // the else-arm; the user's rule is not-reachable for the whole chain).
  {
    ParseResult const r = parseDocument("#if NO_SUCH_THING\n"
                                        "  dim x as integer\n"
                                        "#else\n"
                                        "  dim y as integer\n"
                                        "#endif\n"
                                        "print 1\n");
    CHECK(r.diagnostics.empty());
    CHECK(find(r.roots, "x", SymbolKind::Dim) == nullptr);
    CHECK(find(r.roots, "y", SymbolKind::Dim) == nullptr);
  }
  // `#ifdef`/`#ifndef` ask a question the define table always answers — an
  // unknown name is definably NOT defined, so the else-arm runs (exactly what
  // fbc does). Undecidable is a `#if <expr>` property: an unknown identifier
  // inside an expression has no value to fold.
  {
    ParseResult const r = parseDocument("#ifdef NO_SUCH_THING\n"
                                        "  dim x as integer\n"
                                        "#else\n"
                                        "  dim y as integer\n"
                                        "#endif\n");
    CHECK(find(r.roots, "x", SymbolKind::Dim) == nullptr);
    CHECK(find(r.roots, "y", SymbolKind::Dim) != nullptr);
  }

  // A built-in define is decidable: `#ifdef` against a seeded platform flag
  // picks the arm fbc picks.
  {
    ParseResult const r = parseDocument("#ifdef __FB_DEBUG__\n"
                                        "  dim x as integer\n"
                                        "#else\n"
                                        "  dim y as integer\n"
                                        "#endif\n");
    const Symbol *d = find(r.roots, "x", SymbolKind::Dim);
    CHECK_MSG(d != nullptr, "a seeded define decides the branch");
    CHECK(find(r.roots, "y", SymbolKind::Dim) == nullptr);
  }

  // The evaluator folds a defined value into `#if`: a decidable comparison
  // activates the true arm. (This is the regression pin for the nameEnd/value
  // offset mishap — a define whose value was read from a wrong offset folded
  // nothing and skipped the whole chain.)
  {
    ParseResult const r = parseDocument("#define V 5\n"
                                        "#if V = 5\n"
                                        "  dim q as integer\n"
                                        "#else\n"
                                        "  dim r as integer\n"
                                        "#endif\n");
    CHECK(r.diagnostics.empty());
    CHECK(find(r.roots, "q", SymbolKind::Dim) != nullptr);
    CHECK(find(r.roots, "r", SymbolKind::Dim) == nullptr);
  }

  // A continued directive is one Preprocessor token, so the define's range
  // spans both physical lines and its value folds across the join.
  {
    std::string const src = "#define WIDE 5 _\n  + 1\nprint 1\n";
    ParseResult const r = parseDocument(src);
    CHECK(r.diagnostics.empty());
    const Symbol *d = find(r.roots, "wide", SymbolKind::Define);
    CHECK(d != nullptr);
    if (d != nullptr) {
      CHECK(d->range.end ==
            static_cast<uint32_t>(src.find("+ 1") + 3)); // end of line two
    }
  }
  {
    ParseResult const r = parseDocument("#define WIDE 5 _\n"
                                        "  + 1\n"
                                        "#if WIDE = 6\n"
                                        "  dim w as integer\n"
                                        "#endif\n");
    CHECK(r.diagnostics.empty());
    CHECK(find(r.roots, "w", SymbolKind::Dim) != nullptr);
  }

  // `#undef` closes the define's window: defineEnd lands on the directive.
  // (`#define A 1\nprint A\n` is 20 bytes; the `#undef` starts there.)
  {
    ParseResult const r = parseDocument("#define A 1\nprint A\n#undef A\n");
    const Symbol *d = find(r.roots, "a", SymbolKind::Define);
    CHECK(d != nullptr && d->defineEnd == 20);
  }

  // A redefinition is a replacement: two Define symbols for one key, the
  // first one's window closed at the second's position, and no
  // duplicate-definition diagnostic — value equality is not folded, so the
  // dedupe stays silent (FreeBASIC.md §12).
  {
    ParseResult const r = parseDocument("#define A 1\n"
                                        "#define A 1\n"
                                        "print A\n");
    int count = 0;
    const Symbol *first = nullptr;
    const Symbol *second = nullptr;
    for (const Symbol &s : r.roots) {
      if (s.key == "a" && s.kind == SymbolKind::Define) {
        if (count == 0) {
          first = &s;
        } else {
          second = &s;
        }
        ++count;
      }
    }
    CHECK(count == 2);
    if (first != nullptr && second != nullptr) {
      CHECK(first->defineEnd == second->range.beg);
      CHECK(second->defineEnd == UINT32_MAX);
    }
    CHECK(diagnosticCount(r, "duplicate-definition") == 0);
  }

  // A suffix on a define name is part of its canonical key, like any other
  // identifier.
  {
    ParseResult const r = parseDocument("#define FOO$ \"x\"\n");
    const Symbol *d = find(r.roots, "foo$", SymbolKind::Define);
    CHECK(d != nullptr && d->name == "FOO$");
  }
}

// M21 step 1: module constructors and destructors. `Sub name() Constructor
// [priority]` is a procedure body that runs before its module's own code
// (FreeBASIC.md §9). These cases pin fbc 1.10.2's own numbers for the malformed
// forms — the argument count (1), the declaration-line decorator (3), the
// priority range (189), and the non-static member sub (17) — plus the shapes
// fbc accepts.
static void TestModuleConstructors() {
  // The accepted shapes. `Sub Init() Constructor` is an ordinary Sub carrying
  // the run-before-main timing; the marker and the optional priority are
  // consumed, not left to capture as members.
  {
    ParseResult r = parseDocument("sub Init() constructor\nend sub\n");
    CHECK(r.diagnostics.empty());
    const Symbol *s = find(r.roots, "init", SymbolKind::Sub);
    CHECK(s != nullptr && s->isModuleConstructor);
    CHECK(s != nullptr && !s->isModuleDestructor);
    CHECK(s != nullptr && s->constructorPriority == 0);
  }
  // No parens, and the optional priority: 101 and 65535 are the range ends.
  {
    ParseResult r = parseDocument("sub Init constructor 101\nend sub\n");
    CHECK(r.diagnostics.empty());
    const Symbol *s = find(r.roots, "init", SymbolKind::Sub);
    CHECK(s != nullptr && s->constructorPriority == 101);
  }
  {
    ParseResult r = parseDocument("sub Init() constructor 65535\nend sub\n");
    CHECK(r.diagnostics.empty());
    const Symbol *s = find(r.roots, "init", SymbolKind::Sub);
    CHECK(s != nullptr && s->constructorPriority == 65535);
  }
  // An integer literal is an integer whatever its spelling: hex `&h65` is 101,
  // and an integer suffix is accepted (both probed against fbc 1.10.2).
  {
    ParseResult r = parseDocument("sub Init() constructor &h65\nend sub\n");
    CHECK(r.diagnostics.empty());
    const Symbol *s = find(r.roots, "init", SymbolKind::Sub);
    CHECK(s != nullptr && s->constructorPriority == 101);
  }
  {
    ParseResult r = parseDocument("sub Init() constructor 101%\nend sub\n");
    CHECK(r.diagnostics.empty());
    const Symbol *s = find(r.roots, "init", SymbolKind::Sub);
    CHECK(s != nullptr && s->constructorPriority == 101);
  }
  // The destructor form; the body still closes with `end sub`.
  {
    ParseResult r = parseDocument("sub d() destructor\nend sub\n");
    CHECK(r.diagnostics.empty());
    const Symbol *s = find(r.roots, "d", SymbolKind::Sub);
    CHECK(s != nullptr && s->isModuleDestructor);
    CHECK(s != nullptr && !s->isModuleConstructor);
  }
  // `Static` and a trailing comment are the two tails fbc accepts; a `:` is the
  // ordinary statement separator (all probed).
  for (char const *src : {"sub f() constructor static\nend sub\n",
                          "sub f() constructor ' note\nend sub\n",
                          "sub f() constructor : print 1 : end sub\n"}) {
    ParseResult r = parseDocument(src);
    if (!r.diagnostics.empty()) {
      std::printf("FAIL ctor tail \"%s\": %zu diagnostic(s), first %s\n", src,
                  r.diagnostics.size(), r.diagnostics.front().code.c_str());
      ++failures;
    }
  }

  // A parameter list: fbc's `error 1: Argument count mismatch`, reported and
  // not followed by a second complaint about the (now absent) priority.
  {
    ParseResult r =
        parseDocument("sub Bad(x as integer) constructor\nend sub\n");
    CHECK(fbcErrorFor(r, "module-ctor-params") == FbcDiagId::ArgCntMismatch);
    CHECK(diagnosticCount(r, "module-ctor-params") == 1);
    CHECK(diagnosticCount(r, "module-ctor-priority") == 0);
    CHECK(diagnosticCount(r, "module-ctor-tail") == 0);
    // The same argument-count rule for a destructor.
    ParseResult d = parseDocument("sub d(x as integer) destructor\nend sub\n");
    CHECK(fbcErrorFor(d, "module-ctor-params") == FbcDiagId::ArgCntMismatch);
  }
  // Priority outside 101..65535, and a non-integer literal, are both
  // `error 189: Invalid priority attribute` (probed).
  for (char const *src : {"sub f() constructor 100\nend sub\n",
                          "sub f() constructor 65536\nend sub\n",
                          "sub f() constructor 101.0\nend sub\n",
                          "sub f() constructor 101!\nend sub\n"}) {
    ParseResult r = parseDocument(src);
    if (fbcErrorFor(r, "module-ctor-priority") != FbcDiagId::InvalidPriority) {
      std::printf("FAIL ctor priority \"%s\": fbc %d, want InvalidPriority\n",
                  src,
                  static_cast<int>(fbcErrorFor(r, "module-ctor-priority")));
      ++failures;
    }
  }
  // A tail fbc cannot parse is `error 3: Expected End-of-Line` — a word, a
  // sign, or a second number after the priority.
  for (char const *src : {"sub Init() constructor abc\nend sub\n",
                          "sub Init() constructor -5\nend sub\n",
                          "sub Init() constructor 101 102\nend sub\n"}) {
    ParseResult r = parseDocument(src);
    if (fbcErrorFor(r, "module-ctor-tail") != FbcDiagId::ExpectedEol) {
      std::printf("FAIL ctor tail \"%s\": fbc %d, want ExpectedEol\n", src,
                  static_cast<int>(fbcErrorFor(r, "module-ctor-tail")));
      ++failures;
    }
  }
  // `Export` after the marker is `error 17` (fbc's global-ctor check), not 3.
  {
    ParseResult r = parseDocument("sub f() constructor export\nend sub\n");
    CHECK(fbcErrorFor(r, "module-ctor-export") == FbcDiagId::SyntaxError);
  }

  // `Constructor`/`Destructor`/`Export` on a `Declare` line is `error 3` in
  // both the plain (probed `ctordecl.bas`) and UDT-member (`udtctor.bas`)
  // spellings; a legal `Declare` line stays silent, including the
  // `declare constructor()` prototype the parser does not model.
  {
    ParseResult r = parseDocument("declare sub Init() constructor\n");
    CHECK(fbcErrorFor(r, "decl-decorator") == FbcDiagId::ExpectedEol);
  }
  {
    ParseResult r = parseDocument("declare sub Init() destructor\n");
    CHECK(fbcErrorFor(r, "decl-decorator") == FbcDiagId::ExpectedEol);
  }
  {
    ParseResult r = parseDocument("declare sub f() export\n");
    CHECK(fbcErrorFor(r, "decl-decorator") == FbcDiagId::ExpectedEol);
  }
  {
    ParseResult r = parseDocument("type t\n"
                                  "  x as integer\n"
                                  "  declare constructor()\n"
                                  "  declare function f() as integer\n"
                                  "end type\n");
    CHECK(r.diagnostics.empty());
  }

  // A `Static` UDT member procedure may be a module constructor (probed
  // `udtctor3.bas` compiles); a non-static one is fbc's `error 17`. The check
  // only answers when the owner type and its prototype are both in this file.
  {
    ParseResult r = parseDocument("type t\n"
                                  "  x as integer\n"
                                  "  declare static sub Init()\n"
                                  "end type\n"
                                  "sub t.Init() constructor\n"
                                  "end sub\n");
    CHECK(r.diagnostics.empty());
    const Symbol *impl = find(r.roots, "init", SymbolKind::Sub);
    CHECK(impl != nullptr && impl->ownerKey == "t");
    CHECK(impl != nullptr && impl->isModuleConstructor);
  }
  {
    ParseResult r = parseDocument("type t\n"
                                  "  x as integer\n"
                                  "  declare sub Init()\n"
                                  "end type\n"
                                  "sub t.Init() constructor\n"
                                  "end sub\n");
    CHECK(fbcErrorFor(r, "module-ctor-nonstatic") == FbcDiagId::SyntaxError);
  }
  {
    ParseResult r = parseDocument("type t\n"
                                  "  x as integer\n"
                                  "  declare sub Init()\n"
                                  "end type\n"
                                  "sub t.Init() destructor\n"
                                  "end sub\n");
    CHECK(fbcErrorFor(r, "module-ctor-nonstatic") == FbcDiagId::SyntaxError);
  }
  // The owner type or its prototype in another file: the check stays silent
  // rather than guessing (a header cannot be seen from here).
  {
    ParseResult r = parseDocument("sub t.Init() constructor\nend sub\n");
    CHECK(diagnosticCount(r, "module-ctor-nonstatic") == 0);
  }

  // A `Function` tail is left unconsumed on purpose (FreeBASIC.md §12): fbc's
  // error for `Function f() Constructor` is ambiguous between 17 and 147, so
  // this parser reports nothing rather than picking one.
  {
    ParseResult r = parseDocument("function f() constructor\nend function\n");
    CHECK(diagnosticCount(r, "module-ctor-tail") == 0);
    CHECK(diagnosticCount(r, "module-ctor-params") == 0);
  }
}

// An `Asm ... End Asm` body is raw assembly, not a statement list
// (FreeBASIC.md §7): fbc hands the lines to the assembler, so a mnemonic
// that is also a FreeBASIC keyword — `sub eax, 1` — declares nothing and
// opens nothing. The body used to run through statement dispatch, which
// opened a phantom block (two false diagnostics plus an outline symbol),
// split `:`-compound lines into statements, and captured asm labels as
// FreeBASIC labels. Only the block's own closer is recognized.
static void TestAsmBlockRawBody() {
  // The reproduction: a keyword mnemonic inside a procedure, closed
  // normally. `demo` has no parameters, so any child of it is the phantom
  // `Sub` the `sub` instruction used to declare.
  {
    ParseResult const r = parseDocument("sub demo()\n"
                                        "    asm\n"
                                        "        mov eax, 1\n"
                                        "        sub eax, 1\n"
                                        "    end asm\n"
                                        "end sub\n");
    CHECK(r.diagnostics.empty());
    const Symbol *demo = find(r.roots, "demo", SymbolKind::Sub);
    CHECK(demo != nullptr);
    CHECK(demo->children.empty());
  }
  // Module level, keyword mnemonics and an asm label: nothing reports, no
  // phantom `Sub`, and the label declares no FreeBASIC symbol.
  {
    ParseResult const r = parseDocument("sub noop()\n"
                                        "end sub\n"
                                        "asm\n"
                                        "    push rax\n"
                                        "    call noop\n"
                                        "    sub rax, 1\n"
                                        "    jmp done\n"
                                        "    done:\n"
                                        "    pop rax\n"
                                        "end asm\n");
    CHECK(r.diagnostics.empty());
    CHECK(find(r.roots, "rax", SymbolKind::Sub) == nullptr);
    CHECK(find(r.roots, "done", SymbolKind::Label) == nullptr);
  }
  // A `:`-compound line stays raw as a whole: inside the block a colon is
  // label syntax, never a statement separator, so the head is not captured
  // as a FreeBASIC label and the tail is not dispatched as a statement.
  {
    ParseResult const r = parseDocument("asm\n"
                                        "    mov eax, 1\n"
                                        "    done: sub eax, 1\n"
                                        "    jmp done\n"
                                        "end asm\n");
    CHECK(r.diagnostics.empty());
    CHECK(find(r.roots, "done", SymbolKind::Label) == nullptr);
    CHECK(find(r.roots, "eax", SymbolKind::Sub) == nullptr);
  }
  // A conditional region inside the body keeps its own directive handling
  // and its block sits above the Asm block; the assembly in the live arm
  // is still raw, so the keyword mnemonic declares nothing there either.
  {
    ParseResult const r = parseDocument("asm\n"
                                        "    #ifdef __FB_DEBUG__\n"
                                        "        sub rax, 1\n"
                                        "    #endif\n"
                                        "    nop\n"
                                        "end asm\n");
    CHECK(r.diagnostics.empty());
    CHECK(find(r.roots, "rax", SymbolKind::Sub) == nullptr);
  }
  // An unclosed block still reports at end of file through the open-block
  // sweep. At module level that is the single expected closer.
  {
    ParseResult const r = parseDocument("asm\n"
                                        "    mov eax, 1\n");
    CHECK(diagnosticCount(r, "unterminated-block") == 1);
    CHECK(r.diagnostics.front().message == "Expected 'END ASM'");
    CHECK(fbcErrorFor(r, "unterminated-block") == FbcDiagId::ExpectedEndAsm);
  }
  // Inside a procedure the `end sub` is assembly text too, so both the Asm
  // and the Sub are unterminated — the same pair fbc reports for this
  // source (its errors 45 and 125), innermost block first.
  {
    ParseResult const r = parseDocument("sub demo()\n"
                                        "    asm\n"
                                        "        mov eax, 1\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "unterminated-block") == 2);
    CHECK(r.diagnostics.front().message == "Expected 'END ASM'");
    CHECK(r.diagnostics.back().message == "Expected 'END SUB'");
  }
  // A nested `asm` word is raw text, not an opener: the first `end asm`
  // closes the block, and the leftover closer is the closer machinery's
  // own stray report (fbc: error 33, Illegal 'END' — probed).
  {
    ParseResult const r = parseDocument("asm\n"
                                        "    mov eax, 1\n"
                                        "    asm\n"
                                        "        nop\n"
                                        "    end asm\n"
                                        "end asm\n");
    CHECK(diagnosticCount(r, "unterminated-block") == 0);
    CHECK(diagnosticCount(r, "stray-closer") == 1);
    CHECK(fbcErrorFor(r, "stray-closer") == FbcDiagId::IllegalEnd);
  }
  // Code after `End Asm` parses exactly as before: a declaration after the
  // block exists and a use of it resolves to it.
  {
    std::string const src = "sub demo()\n"
                            "    asm\n"
                            "        sub eax, 1\n"
                            "    end asm\n"
                            "    dim after as integer\n"
                            "    after = 1\n"
                            "end sub\n";
    AnalyzedDoc const doc = analyze(src);
    CHECK(doc.parse.diagnostics.empty());
    const Symbol *demo = find(doc.parse.roots, "demo", SymbolKind::Sub);
    CHECK(demo != nullptr);
    const Symbol *after = find(demo->children, "after", SymbolKind::Dim);
    CHECK(after != nullptr);
    std::size_t const use = src.find("after = 1");
    Symbol const *decl = resolveAt(doc, static_cast<std::uint32_t>(use));
    CHECK(decl == after);
  }
}

static void TestModuleLevelStatementRanges() {
  // Module-level executable statements are recorded; declarations are not.
  // M21 step 3: only the recorded ranges below feed the
  // `module-code-outside-main` diagnostic.
  {
    std::string const src = "dim total as integer\n"
                            "total = 5\n"
                            "helper()\n"
                            "print total\n";
    ParseResult r = parseDocument(src);
    CHECK(r.moduleLevelStatements.size() == 3);
    if (r.moduleLevelStatements.size() == 3) {
      CHECK(src.substr(r.moduleLevelStatements[0].beg,
                       r.moduleLevelStatements[0].end -
                           r.moduleLevelStatements[0].beg) == "total = 5");
      CHECK(src.substr(r.moduleLevelStatements[1].beg,
                       r.moduleLevelStatements[1].end -
                           r.moduleLevelStatements[1].beg) == "helper()");
      CHECK(src.substr(r.moduleLevelStatements[2].beg,
                       r.moduleLevelStatements[2].end -
                           r.moduleLevelStatements[2].beg) == "print total");
    }
  }
  // A module-level control block is ONE statement spanning its closer; the
  // statements in its body are not separate entries.
  {
    std::string const src = "if total then\n"
                            "  print 1\n"
                            "end if\n";
    ParseResult r = parseDocument(src);
    CHECK(r.moduleLevelStatements.size() == 1);
    if (r.moduleLevelStatements.size() == 1) {
      SourceRange const m = r.moduleLevelStatements[0];
      CHECK(m.beg == 0);
      CHECK(src.substr(m.beg, m.end - m.beg) ==
            "if total then\n  print 1\nend if");
    }
  }
  // Nested control blocks collapse to the outermost statement.
  {
    std::string const src = "if 1 then\n"
                            "  for i = 1 to 3\n"
                            "    print i\n"
                            "  next\n"
                            "end if\n";
    ParseResult r = parseDocument(src);
    CHECK(r.moduleLevelStatements.size() == 1);
    if (r.moduleLevelStatements.size() == 1) {
      CHECK(r.moduleLevelStatements[0].beg == 0);
      CHECK(r.moduleLevelStatements[0].end == src.size() - 1);
    }
  }
  // Every module-level control opener counts as one executable statement.
  {
    std::string const src = "for i = 1 to 3\n"
                            "  print i\n"
                            "next\n"
                            "do\n"
                            "  print 2\n"
                            "loop\n"
                            "while 0\n"
                            "  print 3\n"
                            "wend\n"
                            "select case 1\n"
                            "  case 1\n"
                            "    print 4\n"
                            "end select\n"
                            "scope\n"
                            "  print 5\n"
                            "end scope\n"
                            "with 1\n"
                            "  print 6\n"
                            "end with\n";
    ParseResult r = parseDocument(src);
    CHECK(r.moduleLevelStatements.size() == 6);
  }
  // A one-line `if` is one statement, as is a colon compound's each part.
  {
    ParseResult r = parseDocument("if x then print 1\n");
    CHECK(r.moduleLevelStatements.size() == 1);
  }
  {
    std::string const src = "print 1 : print 2\n";
    ParseResult r = parseDocument(src);
    CHECK(r.moduleLevelStatements.size() == 2);
  }
  // A bare `end` terminates; a goto target label is not a statement.
  {
    ParseResult r = parseDocument("print 1\nend\n");
    CHECK(r.moduleLevelStatements.size() == 2);
  }
  {
    std::string const src = "goto skip\n"
                            "skip: print 1\n";
    ParseResult r = parseDocument(src);
    CHECK(r.moduleLevelStatements.size() == 2);
    if (r.moduleLevelStatements.size() == 2) {
      CHECK(src.substr(r.moduleLevelStatements[1].beg,
                       r.moduleLevelStatements[1].end -
                           r.moduleLevelStatements[1].beg) == "print 1");
    }
  }
  // Statements inside a procedure body are not module-level statements.
  {
    ParseResult r = parseDocument("sub work()\n  print 1\nend sub\nprint 2\n");
    CHECK(r.moduleLevelStatements.size() == 1);
    if (r.moduleLevelStatements.size() == 1) {
      CHECK(r.moduleLevelStatements[0].beg ==
            std::string("sub work()\n  print 1\nend sub\nprint 2\n")
                .find("print 2"));
    }
  }
  // None of the declaration forms is a module-level executable statement:
  // `Dim` / `Const` / `Common` (with initializers), `Declare`, `Type` /
  // `Enum` / `Namespace` bodies, `Extern` declarations, and procedure
  // bodies, including module constructors and destructors themselves.
  {
    std::string const src = "dim a as integer\n"
                            "dim b as integer = 5\n"
                            "const k = 7\n"
                            "common c as integer\n"
                            "common shared d as integer\n"
                            "declare sub f()\n"
                            "declare function g() as integer\n"
                            "type point\n"
                            "  x as single\n"
                            "  y as single\n"
                            "end type\n"
                            "enum color\n"
                            "  red\n"
                            "  green\n"
                            "end enum\n"
                            "namespace geo\n"
                            "  const origin = 0\n"
                            "end namespace\n"
                            "extern ext1 as integer\n"
                            "extern \"C\"\n"
                            "  declare sub cfn()\n"
                            "end extern\n"
                            "sub work()\n"
                            "  print 1\n"
                            "end sub\n"
                            "sub init() constructor\n"
                            "  print 2\n"
                            "end sub\n"
                            "sub done() destructor\n"
                            "  print 3\n"
                            "end sub\n";
    ParseResult r = parseDocument(src);
    CHECK(r.moduleLevelStatements.empty());
  }
}

// The `select` diagnostic family (M23 G1): 62 IllegalInsideSelect,
// 34 ExpectedCase, 118 CaseWithoutSelect, 111 EndSelectWithoutSelect,
// 242 IllegalOutsideSelect, 246 ExpectedSelect, 250 NoEnclosedSelect, and
// 323 InvalidCaseRange. Every shape was probed against fbc 1.10.3; the
// anchor is the statement's first token for 62, the offending token for
// 34/246/250, the `select` target token for 242, and the `case` token for
// 323.
static void TestSelectFamily() {
  // 62: a statement between `Select Case` and the first `Case`, one
  // diagnostic per statement (probed: two statements, two errors).
  {
    std::string const src = "dim q as integer = 1\n"
                            "select case q\n"
                            "print \"x\"\n"
                            "case 1\n"
                            "print \"y\"\n"
                            "end select\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "illegal-inside-select") == 1);
    CHECK(fbcErrorFor(r, "illegal-inside-select") ==
          FbcDiagId::IllegalInsideSelect);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].severity == Severity::Error);
      CHECK(r.diagnostics[0].range.beg ==
            static_cast<uint32_t>(src.find("print \"x\"")));
      CHECK(r.diagnostics[0].range.end ==
            static_cast<uint32_t>(src.find("print \"x\"") + 5));
    }
  }
  {
    // Two statements before the first Case: fbc prints 62 twice, so does
    // this parser (per statement, not once per block).
    ParseResult r = parseDocument("dim q as integer = 1\n"
                                  "select case q\n"
                                  "print \"a\"\n"
                                  "print \"b\"\n"
                                  "case 1\n"
                                  "print \"c\"\n"
                                  "end select\n");
    CHECK(diagnosticCount(r, "illegal-inside-select") == 2);
  }
  {
    // A declaration in the region is a statement for this rule (probed:
    // `Dim` and `Const` both draw 62), as is a label and a nested block
    // opener (only the opener, not its body, is directly under the Select).
    ParseResult dim = parseDocument("dim q as integer = 1\n"
                                    "select case q\n"
                                    "dim x as integer\n"
                                    "case 1\n"
                                    "print \"c\"\n"
                                    "end select\n");
    CHECK(diagnosticCount(dim, "illegal-inside-select") == 1);
    ParseResult label = parseDocument("dim q as integer = 1\n"
                                      "select case q\n"
                                      "myLabel:\n"
                                      "case 1\n"
                                      "print \"c\"\n"
                                      "end select\n");
    CHECK(diagnosticCount(label, "illegal-inside-select") == 1);
    ParseResult nested = parseDocument("dim q as integer = 1\n"
                                       "select case q\n"
                                       "if q then\n"
                                       "print \"x\"\n"
                                       "end if\n"
                                       "case 1\n"
                                       "print \"c\"\n"
                                       "end select\n");
    CHECK(diagnosticCount(nested, "illegal-inside-select") == 1);
    // A colon compound is two statements: two diagnostics (probed).
    ParseResult colon = parseDocument("dim q as integer = 1\n"
                                      "select case q : print \"a\" : print "
                                      "\"b\"\n"
                                      "case 1\n"
                                      "print \"c\"\n"
                                      "end select\n");
    CHECK(diagnosticCount(colon, "illegal-inside-select") == 2);
  }
  {
    // Silence: statements inside Case bodies are ordinary code.
    ParseResult r = parseDocument("dim q as integer = 1\n"
                                  "select case q\n"
                                  "case 1\n"
                                  "print \"a\"\n"
                                  "dim x as integer\n"
                                  "case else\n"
                                  "print \"b\"\n"
                                  "end select\n");
    CHECK(diagnosticCount(r, "illegal-inside-select") == 0);
    CHECK(r.diagnostics.empty());
  }

  // 34 ExpectedCase: `Select` not followed by `Case` (probed: `select q`
  // and a bare `select`), and an `End Select` that arrives with no Case
  // ever seen (probed: `select case q` / `end select`).
  {
    std::string const src = "dim q as integer = 1\n"
                            "select q\n"
                            "case 1\n"
                            "print \"x\"\n"
                            "end select\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "expected-case") == 1);
    CHECK(fbcErrorFor(r, "expected-case") == FbcDiagId::ExpectedCase);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].severity == Severity::Error);
      CHECK(r.diagnostics[0].range.beg ==
            static_cast<uint32_t>(src.find("select q") + 7));
      CHECK(r.diagnostics[0].range.end ==
            static_cast<uint32_t>(src.find("select q") + 8));
    }
  }
  {
    std::string const src = "select\n"
                            "case 1\n"
                            "print \"x\"\n"
                            "end select\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "expected-case") == 1);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].range.beg == 0);
      CHECK(r.diagnostics[0].range.end == 6);
    }
  }
  {
    std::string const src = "dim q as integer = 1\n"
                            "select case q\n"
                            "end select\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "expected-case") == 1);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].range.beg ==
            static_cast<uint32_t>(src.find("end select")));
      CHECK(r.diagnostics[0].range.end ==
            static_cast<uint32_t>(src.find("end select") + 3));
    }
    // The As Const spelling takes the same End Select check (probed).
    ParseResult k = parseDocument("dim q as integer = 1\n"
                                  "select case as const q\n"
                                  "end select\n");
    CHECK(diagnosticCount(k, "expected-case") == 1);
  }
  {
    // A well-formed Select is silent, and the Case after a malformed
    // opener still counts as the Select's Case (no second 34 at End
    // Select, probed).
    ParseResult r = parseDocument("dim q as integer = 1\n"
                                  "select case q\n"
                                  "case 1\n"
                                  "print \"x\"\n"
                                  "end select\n");
    CHECK(diagnosticCount(r, "expected-case") == 0);
    CHECK(r.diagnostics.empty());
  }

  // 118 / 111 pinning: the stray Case and stray End Select carry their
  // identities and anchor on the stray word (already routed through
  // `stray-closer`; these pin the Select pair the family owns).
  {
    std::string const src = "case 1\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "stray-closer") == 1);
    CHECK(fbcErrorFor(r, "stray-closer") == FbcDiagId::CaseWithoutSelect);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].severity == Severity::Error);
      CHECK(r.diagnostics[0].range.beg == 0);
      CHECK(r.diagnostics[0].range.end == 4);
    }
    // `Case Else` outside a Select is the same 118 (probed), not 242.
    ParseResult els = parseDocument("case else\n");
    CHECK(fbcErrorFor(els, "stray-closer") == FbcDiagId::CaseWithoutSelect);
    CHECK(diagnosticCount(els, "illegal-outside-select") == 0);
  }
  {
    std::string const src = "end select\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "stray-closer") == 1);
    CHECK(fbcErrorFor(r, "stray-closer") == FbcDiagId::EndSelectWithoutSelect);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].severity == Severity::Error);
      CHECK(r.diagnostics[0].range.beg == 0);
      // The landed stray-closer anchor spans `end select` whole.
      CHECK(r.diagnostics[0].range.end == 10);
    }
  }

  // 242 IllegalOutsideSelect: `Exit Select` with no enclosing Select
  // anywhere up the stack (probed, including inside an If and a Sub).
  {
    std::string const src = "exit select\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "illegal-outside-select") == 1);
    CHECK(fbcErrorFor(r, "illegal-outside-select") ==
          FbcDiagId::IllegalOutsideSelect);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].severity == Severity::Error);
      CHECK(r.diagnostics[0].range.beg == 5);
      CHECK(r.diagnostics[0].range.end == 11);
    }
    ParseResult inIf = parseDocument("if 1 then\n"
                                     "exit select\n"
                                     "end if\n");
    CHECK(diagnosticCount(inIf, "illegal-outside-select") == 1);
    ParseResult inSub = parseDocument("sub foo()\n"
                                      "exit select\n"
                                      "end sub\n");
    CHECK(diagnosticCount(inSub, "illegal-outside-select") == 1);
    // No enclosing Select: the multi-target form is still 242, and does
    // not cascade into 246/250 (probed).
    ParseResult multi = parseDocument("exit select, select\n");
    CHECK(diagnosticCount(multi, "illegal-outside-select") == 1);
    CHECK(diagnosticCount(multi, "expected-select") == 0);
    CHECK(diagnosticCount(multi, "no-enclosed-select") == 0);
  }
  {
    // Silence: Exit Select inside a Select, even through an If (the
    // enclosing Select is what matters, not the innermost block).
    ParseResult r = parseDocument("dim q as integer = 1\n"
                                  "select case q\n"
                                  "case 1\n"
                                  "if q then\n"
                                  "exit select\n"
                                  "end if\n"
                                  "end select\n");
    CHECK(diagnosticCount(r, "illegal-outside-select") == 0);
    CHECK(r.diagnostics.empty());
    // Before the first Case an Exit Select is 62, not a second 242
    // (probed: fbc reports 62 and skips the statement).
    ParseResult before = parseDocument("dim q as integer = 1\n"
                                       "select case q\n"
                                       "exit select\n"
                                       "case 1\n"
                                       "print \"x\"\n"
                                       "end select\n");
    CHECK(diagnosticCount(before, "illegal-inside-select") == 1);
    CHECK(diagnosticCount(before, "illegal-outside-select") == 0);
  }

  // 246 ExpectedSelect: in the multi-target `Exit Select, <target>` form,
  // a target after the comma that is not `Select` (probed: `for`, an
  // identifier, and a trailing comma).
  {
    std::string const src = "dim q as integer = 1\n"
                            "select case q\n"
                            "case 1\n"
                            "exit select, for\n"
                            "end select\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "expected-select") == 1);
    CHECK(fbcErrorFor(r, "expected-select") == FbcDiagId::ExpectedSelect);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].severity == Severity::Error);
      CHECK(r.diagnostics[0].range.beg ==
            static_cast<uint32_t>(src.find("exit select, for") + 13));
      CHECK(r.diagnostics[0].range.end ==
            static_cast<uint32_t>(src.find("exit select, for") + 16));
    }
    ParseResult ident = parseDocument("dim q as integer = 1\n"
                                      "select case q\n"
                                      "case 1\n"
                                      "exit select, foo\n"
                                      "end select\n");
    CHECK(diagnosticCount(ident, "expected-select") == 1);
    ParseResult trailing = parseDocument("dim q as integer = 1\n"
                                         "select case q\n"
                                         "case 1\n"
                                         "exit select,\n"
                                         "end select\n");
    CHECK(diagnosticCount(trailing, "expected-select") == 1);
  }

  // 250 NoEnclosedSelect: a repeated `Select` target with no outer Select
  // left to exit (probed: one enclosing Select, two targets).
  {
    std::string const src = "dim q as integer = 1\n"
                            "select case q\n"
                            "case 1\n"
                            "exit select, select\n"
                            "end select\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "no-enclosed-select") == 1);
    CHECK(fbcErrorFor(r, "no-enclosed-select") == FbcDiagId::NoEnclosedSelect);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].severity == Severity::Error);
      CHECK(r.diagnostics[0].range.beg ==
            static_cast<uint32_t>(src.find("exit select, select") + 13));
      CHECK(r.diagnostics[0].range.end ==
            static_cast<uint32_t>(src.find("exit select, select") + 19));
    }
    ParseResult three = parseDocument("dim q as integer = 1\n"
                                      "dim r as integer = 1\n"
                                      "select case q\n"
                                      "case 1\n"
                                      "select case r\n"
                                      "case 1\n"
                                      "exit select, select, select\n"
                                      "end select\n"
                                      "end select\n");
    CHECK(diagnosticCount(three, "no-enclosed-select") == 1);
  }
  {
    // Silence: two targets, two enclosing Selects.
    ParseResult r = parseDocument("dim q as integer = 1\n"
                                  "dim r as integer = 1\n"
                                  "select case q\n"
                                  "case 1\n"
                                  "select case r\n"
                                  "case 1\n"
                                  "exit select, select\n"
                                  "end select\n"
                                  "end select\n");
    CHECK(diagnosticCount(r, "no-enclosed-select") == 0);
    CHECK(diagnosticCount(r, "expected-select") == 0);
    CHECK(r.diagnostics.empty());
  }

  // 323 InvalidCaseRange: only in an `As Const` Select, where the Case
  // bounds must be constant (probed: literals, constant expressions, and
  // constant names are judged; a variable bound is fbc error 11, not 323;
  // a regular Select never judges the range at all).
  {
    std::string const src = "dim q as integer = 1\n"
                            "select case as const q\n"
                            "case 5 to 2\n"
                            "print \"x\"\n"
                            "end select\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "invalid-case-range") == 1);
    CHECK(fbcErrorFor(r, "invalid-case-range") == FbcDiagId::InvalidCaseRange);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].severity == Severity::Error);
      CHECK(r.diagnostics[0].range.beg ==
            static_cast<uint32_t>(src.find("case 5 to 2")));
      CHECK(r.diagnostics[0].range.end ==
            static_cast<uint32_t>(src.find("case 5 to 2") + 4));
    }
    ParseResult expr = parseDocument("dim q as integer = 1\n"
                                     "select case as const q\n"
                                     "case 1 + 3 to 2\n"
                                     "print \"x\"\n"
                                     "end select\n");
    CHECK(diagnosticCount(expr, "invalid-case-range") == 1);
    ParseResult second = parseDocument("dim q as integer = 1\n"
                                       "select case as const q\n"
                                       "case 1, 5 to 2\n"
                                       "print \"x\"\n"
                                       "end select\n");
    CHECK(diagnosticCount(second, "invalid-case-range") == 1);
    // One diagnostic per Case statement, even with two bad ranges
    // (probed).
    ParseResult two = parseDocument("dim q as integer = 1\n"
                                    "select case as const q\n"
                                    "case 5 to 2, 9 to 1\n"
                                    "print \"x\"\n"
                                    "end select\n");
    CHECK(diagnosticCount(two, "invalid-case-range") == 1);
    ParseResult define = parseDocument("#define LO 5\n"
                                       "#define HI 2\n"
                                       "dim q as integer = 1\n"
                                       "select case as const q\n"
                                       "case LO to HI\n"
                                       "print \"x\"\n"
                                       "end select\n");
    CHECK(diagnosticCount(define, "invalid-case-range") == 1);
    ParseResult constName = parseDocument("const k as integer = 5\n"
                                          "dim q as integer = 1\n"
                                          "select case as const q\n"
                                          "case k to 2\n"
                                          "print \"x\"\n"
                                          "end select\n");
    CHECK(diagnosticCount(constName, "invalid-case-range") == 1);
    ParseResult enumName = parseDocument("enum e\n"
                                         "a = 5\n"
                                         "b = 2\n"
                                         "end enum\n"
                                         "dim q as integer = 1\n"
                                         "select case as const q\n"
                                         "case a to b\n"
                                         "print \"x\"\n"
                                         "end select\n");
    CHECK(diagnosticCount(enumName, "invalid-case-range") == 1);
  }
  {
    // Silence: a forward range, a single value, Case Else, a regular
    // Select's reversed range (fbc accepts it), variable bounds, and
    // string bounds.
    ParseResult ok = parseDocument("dim q as integer = 1\n"
                                   "select case as const q\n"
                                   "case 2 to 5\n"
                                   "print \"x\"\n"
                                   "case 5\n"
                                   "print \"y\"\n"
                                   "case else\n"
                                   "print \"z\"\n"
                                   "end select\n");
    CHECK(diagnosticCount(ok, "invalid-case-range") == 0);
    CHECK(ok.diagnostics.empty());
    ParseResult regular = parseDocument("dim q as integer = 1\n"
                                        "select case q\n"
                                        "case 5 to 2\n"
                                        "print \"x\"\n"
                                        "end select\n");
    CHECK(diagnosticCount(regular, "invalid-case-range") == 0);
    ParseResult var = parseDocument("dim q as integer = 1\n"
                                    "dim a as integer = 5\n"
                                    "select case as const q\n"
                                    "case a to 2\n"
                                    "print \"x\"\n"
                                    "end select\n");
    CHECK(diagnosticCount(var, "invalid-case-range") == 0);
    ParseResult str = parseDocument("dim s as string = \"x\"\n"
                                    "select case s\n"
                                    "case \"b\" to \"a\"\n"
                                    "print \"x\"\n"
                                    "end select\n");
    CHECK(diagnosticCount(str, "invalid-case-range") == 0);
  }
}
// The loop families (M23 G2): 13 EXPECTEDNEXT, 29 EXPECTEDLOOP,
// 30 EXPECTEDWEND, 106 LOOPWITHOUTDO, 107 NEXTWITHOUTFOR,
// 108 WENDWITHOUTWHILE, 283 FORNEXTVARIABLEMISMATCH, and warning 26
// NEXTVARMEANINGLESS. Every shape was probed against fbc 1.10.3; the
// anchor is the opener for the Expected pair, the stray word for the
// without pair, and the name token for 283/warning 26.
static void TestLoopFamily() {
  // The false-positive fix: one `Next` closes one `For` per name it
  // carries, innermost first, so `next j, i` is silent. A bare `Next`
  // closes one. (Probed: fbc compiles the multi-name form clean; before
  // this wave it drew a bogus `unterminated-block` for the outer loop.)
  {
    std::string const src = "dim i as integer\n"
                            "dim j as integer\n"
                            "for i = 1 to 2\n"
                            "  for j = 1 to 2\n"
                            "    print i, j\n"
                            "  next j, i\n"
                            "print \"ok\"\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "unterminated-block") == 0);
    CHECK(diagnosticCount(r, "for-next-variable-mismatch") == 0);
    CHECK(diagnosticCount(r, "stray-closer") == 0);
    CHECK(diagnosticCount(r, "next-var-meaningless") == 2);
    CHECK(fbcErrorFor(r, "next-var-meaningless") ==
          FbcDiagId::NextVarMeaningless);
    CHECK(r.diagnostics.size() == 2);
    if (r.diagnostics.size() == 2) {
      CHECK(r.diagnostics[0].severity == Severity::Warning);
      CHECK(r.diagnostics[0].fbcKind == FbcMessageKind::Warning);
      CHECK(r.diagnostics[0].range.beg ==
            static_cast<uint32_t>(src.find("next j, i") + 5));
      CHECK(r.diagnostics[0].range.end ==
            static_cast<uint32_t>(src.find("next j, i") + 6));
      CHECK(r.diagnostics[0].message ==
            "variable following NEXT is meaningless: 'j'");
      CHECK(r.diagnostics[1].range.beg ==
            static_cast<uint32_t>(src.find("next j, i") + 8));
      CHECK(r.diagnostics[1].range.end ==
            static_cast<uint32_t>(src.find("next j, i") + 9));
    }
    // Separate `Next`s (each closes its own loop) are silent too, and a
    // trailing comma still closes the outer loop bare (probed).
    ParseResult separate = parseDocument("dim i as integer\n"
                                         "dim j as integer\n"
                                         "for i = 1 to 2\n"
                                         "  for j = 1 to 2\n"
                                         "    print i, j\n"
                                         "  next j\n"
                                         "next i\n"
                                         "print \"ok\"\n");
    CHECK(diagnosticCount(separate, "for-next-variable-mismatch") == 0);
    CHECK(diagnosticCount(separate, "unterminated-block") == 0);
    CHECK(diagnosticCount(separate, "next-var-meaningless") == 2);
    ParseResult trailing = parseDocument("dim i as integer\n"
                                         "dim j as integer\n"
                                         "for i = 1 to 2\n"
                                         "  for j = 1 to 2\n"
                                         "    print i, j\n"
                                         "  next j,\n"
                                         "print \"ok\"\n");
    CHECK(diagnosticCount(trailing, "for-next-variable-mismatch") == 0);
    CHECK(diagnosticCount(trailing, "stray-closer") == 0);
    CHECK(diagnosticCount(trailing, "next-var-meaningless") == 1);
  }

  // 283 FORNEXTVARIABLEMISMATCH: each name is checked against the `For`
  // it closes, anchored on its token, the message naming the counter
  // (expected) and the name found. A `Next` still closes its loop on a
  // mismatch, so no `unterminated-block` follows (probed).
  {
    std::string const src = "dim i as integer\n"
                            "dim k as integer\n"
                            "for i = 1 to 2\n"
                            "  print i\n"
                            "next k\n"
                            "print \"ok\"\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "for-next-variable-mismatch") == 1);
    CHECK(fbcErrorFor(r, "for-next-variable-mismatch") ==
          FbcDiagId::ForNextVariableMismatch);
    CHECK(diagnosticCount(r, "next-var-meaningless") == 1);
    CHECK(diagnosticCount(r, "unterminated-block") == 0);
    CHECK(r.diagnostics.size() == 2);
    if (!r.diagnostics.empty()) {
      CHECK(r.diagnostics[0].severity == Severity::Error);
      CHECK(r.diagnostics[0].range.beg ==
            static_cast<uint32_t>(src.find("next k") + 5));
      CHECK(r.diagnostics[0].range.end ==
            static_cast<uint32_t>(src.find("next k") + 6));
      CHECK(r.diagnostics[0].message ==
            "FOR/NEXT variable name mismatch: expected 'i', found 'k'");
    }
    // Outer name first (`next i, j`): both names mismatch their loops,
    // one diagnostic per name (probed: two 283, in name order).
    ParseResult outerFirst = parseDocument("dim i as integer\n"
                                           "dim j as integer\n"
                                           "for i = 1 to 2\n"
                                           "  for j = 1 to 2\n"
                                           "    print i, j\n"
                                           "  next i, j\n"
                                           "print \"ok\"\n");
    CHECK(diagnosticCount(outerFirst, "for-next-variable-mismatch") == 2);
    CHECK(diagnosticCount(outerFirst, "next-var-meaningless") == 2);
    CHECK(diagnosticCount(outerFirst, "unterminated-block") == 0);
    CHECK(diagnosticCount(outerFirst, "stray-closer") == 0);
    // Skipping the inner loop with two single-name `Next`s: each closes
    // the innermost loop and mismatches it (probed: 283 on each line).
    ParseResult skip = parseDocument("dim i as integer\n"
                                     "dim j as integer\n"
                                     "for i = 1 to 2\n"
                                     "  for j = 1 to 2\n"
                                     "    print i, j\n"
                                     "  next i\n"
                                     "next j\n"
                                     "print \"ok\"\n");
    CHECK(diagnosticCount(skip, "for-next-variable-mismatch") == 2);
    CHECK(diagnosticCount(skip, "unterminated-block") == 0);
    // A member spelling in `Next` is still a name to judge (probed: 283).
    ParseResult memberNext = parseDocument("type t\n"
                                           "  x as integer\n"
                                           "end type\n"
                                           "dim w as t\n"
                                           "dim i as integer\n"
                                           "for i = 1 to 2\n"
                                           "  print i\n"
                                           "next w.x\n"
                                           "print \"ok\"\n");
    CHECK(diagnosticCount(memberNext, "for-next-variable-mismatch") == 1);
  }

  // Silence on the probed clean shapes: different casing, a suffix that
  // canonicalizes away, and a bare `Next`. Names compare by canonical
  // key, never raw (probed: `next I` and `next i$` compile).
  {
    ParseResult casing = parseDocument("dim i as integer\n"
                                       "for i = 1 to 2\n"
                                       "  print i\n"
                                       "next I\n"
                                       "print \"ok\"\n");
    CHECK(diagnosticCount(casing, "for-next-variable-mismatch") == 0);
    CHECK(diagnosticCount(casing, "next-var-meaningless") == 1);
    ParseResult suffix = parseDocument("dim i as integer\n"
                                       "for i = 1 to 2\n"
                                       "  print i\n"
                                       "next i$\n"
                                       "print \"ok\"\n");
    CHECK(diagnosticCount(suffix, "for-next-variable-mismatch") == 0);
    CHECK(diagnosticCount(suffix, "next-var-meaningless") == 1);
    ParseResult bare = parseDocument("dim i as integer\n"
                                     "for i = 1 to 2\n"
                                     "  print i\n"
                                     "next\n"
                                     "print \"ok\"\n");
    CHECK(bare.diagnostics.empty());
    ParseResult typed = parseDocument("for i as integer = 1 to 2\n"
                                      "  print i\n"
                                      "next i\n"
                                      "print \"ok\"\n");
    CHECK(diagnosticCount(typed, "for-next-variable-mismatch") == 0);
    CHECK(diagnosticCount(typed, "next-var-meaningless") == 1);
  }

  // Undeclared and non-plain-counter shapes stay silent for 283. An
  // undeclared `Next` name is fbc error 42 (the resolve layer, not this
  // family), and a member/element counter cannot be named (fbc error
  // 52, outside this family); the warning still runs for the latter.
  {
    ParseResult undec = parseDocument("dim i as integer\n"
                                      "for i = 1 to 2\n"
                                      "  print i\n"
                                      "next qq\n"
                                      "print \"ok\"\n");
    CHECK(undec.diagnostics.empty());
    ParseResult memberCounter = parseDocument("type t\n"
                                              "  x as integer\n"
                                              "end type\n"
                                              "dim w as t\n"
                                              "for w.x = 1 to 2\n"
                                              "  print w.x\n"
                                              "next w.x\n"
                                              "print \"ok\"\n");
    CHECK(diagnosticCount(memberCounter, "for-next-variable-mismatch") == 0);
    CHECK(diagnosticCount(memberCounter, "next-var-meaningless") == 1);
    ParseResult elementCounter = parseDocument("dim a(10) as integer\n"
                                               "for a(0) = 1 to 2\n"
                                               "  print a(0)\n"
                                               "next a(0)\n"
                                               "print \"ok\"\n");
    CHECK(diagnosticCount(elementCounter, "for-next-variable-mismatch") == 0);
    CHECK(diagnosticCount(elementCounter, "next-var-meaningless") == 1);
  }

  // No cascades: names that run out of `For`s report the stray remainder
  // once through `stray-closer`, never once per extra name (probed:
  // excess names draw no warning either).
  {
    ParseResult excess = parseDocument("dim i as integer\n"
                                       "dim j as integer\n"
                                       "dim k as integer\n"
                                       "for i = 1 to 2\n"
                                       "  for j = 1 to 2\n"
                                       "    print i, j\n"
                                       "  next j, i, k\n"
                                       "print \"ok\"\n");
    CHECK(diagnosticCount(excess, "stray-closer") == 1);
    CHECK(fbcErrorFor(excess, "stray-closer") == FbcDiagId::NextWithoutFor);
    CHECK(diagnosticCount(excess, "next-var-meaningless") == 2);
    CHECK(diagnosticCount(excess, "unterminated-block") == 0);
    CHECK(diagnosticCount(excess, "for-next-variable-mismatch") == 0);
    ParseResult twoExtra = parseDocument("dim i as integer\n"
                                         "dim j as integer\n"
                                         "dim k as integer\n"
                                         "dim k2 as integer\n"
                                         "for i = 1 to 2\n"
                                         "  for j = 1 to 2\n"
                                         "    print i, j\n"
                                         "  next j, i, k, k2\n"
                                         "print \"ok\"\n");
    CHECK(diagnosticCount(twoExtra, "stray-closer") == 1);
    CHECK(diagnosticCount(twoExtra, "next-var-meaningless") == 2);
    ParseResult dup = parseDocument("dim i as integer\n"
                                    "for i = 1 to 2\n"
                                    "  print i\n"
                                    "next i, i\n"
                                    "print \"ok\"\n");
    CHECK(diagnosticCount(dup, "stray-closer") == 1);
    CHECK(diagnosticCount(dup, "next-var-meaningless") == 1);
    CHECK(diagnosticCount(dup, "unterminated-block") == 0);
  }

  // 13 / 29 / 30 pinning: the unterminated loop carries its specific
  // Expected identity, anchored on the opener (already routed; these
  // pin the loop trio the family owns).
  {
    std::string const srcFor = "for i = 1 to 2\nprint i\n";
    ParseResult rFor = parseDocument(srcFor);
    CHECK(diagnosticCount(rFor, "unterminated-block") == 1);
    CHECK(fbcErrorFor(rFor, "unterminated-block") == FbcDiagId::ExpectedNext);
    CHECK(rFor.diagnostics.size() == 1);
    if (rFor.diagnostics.size() == 1) {
      CHECK(rFor.diagnostics[0].severity == Severity::Error);
      CHECK(rFor.diagnostics[0].range.beg == 0);
      CHECK(rFor.diagnostics[0].range.end == 3);
    }
    ParseResult rWhile = parseDocument("while 1\nprint 1\n");
    CHECK(diagnosticCount(rWhile, "unterminated-block") == 1);
    CHECK(fbcErrorFor(rWhile, "unterminated-block") == FbcDiagId::ExpectedWend);
    CHECK(rWhile.diagnostics.size() == 1);
    if (rWhile.diagnostics.size() == 1) {
      CHECK(rWhile.diagnostics[0].range.beg == 0);
      CHECK(rWhile.diagnostics[0].range.end == 5);
    }
    ParseResult rDo = parseDocument("do\nprint 1\n");
    CHECK(diagnosticCount(rDo, "unterminated-block") == 1);
    CHECK(fbcErrorFor(rDo, "unterminated-block") == FbcDiagId::ExpectedLoop);
    CHECK(rDo.diagnostics.size() == 1);
    if (rDo.diagnostics.size() == 1) {
      CHECK(rDo.diagnostics[0].range.beg == 0);
      CHECK(rDo.diagnostics[0].range.end == 2);
    }
  }

  // 106 / 107 / 108 pinning: the stray closer carries its specific
  // without identity, anchored on the stray word (already routed
  // through `stray-closer`; these pin the loop trio).
  {
    std::string const srcNext = "next\n";
    ParseResult rNext = parseDocument(srcNext);
    CHECK(diagnosticCount(rNext, "stray-closer") == 1);
    CHECK(fbcErrorFor(rNext, "stray-closer") == FbcDiagId::NextWithoutFor);
    CHECK(rNext.diagnostics.size() == 1);
    if (rNext.diagnostics.size() == 1) {
      CHECK(rNext.diagnostics[0].severity == Severity::Error);
      CHECK(rNext.diagnostics[0].range.beg == 0);
      CHECK(rNext.diagnostics[0].range.end == 4);
    }
    ParseResult rNextNamed = parseDocument("next i, j\n");
    CHECK(diagnosticCount(rNextNamed, "stray-closer") == 1);
    CHECK(fbcErrorFor(rNextNamed, "stray-closer") == FbcDiagId::NextWithoutFor);
    CHECK(diagnosticCount(rNextNamed, "next-var-meaningless") == 0);
    ParseResult rLoop = parseDocument("loop\n");
    CHECK(diagnosticCount(rLoop, "stray-closer") == 1);
    CHECK(fbcErrorFor(rLoop, "stray-closer") == FbcDiagId::LoopWithoutDo);
    CHECK(rLoop.diagnostics.size() == 1);
    if (rLoop.diagnostics.size() == 1) {
      CHECK(rLoop.diagnostics[0].range.beg == 0);
      CHECK(rLoop.diagnostics[0].range.end == 4);
    }
    ParseResult rWend = parseDocument("wend\n");
    CHECK(diagnosticCount(rWend, "stray-closer") == 1);
    CHECK(fbcErrorFor(rWend, "stray-closer") == FbcDiagId::WendWithoutWhile);
    CHECK(rWend.diagnostics.size() == 1);
    if (rWend.diagnostics.size() == 1) {
      CHECK(rWend.diagnostics[0].range.beg == 0);
      CHECK(rWend.diagnostics[0].range.end == 4);
    }
  }

  // Warning 26 NEXTVARMEANINGLESS: one per declared name after `Next`
  // (matching or not, probed), none for a bare `Next` or an undeclared
  // name. As a warning it rides the landed `-w` gate (catalog level 1)
  // and the parser records it unconditionally.
  {
    ParseResult named = parseDocument("dim i as integer\n"
                                      "for i = 1 to 2\n"
                                      "  print i\n"
                                      "next i\n"
                                      "print \"ok\"\n");
    CHECK(diagnosticCount(named, "next-var-meaningless") == 1);
    CHECK(fbcErrorFor(named, "next-var-meaningless") ==
          FbcDiagId::NextVarMeaningless);
    CHECK(named.diagnostics.size() == 1);
    if (named.diagnostics.size() == 1) {
      CHECK(named.diagnostics[0].severity == Severity::Warning);
      CHECK(named.diagnostics[0].fbcKind == FbcMessageKind::Warning);
    }
    ParseResult bare = parseDocument("dim i as integer\n"
                                     "for i = 1 to 2\n"
                                     "  print i\n"
                                     "next\n"
                                     "print \"ok\"\n");
    CHECK(diagnosticCount(bare, "next-var-meaningless") == 0);
    CHECK(bare.diagnostics.empty());
    // A non-identifier tail still closes one loop bare (fbc error 3
    // belongs to G5 and is not judged here).
    ParseResult num = parseDocument("dim i as integer\n"
                                    "for i = 1 to 2\n"
                                    "  print i\n"
                                    "next 1\n"
                                    "print \"ok\"\n");
    CHECK(diagnosticCount(num, "next-var-meaningless") == 0);
    CHECK(diagnosticCount(num, "for-next-variable-mismatch") == 0);
    CHECK(diagnosticCount(num, "unterminated-block") == 0);
    ParseResult paren = parseDocument("dim i as integer\n"
                                      "for i = 1 to 2\n"
                                      "  print i\n"
                                      "next (i)\n"
                                      "print \"ok\"\n");
    CHECK(diagnosticCount(paren, "next-var-meaningless") == 0);
    CHECK(diagnosticCount(paren, "for-next-variable-mismatch") == 0);
    // `next i+1` carries its name (warning 26) while the `+1` tail stays
    // for G5 (probed: warning 26 plus error 3).
    ParseResult expr = parseDocument("dim i as integer\n"
                                     "for i = 1 to 2\n"
                                     "  print i\n"
                                     "next i+1\n"
                                     "print \"ok\"\n");
    CHECK(diagnosticCount(expr, "next-var-meaningless") == 1);
    CHECK(diagnosticCount(expr, "for-next-variable-mismatch") == 0);
  }
}

// M23 G3: the `Exit` / `Continue` target-word family. 251
// INVALIDEXITSTMT and 252 INVALIDCONTINUESTMT are purely lexical (probed at
// fbc 1.10.3): the word after the keyword alone decides, in every context,
// and every placement question about an accepted target belongs to another
// family (239/240/241 for a loop target with no enclosing loop, G1's
// 242/246/250 for `Exit Select`, 253/254/... for a procedure target outside
// its own procedure) and stays silent here.
static void TestExitContinueFamily() {
  // 251 InvalidExitStmt: the bare form, anchored on the `exit` keyword
  // itself. One diagnostic, no cascade — the enclosing loop still closes
  // (probed: fbc reports 251 and skips the statement).
  {
    std::string const src = "dim i as integer\n"
                            "for i = 1 to 2\n"
                            "  exit\n"
                            "next\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "invalid-exit") == 1);
    CHECK(fbcErrorFor(r, "invalid-exit") == FbcDiagId::InvalidExitStmt);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].severity == Severity::Error);
      CHECK(r.diagnostics[0].fbcKind == FbcMessageKind::Error);
      std::size_t const at = src.find("exit\n");
      CHECK(r.diagnostics[0].range.beg == static_cast<uint32_t>(at));
      CHECK(r.diagnostics[0].range.end == static_cast<uint32_t>(at + 4));
      CHECK(r.diagnostics[0].message ==
            "expected 'CONSTRUCTOR', 'DESTRUCTOR', 'DO', 'FOR', 'FUNCTION', "
            "'OPERATOR', 'PROPERTY', 'SELECT', 'SUB' or 'WHILE' after EXIT");
    }
    CHECK(diagnosticCount(r, "unterminated-block") == 0);
    CHECK(diagnosticCount(r, "stray-closer") == 0);
    // Every context draws the same one diagnostic (probed: Do, While,
    // Select after a Case, Sub, module level).
    ParseResult inDo = parseDocument("do\n  exit\nloop\n");
    CHECK(diagnosticCount(inDo, "invalid-exit") == 1);
    CHECK(inDo.diagnostics.size() == 1);
    ParseResult inWhile = parseDocument("while 1\n  exit\nwend\n");
    CHECK(diagnosticCount(inWhile, "invalid-exit") == 1);
    ParseResult inSelect = parseDocument("dim q as integer = 1\n"
                                         "select case q\n"
                                         "case 1\n"
                                         "  exit\n"
                                         "end select\n");
    CHECK(diagnosticCount(inSelect, "invalid-exit") == 1);
    CHECK(diagnosticCount(inSelect, "illegal-inside-select") == 0);
    ParseResult inSub = parseDocument("sub s()\n  exit\nend sub\n");
    CHECK(diagnosticCount(inSub, "invalid-exit") == 1);
    CHECK(inSub.diagnostics.size() == 1);
    ParseResult atModule = parseDocument("exit\n");
    CHECK(diagnosticCount(atModule, "invalid-exit") == 1);
    CHECK(atModule.diagnostics.size() == 1);
  }

  // 251: a word fbc does not accept, anchored on the word token and naming
  // it in the message. An identifier, an uppercase identifier (as written),
  // and keywords outside the nine (probed: `if`, `scope`, `type`, `end`).
  {
    std::string const src = "sub s()\n  exit foo\nend sub\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "invalid-exit") == 1);
    CHECK(fbcErrorFor(r, "invalid-exit") == FbcDiagId::InvalidExitStmt);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].severity == Severity::Error);
      std::size_t const at = src.find("foo");
      CHECK(r.diagnostics[0].range.beg == static_cast<uint32_t>(at));
      CHECK(r.diagnostics[0].range.end == static_cast<uint32_t>(at + 3));
      CHECK(r.diagnostics[0].message ==
            "expected 'CONSTRUCTOR', 'DESTRUCTOR', 'DO', 'FOR', 'FUNCTION', "
            "'OPERATOR', 'PROPERTY', 'SELECT', 'SUB' or 'WHILE' after EXIT, "
            "found 'foo'");
    }
    ParseResult upper = parseDocument("sub s()\n  exit FOO\nend sub\n");
    CHECK(diagnosticCount(upper, "invalid-exit") == 1);
    if (upper.diagnostics.size() == 1) {
      // The found word is the spelling as written, not folded.
      std::size_t const at =
          std::string("sub s()\n  exit FOO\nend sub\n").find("FOO");
      CHECK(upper.diagnostics[0].range.beg == static_cast<uint32_t>(at));
      CHECK(upper.diagnostics[0].message.find("found 'FOO'") !=
            std::string::npos);
    }
    ParseResult inIf = parseDocument("if 1 then\n  exit if\nend if\n");
    CHECK(diagnosticCount(inIf, "invalid-exit") == 1);
    ParseResult inScope = parseDocument("scope\n  exit scope\nend scope\n");
    CHECK(diagnosticCount(inScope, "invalid-exit") == 1);
    ParseResult typeWord = parseDocument("sub s()\n  exit type\nend sub\n");
    CHECK(diagnosticCount(typeWord, "invalid-exit") == 1);
    ParseResult endWord = parseDocument("sub s()\n  exit end\nend sub\n");
    CHECK(diagnosticCount(endWord, "invalid-exit") == 1);
    CHECK(endWord.diagnostics.size() == 1);
  }

  // 251: a tail that is not a word — a separator or a literal — is the
  // bare form (fbc drops its found clause for a literal too, probed):
  // anchored on the `exit` keyword, no found word in the message.
  {
    ParseResult comma = parseDocument("sub s()\n  exit ,\nend sub\n");
    CHECK(diagnosticCount(comma, "invalid-exit") == 1);
    CHECK(comma.diagnostics.size() == 1);
    if (comma.diagnostics.size() == 1) {
      std::string const src = "sub s()\n  exit ,\nend sub\n";
      CHECK(comma.diagnostics[0].range.beg ==
            static_cast<uint32_t>(src.find("exit")));
      CHECK(comma.diagnostics[0].range.end ==
            static_cast<uint32_t>(src.find("exit") + 4));
      CHECK(comma.diagnostics[0].message.find("found") == std::string::npos);
    }
    ParseResult num = parseDocument("sub s()\n  exit 123\nend sub\n");
    CHECK(diagnosticCount(num, "invalid-exit") == 1);
    CHECK(num.diagnostics.size() == 1);
  }

  // 251: one per statement, and fbc's recovery skips the rest of the
  // statement — a comma tail after a bad word is never judged (probed:
  // `exit foo, bar` is one 251), two bad exits draw two, a bad exit does
  // not cascade into the closer shapes of the block it sits in.
  {
    ParseResult multi = parseDocument("sub s()\n  exit foo, bar\nend sub\n");
    CHECK(diagnosticCount(multi, "invalid-exit") == 1);
    CHECK(multi.diagnostics.size() == 1);
    ParseResult two = parseDocument("sub s()\n  exit foo\n  exit bar\n"
                                    "end sub\n");
    CHECK(diagnosticCount(two, "invalid-exit") == 2);
    CHECK(two.diagnostics.size() == 2);
    ParseResult thenValid =
        parseDocument("sub s()\n  exit foo\n  print \"x\"\nend sub\n");
    CHECK(diagnosticCount(thenValid, "invalid-exit") == 1);
    CHECK(thenValid.diagnostics.size() == 1);
    ParseResult inFor = parseDocument("dim i as integer\n"
                                      "for i = 1 to 2\n"
                                      "  exit foo\n"
                                      "next\n"
                                      "print \"ok\"\n");
    CHECK(diagnosticCount(inFor, "invalid-exit") == 1);
    CHECK(diagnosticCount(inFor, "unterminated-block") == 0);
    CHECK(diagnosticCount(inFor, "stray-closer") == 0);
    CHECK(diagnosticCount(inFor, "next-var-meaningless") == 0);
  }

  // 252 InvalidContinueStmt: the bare form anchored on the `continue`
  // keyword, in every context (probed: For, Do, While, Select after a
  // Case, Sub, module level).
  {
    std::string const src = "dim i as integer\n"
                            "for i = 1 to 2\n"
                            "  continue\n"
                            "next\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "invalid-continue") == 1);
    CHECK(fbcErrorFor(r, "invalid-continue") == FbcDiagId::InvalidContinueStmt);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].severity == Severity::Error);
      std::size_t const at = src.find("continue\n");
      CHECK(r.diagnostics[0].range.beg == static_cast<uint32_t>(at));
      CHECK(r.diagnostics[0].range.end == static_cast<uint32_t>(at + 8));
      CHECK(r.diagnostics[0].message ==
            "expected 'DO', 'FOR' or 'WHILE' after CONTINUE");
    }
    CHECK(diagnosticCount(r, "unterminated-block") == 0);
    ParseResult inSub = parseDocument("sub s()\n  continue\nend sub\n");
    CHECK(diagnosticCount(inSub, "invalid-continue") == 1);
    ParseResult atModule = parseDocument("continue\n");
    CHECK(diagnosticCount(atModule, "invalid-continue") == 1);
    CHECK(atModule.diagnostics.size() == 1);
  }

  // 252: words outside the three (probed: `sub`, `select`, an identifier),
  // anchored on the word and named in the message.
  {
    std::string const src =
        "dim i as integer\nfor i = 1 to 2\n  continue sub\nnext\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "invalid-continue") == 1);
    CHECK(fbcErrorFor(r, "invalid-continue") == FbcDiagId::InvalidContinueStmt);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      std::size_t const at = src.find("sub");
      CHECK(r.diagnostics[0].range.beg == static_cast<uint32_t>(at));
      CHECK(r.diagnostics[0].range.end == static_cast<uint32_t>(at + 3));
      CHECK(r.diagnostics[0].message ==
            "expected 'DO', 'FOR' or 'WHILE' after CONTINUE, found 'sub'");
    }
    ParseResult selectWord = parseDocument("dim q as integer = 1\n"
                                           "select case q\n"
                                           "case 1\n"
                                           "  continue select\n"
                                           "end select\n");
    CHECK(diagnosticCount(selectWord, "invalid-continue") == 1);
    CHECK(diagnosticCount(selectWord, "illegal-inside-select") == 0);
    ParseResult ident =
        parseDocument("dim i as integer\nfor i = 1 to 2\n  continue foo\n"
                      "next\n");
    CHECK(diagnosticCount(ident, "invalid-continue") == 1);
    CHECK(ident.diagnostics.size() == 1);
  }

  // Silence: an accepted target never draws 251/252, wherever it sits.
  // The matching loop targets (probed), including through transparent
  // blocks (If, Scope, With) and past a non-matching inner block (the
  // target is found up the stack, not on the innermost block).
  {
    ParseResult matching = parseDocument("dim i as integer\n"
                                         "for i = 1 to 2\n"
                                         "  exit for\n"
                                         "next\n");
    CHECK(diagnosticCount(matching, "invalid-exit") == 0);
    CHECK(matching.diagnostics.empty());
    ParseResult inDo = parseDocument("do\n  exit do\nloop\n");
    CHECK(inDo.diagnostics.empty());
    ParseResult inWhile = parseDocument("while 1\n  exit while\nwend\n");
    CHECK(inWhile.diagnostics.empty());
    ParseResult contFor = parseDocument("dim i as integer\n"
                                        "for i = 1 to 2\n"
                                        "  continue for\n"
                                        "next\n");
    CHECK(contFor.diagnostics.empty());
    // Upper-case spelling: keywords fold, as fbc does.
    ParseResult upper = parseDocument("dim i as integer\n"
                                      "for i = 1 to 2\n"
                                      "  EXIT FOR\n"
                                      "next\n");
    CHECK(upper.diagnostics.empty());
    // A suffix that rode on the target keyword is shed (warning 44 is the
    // landed report for it), so `exit sub$` is `exit sub`.
    ParseResult suffixed = parseDocument("sub s()\n  exit sub$\nend sub\n");
    CHECK(diagnosticCount(suffixed, "invalid-exit") == 0);
    CHECK(diagnosticCount(suffixed, "keyword-suffix") == 1);
    // Not the innermost block, but enclosing (probed: `exit for` inside a
    // Do inside a For compiles).
    ParseResult forInDoInFor = parseDocument("dim i as integer\n"
                                             "for i = 1 to 2\n"
                                             "  do\n"
                                             "    exit for\n"
                                             "  loop\n"
                                             "next\n");
    CHECK(forInDoInFor.diagnostics.empty());
    ParseResult contInDoInFor = parseDocument("dim i as integer\n"
                                              "for i = 1 to 2\n"
                                              "  do\n"
                                              "    continue for\n"
                                              "  loop\n"
                                              "next\n");
    CHECK(contInDoInFor.diagnostics.empty());
    ParseResult doInForInDo = parseDocument("do\n"
                                            "  dim i as integer\n"
                                            "  for i = 1 to 2\n"
                                            "    exit do\n"
                                            "  next\n"
                                            "loop\n");
    CHECK(doInForInDo.diagnostics.empty());
    // Transparent blocks: If, Scope, With inside the loop.
    ParseResult inIf = parseDocument("dim i as integer\n"
                                     "for i = 1 to 2\n"
                                     "  if i then\n"
                                     "    exit for\n"
                                     "  end if\n"
                                     "next\n");
    CHECK(inIf.diagnostics.empty());
    ParseResult inScope = parseDocument("dim i as integer\n"
                                        "for i = 1 to 2\n"
                                        "  scope\n"
                                        "    exit for\n"
                                        "    continue for\n"
                                        "  end scope\n"
                                        "next\n");
    CHECK(inScope.diagnostics.empty());
    ParseResult inWith = parseDocument("dim i as integer\n"
                                       "type T\n"
                                       "  a as integer\n"
                                       "end type\n"
                                       "dim w as T\n"
                                       "for i = 1 to 2\n"
                                       "  with w\n"
                                       "    exit for\n"
                                       "  end with\n"
                                       "next\n");
    CHECK(inWith.diagnostics.empty());
  }

  // Silence: procedure targets inside their own procedure, from inside a
  // loop and through transparent blocks (probed: `exit sub` in a For in a
  // Sub, `exit function` in a For in a Function, through If/Scope/With/
  // Select, and each procedure kind in its own body).
  {
    ParseResult subFromLoop = parseDocument("sub s()\n"
                                            "  dim i as integer\n"
                                            "  for i = 1 to 2\n"
                                            "    exit sub\n"
                                            "  next\n"
                                            "end sub\n");
    CHECK(subFromLoop.diagnostics.empty());
    ParseResult fnFromLoop = parseDocument("function f() as integer\n"
                                           "  dim i as integer\n"
                                           "  for i = 1 to 2\n"
                                           "    exit function\n"
                                           "  next\n"
                                           "  return 0\n"
                                           "end function\n");
    CHECK(fnFromLoop.diagnostics.empty());
    ParseResult throughBlocks = parseDocument("sub s()\n"
                                              "  if 1 then\n"
                                              "    exit sub\n"
                                              "  end if\n"
                                              "  scope\n"
                                              "    exit sub\n"
                                              "  end scope\n"
                                              "  dim q as integer = 1\n"
                                              "  select case q\n"
                                              "  case 1\n"
                                              "    exit sub\n"
                                              "  end select\n"
                                              "end sub\n");
    CHECK(throughBlocks.diagnostics.empty());
    ParseResult kinds = parseDocument("type T\n"
                                      "  x as integer\n"
                                      "  declare constructor()\n"
                                      "  declare destructor()\n"
                                      "  declare property p() as integer\n"
                                      "  declare operator cast() as integer\n"
                                      "end type\n"
                                      "constructor T()\n"
                                      "  exit constructor\n"
                                      "end constructor\n"
                                      "destructor T()\n"
                                      "  exit destructor\n"
                                      "end destructor\n"
                                      "property T.p() as integer\n"
                                      "  exit property\n"
                                      "  return x\n"
                                      "end property\n"
                                      "operator T.cast() as integer\n"
                                      "  exit operator\n"
                                      "  return x\n"
                                      "end operator\n");
    CHECK(diagnosticCount(kinds, "invalid-exit") == 0);
    CHECK(kinds.diagnostics.empty());
    // `Exit Select` inside a Select stays exactly the G1 behaviour: no
    // 251 from this family on an accepted `select` target.
    ParseResult exitSelect = parseDocument("dim q as integer = 1\n"
                                           "select case q\n"
                                           "case 1\n"
                                           "  exit select\n"
                                           "end select\n");
    CHECK(diagnosticCount(exitSelect, "invalid-exit") == 0);
    CHECK(diagnosticCount(exitSelect, "illegal-outside-select") == 0);
    CHECK(exitSelect.diagnostics.empty());
  }

  // The probed split — accepted targets whose *placement* fbc rejects draw
  // other families' numbers, all outside G3, and stay silent here: a loop
  // target with no enclosing loop of its kind is 239/240/241, a procedure
  // target outside its own procedure is 253/254, and the loop multi-target
  // forms are 243/244/245/247/248/249 (none implemented in this family).
  {
    ParseResult exitForInSub = parseDocument("sub s()\n  exit for\n"
                                             "end sub\n");
    CHECK(diagnosticCount(exitForInSub, "invalid-exit") == 0);
    CHECK(exitForInSub.diagnostics.empty());
    ParseResult exitForInDo = parseDocument("do\n  exit for\nloop\n");
    CHECK(diagnosticCount(exitForInDo, "invalid-exit") == 0);
    CHECK(exitForInDo.diagnostics.empty());
    ParseResult contForInDo = parseDocument("do\n  continue for\nloop\n");
    CHECK(diagnosticCount(contForInDo, "invalid-continue") == 0);
    CHECK(contForInDo.diagnostics.empty());
    ParseResult exitForModule = parseDocument("exit for\n");
    CHECK(diagnosticCount(exitForModule, "invalid-exit") == 0);
    CHECK(exitForModule.diagnostics.empty());
    ParseResult contForModule = parseDocument("continue for\n");
    CHECK(diagnosticCount(contForModule, "invalid-continue") == 0);
    CHECK(contForModule.diagnostics.empty());
    ParseResult fnInSub = parseDocument("sub s()\n  exit function\n"
                                        "end sub\n");
    CHECK(diagnosticCount(fnInSub, "invalid-exit") == 0);
    CHECK(fnInSub.diagnostics.empty());
    ParseResult subInFn = parseDocument("function f() as integer\n"
                                        "  exit sub\n"
                                        "  return 0\n"
                                        "end function\n");
    CHECK(diagnosticCount(subInFn, "invalid-exit") == 0);
    CHECK(subInFn.diagnostics.empty());
    ParseResult subAtModule = parseDocument("exit sub\n");
    CHECK(diagnosticCount(subAtModule, "invalid-exit") == 0);
    CHECK(subAtModule.diagnostics.empty());
    // Multi-target loop forms (probed: `exit do, for` is 244, `exit for,
    // do` is 243, `exit for, for` over two Fors compiles, a trailing comma
    // is 243) — the first target is accepted, so none of them is 251.
    ParseResult doFor = parseDocument("dim i as integer\n"
                                      "for i = 1 to 2\n"
                                      "  do\n"
                                      "    exit do, for\n"
                                      "  loop\n"
                                      "next\n");
    CHECK(diagnosticCount(doFor, "invalid-exit") == 0);
    CHECK(doFor.diagnostics.empty());
    ParseResult forDo = parseDocument("dim i as integer\n"
                                      "for i = 1 to 2\n"
                                      "  do\n"
                                      "    exit for, do\n"
                                      "  loop\n"
                                      "next\n");
    CHECK(diagnosticCount(forDo, "invalid-exit") == 0);
    CHECK(forDo.diagnostics.empty());
    ParseResult forFor = parseDocument("dim i as integer\n"
                                       "dim j as integer\n"
                                       "for i = 1 to 2\n"
                                       "  for j = 1 to 2\n"
                                       "    exit for, for\n"
                                       "  next\n"
                                       "next\n");
    CHECK(forFor.diagnostics.empty());
    ParseResult trailing =
        parseDocument("dim i as integer\nfor i = 1 to 2\n  exit for,\n"
                      "next\n");
    CHECK(diagnosticCount(trailing, "invalid-exit") == 0);
    // A procedure target has no multi-target form at all (probed: `exit
    // sub, sub` is fbc error 3, G5's); the first target is accepted, so
    // no 251 here either.
    ParseResult subSub = parseDocument("sub s()\n  exit sub, sub\nend sub\n");
    CHECK(diagnosticCount(subSub, "invalid-exit") == 0);
    CHECK(subSub.diagnostics.empty());
  }

  // The pre-Case region: an `Exit` statement there is 62 alone (fbc errors
  // and skips it before the Exit questions, probed), so 251 stands down;
  // a `Continue` never draws 62 (its handler does not ask the region
  // question), so 252 fires there as anywhere else (probed).
  {
    ParseResult bareExit = parseDocument("dim q as integer = 1\n"
                                         "select case q\n"
                                         "  exit\n"
                                         "case 1\n"
                                         "  print \"x\"\n"
                                         "end select\n");
    CHECK(diagnosticCount(bareExit, "illegal-inside-select") == 1);
    CHECK(diagnosticCount(bareExit, "invalid-exit") == 0);
    ParseResult badExit = parseDocument("dim q as integer = 1\n"
                                        "select case q\n"
                                        "  exit foo\n"
                                        "case 1\n"
                                        "  print \"x\"\n"
                                        "end select\n");
    CHECK(diagnosticCount(badExit, "illegal-inside-select") == 1);
    CHECK(diagnosticCount(badExit, "invalid-exit") == 0);
    CHECK(badExit.diagnostics.size() == 1);
    ParseResult bareCont = parseDocument("dim q as integer = 1\n"
                                         "select case q\n"
                                         "  continue\n"
                                         "case 1\n"
                                         "  print \"x\"\n"
                                         "end select\n");
    CHECK(diagnosticCount(bareCont, "illegal-inside-select") == 0);
    CHECK(diagnosticCount(bareCont, "invalid-continue") == 1);
    CHECK(bareCont.diagnostics.size() == 1);
    ParseResult badCont = parseDocument("dim q as integer = 1\n"
                                        "select case q\n"
                                        "  continue foo\n"
                                        "case 1\n"
                                        "  print \"x\"\n"
                                        "end select\n");
    CHECK(diagnosticCount(badCont, "illegal-inside-select") == 0);
    CHECK(diagnosticCount(badCont, "invalid-continue") == 1);
    CHECK(badCont.diagnostics.size() == 1);
  }
}

// M23 G4: placement diagnostics (61 / 96) and branch-crossing diagnostics
// (105 / warning 14).
static void TestPlacementAndBranchCrossing() {
  // 61 ILLEGALINSIDEASUB: declarations that cannot appear inside a procedure.
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  declare sub g()\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 1);
    CHECK(fbcErrorFor(r, "illegal-inside-sub") == FbcDiagId::IllegalInsideSub);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  namespace N\n"
                                        "  end namespace\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 1);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  extern x as integer\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 1);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  extern \"C\"\n"
                                        "  end extern\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 1);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  common x as integer\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 1);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  public:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 1);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  private:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 1);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  sub g()\n"
                                        "  end sub\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 1);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  function g() as integer\n"
                                        "  end function\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 1);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  property P() as integer\n"
                                        "  end property\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 1);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  operator +(a as integer, b as "
                                        "integer) as integer\n"
                                        "  end operator\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 1);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  destructor D()\n"
                                        "  end destructor\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 1);
  }
  // `constructor` inside a Sub is error 165, outside G4; leave it silent here.
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  constructor C()\n"
                                        "  end constructor\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 0);
  }

  // 96 ILLEGALINSIDEASCOPE: the same declarations inside a scope/control block
  // at module level.
  {
    ParseResult const r = parseDocument("scope\n"
                                        "  declare sub g()\n"
                                        "end scope\n");
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 1);
    CHECK(fbcErrorFor(r, "illegal-inside-scope") ==
          FbcDiagId::IllegalInsideScope);
  }
  {
    ParseResult const r = parseDocument("if 1 then\n"
                                        "  namespace N\n"
                                        "  end namespace\n"
                                        "end if\n");
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 1);
  }
  {
    ParseResult const r = parseDocument("scope\n"
                                        "  common x as integer\n"
                                        "end scope\n");
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 1);
  }
  {
    ParseResult const r = parseDocument("scope\n"
                                        "  public:\n"
                                        "end scope\n");
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 1);
  }
  {
    ParseResult const r = parseDocument("scope\n"
                                        "  sub g()\n"
                                        "  end sub\n"
                                        "end scope\n");
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 1);
  }
  {
    ParseResult const r = parseDocument("scope\n"
                                        "  constructor C()\n"
                                        "  end constructor\n"
                                        "end scope\n");
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 1);
  }
  // But `type` and `dim` are allowed inside scope blocks.
  {
    ParseResult const r = parseDocument("scope\n"
                                        "  type T\n"
                                        "    x as integer\n"
                                        "  end type\n"
                                        "  dim x as integer\n"
                                        "end scope\n");
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 0);
  }

  // 96 inside an `Extern "C"` block at module level: executable/control/redim.
  {
    ParseResult const r = parseDocument("extern \"C\"\n"
                                        "  redim x(1) as integer\n"
                                        "end extern\n");
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 1);
  }
  {
    ParseResult const r = parseDocument("extern \"C\"\n"
                                        "  if 1 then\n"
                                        "  end if\n"
                                        "end extern\n");
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 1);
  }
  {
    ParseResult const r = parseDocument("extern \"C\"\n"
                                        "  goto L\n"
                                        "  L:\n"
                                        "end extern\n");
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 2);
  }
  // Declarations inside Extern are allowed.
  {
    ParseResult const r = parseDocument("extern \"C\"\n"
                                        "  declare sub c_func()\n"
                                        "  dim x as integer\n"
                                        "end extern\n");
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 0);
  }

  // Branch crossing: error 105 for local arrays.
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  goto L\n"
                                        "  dim a(10) as integer\n"
                                        "  L:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 1);
    CHECK(fbcErrorFor(r, "branch-crossing-dyn-data-def") ==
          FbcDiagId::BranchCrossingDynDataDef);
  }
  {
    ParseResult const r = parseDocument("sub f(n as integer)\n"
                                        "  goto L\n"
                                        "  dim a(1 to n) as integer\n"
                                        "  L:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 1);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  goto L\n"
                                        "  redim a(1) as integer\n"
                                        "  L:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 1);
  }

  // Branch crossing: error 105 for var-len strings.
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  goto L\n"
                                        "  dim s as string\n"
                                        "  L:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 1);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  goto L\n"
                                        "  redim s(10) as string\n"
                                        "  L:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 1);
  }
  // Fixed-length strings are not dynamic data.
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  goto L\n"
                                        "  dim s as string * 10\n"
                                        "  L:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 0);
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 1);
  }

  // Branch crossing: error 105 for UDT instances whose type has a constructor.
  {
    ParseResult const r = parseDocument("type T\n"
                                        "  x as integer\n"
                                        "  declare constructor()\n"
                                        "end type\n"
                                        "sub f()\n"
                                        "  goto L\n"
                                        "  dim u as T\n"
                                        "  L:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 1);
  }
  // UDT without a constructor is only warning 14.
  {
    ParseResult const r = parseDocument("type T\n"
                                        "  x as integer\n"
                                        "end type\n"
                                        "sub f()\n"
                                        "  goto L\n"
                                        "  dim u as T\n"
                                        "  L:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 0);
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 1);
    CHECK(fbcErrorFor(r, "branch-crossing-local-var") ==
          FbcDiagId::BranchCrossingLocalVar);
  }

  // Branch crossing: warning 14 for plain locals and `Var`.
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  goto L\n"
                                        "  dim x as integer\n"
                                        "  L:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 1);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  goto L\n"
                                        "  var x = 1\n"
                                        "  L:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 1);
  }

  // Branch crossing: silent cases.
  {
    // Backward jump.
    ParseResult const r = parseDocument("sub f()\n"
                                        "  L:\n"
                                        "  dim x as integer\n"
                                        "  goto L\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 0);
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 0);
  }
  {
    // Static local, Shared, Common, Const, and loop counter are excluded.
    ParseResult const r = parseDocument("sub f()\n"
                                        "  goto L\n"
                                        "  static x as integer\n"
                                        "  dim shared y as integer\n"
                                        "  common z as integer\n"
                                        "  const c = 1\n"
                                        "  for i as integer = 1 to 2\n"
                                        "  next\n"
                                        "  L:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 0);
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 0);
  }
  {
    // Jump that crosses no definition.
    ParseResult const r = parseDocument("sub f()\n"
                                        "  goto L\n"
                                        "  L:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 0);
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 0);
  }

  // Module-level implicit main also participates in branch crossing.
  {
    ParseResult const r = parseDocument("goto L\n"
                                        "dim x as integer\n"
                                        "L:\n");
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 1);
  }
  {
    ParseResult const r = parseDocument("goto L\n"
                                        "dim shared x as integer\n"
                                        "L:\n");
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 0);
  }

  // Multiple crossed definitions: one diagnostic per definition.
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  goto L\n"
                                        "  dim a(10) as integer\n"
                                        "  dim b as string\n"
                                        "  L:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 2);
  }

  // 61 takes precedence over 96: a declaration inside a Scope inside a
  // procedure draws 61 alone, never both (probed).
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  scope\n"
                                        "  declare sub g()\n"
                                        "  end scope\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 1);
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 0);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  if 1 then\n"
                                        "  common x as integer\n"
                                        "  end if\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 1);
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 0);
  }

  // Overlap pins: shapes owned by landed families keep their numbers and
  // draw no placement diagnostic (probed with fbc 1.10.3).
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  next\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 0);
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 0);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  case 1\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 0);
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 0);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  exit foo\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 0);
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 0);
    CHECK(diagnosticCount(r, "invalid-exit") == 1);
  }
  {
    ParseResult const r = parseDocument("scope\n"
                                        "  exit foo\n"
                                        "end scope\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 0);
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 0);
    CHECK(diagnosticCount(r, "invalid-exit") == 1);
  }
  {
    ParseResult const r = parseDocument("scope\n"
                                        "  continue foo\n"
                                        "end scope\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 0);
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 0);
    CHECK(diagnosticCount(r, "invalid-continue") == 1);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  exit select\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 0);
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 0);
    CHECK(diagnosticCount(r, "illegal-outside-select") == 1);
  }
  {
    ParseResult const r = parseDocument("extern \"C\"\n"
                                        "  exit foo\n"
                                        "end extern\n");
    CHECK(diagnosticCount(r, "illegal-inside-sub") == 0);
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 0);
    CHECK(diagnosticCount(r, "invalid-exit") == 1);
  }
  {
    ParseResult const r = parseDocument("extern \"C\"\n"
                                        "  else\n"
                                        "end extern\n");
    CHECK(diagnosticCount(r, "illegal-inside-scope") == 0);
    CHECK(diagnosticCount(r, "stray-closer") == 1);
  }

  // Legal jumps (probed): jumping out of or over a block, between sibling
  // scopes, and across `if`/`case` arms is silent; landing inside the
  // definition's block past it counts.
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  scope\n"
                                        "  goto L\n"
                                        "  dim x as integer\n"
                                        "  end scope\n"
                                        "  L:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 0);
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 0);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  goto L\n"
                                        "  scope\n"
                                        "  dim x as integer\n"
                                        "  end scope\n"
                                        "  L:\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 0);
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 0);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  goto L\n"
                                        "  scope\n"
                                        "  dim x as integer\n"
                                        "  L:\n"
                                        "  end scope\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 1);
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 0);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  goto L\n"
                                        "  scope\n"
                                        "  dim x as integer\n"
                                        "  end scope\n"
                                        "  scope\n"
                                        "  L:\n"
                                        "  end scope\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 0);
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 0);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  goto L\n"
                                        "  if 1 then\n"
                                        "  dim x as integer\n"
                                        "  else\n"
                                        "  L:\n"
                                        "  end if\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 0);
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 0);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  select case 1\n"
                                        "  case 1\n"
                                        "  goto L\n"
                                        "  dim x as integer\n"
                                        "  L:\n"
                                        "  case 2\n"
                                        "  end select\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 1);
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 0);
  }
  {
    ParseResult const r = parseDocument("sub f()\n"
                                        "  goto L\n"
                                        "  select case 1\n"
                                        "  case 1\n"
                                        "  dim x as integer\n"
                                        "  case 2\n"
                                        "  L:\n"
                                        "  end select\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 0);
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 0);
  }
  {
    // Record fields are members, not locals: a type body between the branch
    // and the label draws nothing (probed).
    ParseResult const r = parseDocument("goto L\n"
                                        "type T\n"
                                        "x as integer\n"
                                        "end type\n"
                                        "L:\n");
    CHECK(diagnosticCount(r, "branch-crossing-local-var") == 0);
    CHECK(diagnosticCount(r, "branch-crossing-dyn-data-def") == 0);
  }
}

// M23 G5: the punctuator family (17/14/9/6/7/3/10/16/66/309/11/68/67/289).
static void TestPunctuatorFamily() {
  // Pin: pre-existing 14 site — a reserved word that is never a field name
  // (skipStatement member capture). Behavior unchanged by the helper.
  {
    ParseResult const r = parseDocument("type T\n"
                                        "  as integer and\n"
                                        "end type\n");
    CHECK(diagnosticCount(r, "invalid-member-name") == 1);
    CHECK(fbcErrorFor(r, "invalid-member-name") ==
          FbcDiagId::ExpectedIdentifier);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].severity == Severity::Error);
      CHECK(r.diagnostics[0].fbcKind == FbcMessageKind::Error);
      CHECK(r.diagnostics[0].message ==
            "'and' is a reserved word and cannot be a field name");
    }
  }
  // Pin: pre-existing 14 site in handleVarDecls (Dim record field).
  {
    ParseResult const r = parseDocument("type T\n"
                                        "  dim and as integer\n"
                                        "end type\n");
    CHECK(diagnosticCount(r, "invalid-member-name") == 1);
    CHECK(fbcErrorFor(r, "invalid-member-name") ==
          FbcDiagId::ExpectedIdentifier);
  }
  // Pin: pre-existing 17 site — Export on a module-constructor body.
  {
    ParseResult const r = parseDocument("sub f() constructor export\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "module-ctor-export") == 1);
    CHECK(fbcErrorFor(r, "module-ctor-export") == FbcDiagId::SyntaxError);
    CHECK(r.diagnostics.size() == 1);
  }
  // Pin: pre-existing 3 site — tail after a module-constructor body.
  {
    ParseResult const r = parseDocument("sub f() constructor foo\n"
                                        "end sub\n");
    CHECK(diagnosticCount(r, "module-ctor-tail") == 1);
    CHECK(fbcErrorFor(r, "module-ctor-tail") == FbcDiagId::ExpectedEol);
    CHECK(r.diagnostics.size() == 1);
  }
  // Pin: pre-existing 3 site — Constructor/Export on a Declare line.
  {
    ParseResult const r = parseDocument("declare sub f() constructor\n");
    CHECK(diagnosticCount(r, "decl-decorator") == 1);
    CHECK(fbcErrorFor(r, "decl-decorator") == FbcDiagId::ExpectedEol);
  }
  // Pin: pre-existing 17 site — stray #endmacro.
  {
    ParseResult const r = parseDocument("#endmacro\n");
    CHECK(diagnosticCount(r, "stray-closer") == 1);
    CHECK(fbcErrorFor(r, "stray-closer") == FbcDiagId::SyntaxError);
    CHECK(r.diagnostics.size() == 1);
  }
  // Pin: pre-existing 3 site — reserved word as enum member (boundary).
  {
    ParseResult const r = parseDocument("enum E\n"
                                        "  dim\n"
                                        "end enum\n");
    CHECK(diagnosticCount(r, "invalid-member-name") == 1);
    CHECK(fbcErrorFor(r, "invalid-member-name") == FbcDiagId::ExpectedEol);
  }
  // 7 ExpectedRprnt: unclosed parameter lists and bounds (probed).
  {
    ParseResult const r = parseDocument("sub f(a as integer\n"
                                        "end sub\n"
                                        "print \"ok\"\n");
    CHECK(fbcErrorFor(r, "expected-rprnt") == FbcDiagId::ExpectedRprnt);
    CHECK(diagnosticCount(r, "expected-rprnt") == 1);
    CHECK(diagnosticCount(r, "unterminated-block") == 0);
    CHECK(diagnosticCount(r, "stray-closer") == 0);
    ParseResult const redim = parseDocument("dim a(10) as integer\n"
                                            "redim a(5\n"
                                            "print \"ok\"\n");
    CHECK(fbcErrorFor(redim, "expected-rprnt") == FbcDiagId::ExpectedRprnt);
    CHECK(diagnosticCount(redim, "expected-rprnt") == 1);
  }
  // 10 ExpectedEq: Const with no `=`; For counter with no `=` (probed).
  {
    ParseResult const r = parseDocument("const C\n"
                                        "print \"ok\"\n");
    CHECK(fbcErrorFor(r, "expected-eq") == FbcDiagId::ExpectedEq);
    CHECK(diagnosticCount(r, "expected-eq") == 1);
    CHECK(r.diagnostics.size() == 1);
    ParseResult const c5 = parseDocument("const C 5\n");
    CHECK(fbcErrorFor(c5, "expected-eq") == FbcDiagId::ExpectedEq);
    CHECK(c5.diagnostics.size() == 1);
    ParseResult const f = parseDocument("dim i as integer\n"
                                        "for i 1 to 10\n"
                                        "next\n");
    CHECK(fbcErrorFor(f, "expected-eq") == FbcDiagId::ExpectedEq);
    CHECK(diagnosticCount(f, "unterminated-block") == 0);
    // Silence: valid forms.
    ParseResult const ok = parseDocument("const C = 5\n"
                                         "dim i as integer\n"
                                         "for i = 1 to 10 step 2\n"
                                         "next\n");
    CHECK(ok.diagnostics.empty());
  }
  // 11 ExpectedConst: `=` with no constant on a Const line (probed, the
  // name rides in the text) and on an enum member (no name in the text).
  {
    ParseResult const r = parseDocument("const C =\n"
                                        "print \"ok\"\n");
    CHECK(fbcErrorFor(r, "expected-const") == FbcDiagId::ExpectedConst);
    CHECK(diagnosticCount(r, "expected-const") == 1);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].message == "Expected constant, C");
    }
    ParseResult const multi = parseDocument("const C = 1, D =\n"
                                            "print \"ok\"\n");
    CHECK(diagnosticCount(multi, "expected-const") == 1);
    if (multi.diagnostics.size() == 1) {
      CHECK(multi.diagnostics[0].message == "Expected constant, D");
    }
    ParseResult const e = parseDocument("enum E\n"
                                        "  a = 1, b =\n"
                                        "end enum\n");
    CHECK(diagnosticCount(e, "expected-const") == 1);
    CHECK(fbcErrorFor(e, "expected-const") == FbcDiagId::ExpectedConst);
  }
  // 9 ExpectedExpression: `Dim x =` with nothing after, `x =` bare, and a
  // bare Case (probed).
  {
    ParseResult const r = parseDocument("dim x as integer =\n");
    CHECK(fbcErrorFor(r, "expected-expression") ==
          FbcDiagId::ExpectedExpression);
    CHECK(r.diagnostics.size() == 1);
    ParseResult const assign = parseDocument("dim x as integer\n"
                                             "x =\n"
                                             "print \"ok\"\n");
    CHECK(fbcErrorFor(assign, "expected-expression") ==
          FbcDiagId::ExpectedExpression);
    CHECK(diagnosticCount(assign, "unterminated-block") == 0);
    ParseResult const caseR = parseDocument("select case 1\n"
                                            "case\n"
                                            "end select\n");
    CHECK(fbcErrorFor(caseR, "expected-expression") ==
          FbcDiagId::ExpectedExpression);
    CHECK(diagnosticCount(caseR, "expected-expression") == 1);
  }
  // 14 ExpectedIdentifier: bare Dim / Dim As Integer / nameless procedures /
  // Declare Sub with no name / bare Goto (probed).
  {
    ParseResult const dim = parseDocument("dim\n"
                                          "print \"ok\"\n");
    CHECK(fbcErrorFor(dim, "expected-identifier") ==
          FbcDiagId::ExpectedIdentifier);
    CHECK(diagnosticCount(dim, "unterminated-block") == 0);
    ParseResult const asDim = parseDocument("dim as integer\n");
    CHECK(fbcErrorFor(asDim, "expected-identifier") ==
          FbcDiagId::ExpectedIdentifier);
    ParseResult const sub = parseDocument("sub()\n"
                                          "end sub\n");
    CHECK(fbcErrorFor(sub, "expected-identifier") ==
          FbcDiagId::ExpectedIdentifier);
    CHECK(diagnosticCount(sub, "unterminated-block") == 0);
    CHECK(diagnosticCount(sub, "stray-closer") == 0);
    ParseResult const decl = parseDocument("declare sub\n");
    CHECK(fbcErrorFor(decl, "expected-identifier") ==
          FbcDiagId::ExpectedIdentifier);
    ParseResult const gotoR = parseDocument("goto\n");
    CHECK(fbcErrorFor(gotoR, "expected-identifier") ==
          FbcDiagId::ExpectedIdentifier);
    // Silence: type-first and multi-name forms, member implementations.
    ParseResult const ok = parseDocument("namespace gfx\n"
                                         "  type sprite\n"
                                         "    w as integer\n"
                                         "  end type\n"
                                         "end namespace\n"
                                         "dim as gfx.sprite ptr firstImage\n"
                                         "dim as integer pp, qq\n"
                                         "dim rr, ss\n");
    CHECK(ok.diagnostics.empty());
  }
  // 17 SyntaxError fallback: a bare Declare and a Declare whose next word is
  // not a procedure kind (probed: Integer/Bar/String all draw 17, not 289).
  {
    ParseResult const bare = parseDocument("declare\n"
                                           "print \"ok\"\n");
    CHECK(fbcErrorFor(bare, "syntax-error") == FbcDiagId::SyntaxError);
    CHECK(diagnosticCount(bare, "syntax-error") == 1);
    ParseResult const bad = parseDocument("declare integer\n");
    CHECK(fbcErrorFor(bad, "syntax-error") == FbcDiagId::SyntaxError);
    CHECK(bad.diagnostics.size() == 1);
    if (bad.diagnostics.size() == 1) {
      CHECK(bad.diagnostics[0].message == "Syntax error, found 'integer'");
    }
    ParseResult const bar = parseDocument("declare bar\n");
    CHECK(fbcErrorFor(bar, "syntax-error") == FbcDiagId::SyntaxError);
  }
  // 309 ExpectedGt: `Integer<...>` with no `>` (probed); undeclared-type
  // bounds stay silent.
  {
    ParseResult const r = parseDocument("dim x as integer<16\n"
                                        "print \"ok\"\n");
    CHECK(fbcErrorFor(r, "expected-gt") == FbcDiagId::ExpectedGt);
    CHECK(diagnosticCount(r, "expected-gt") == 1);
    CHECK(r.diagnostics.size() == 1);
    ParseResult const ok = parseDocument("dim x as integer<16> = 5\n"
                                         "dim y as uinteger<8>\n");
    CHECK(ok.diagnostics.empty());
  }
  // 66 ExpectedRBracket: a New vector without `]` (probed); pointer
  // indexing without `new` is 17's, not this family's, and stays silent
  // here.
  {
    ParseResult const r = parseDocument("dim p as integer ptr\n"
                                        "p = new integer[4\n"
                                        "print \"ok\"\n");
    CHECK(fbcErrorFor(r, "expected-rbracket") == FbcDiagId::ExpectedRBracket);
    CHECK(diagnosticCount(r, "expected-rbracket") == 1);
    ParseResult const ok = parseDocument("dim p as integer ptr\n"
                                         "p = new integer[4]\n"
                                         "delete p\n");
    CHECK(ok.diagnostics.empty());
  }
  // 68 ExpectedRestype: Operator with no `As`; function pointer with no `As`
  // (probed).
  {
    ParseResult const op = parseDocument("operator +(a as integer)\n"
                                         "end operator\n");
    CHECK(fbcErrorFor(op, "expected-res-type") == FbcDiagId::ExpectedRestype);
    CHECK(diagnosticCount(op, "unterminated-block") == 0);
    ParseResult const ptr = parseDocument("dim f as function()\n");
    CHECK(fbcErrorFor(ptr, "expected-res-type") == FbcDiagId::ExpectedRestype);
    ParseResult const ok = parseDocument("dim f as function() as integer\n"
                                         "operator +(a as integer) as integer\n"
                                         "end operator\n");
    (void)ok;
    ParseResult const ok2 = parseDocument("dim f as function() as integer\n");
    CHECK(ok2.diagnostics.empty());
  }
  // 67 TooManyExpressions: UDT paren-initializer with more elements than
  // fields (probed); an undeclared type stays silent.
  {
    ParseResult const r = parseDocument("type T\n"
                                        "  x as integer\n"
                                        "end type\n"
                                        "dim q as T = (1, 2)\n"
                                        "print \"ok\"\n");
    CHECK(fbcErrorFor(r, "too-many-expressions") ==
          FbcDiagId::TooManyExpressions);
    CHECK(diagnosticCount(r, "too-many-expressions") == 1);
    CHECK(r.diagnostics.size() == 1);
    ParseResult const ok = parseDocument("type T\n"
                                         "  x as integer\n"
                                         "  y as integer\n"
                                         "end type\n"
                                         "dim q as T = (1, 2)\n");
    CHECK(ok.diagnostics.empty());
  }
  // 289 ExpectedSub: ThreadCall naming an in-file Function (probed);
  // a Sub is silent, an unknown name is the resolve layer's.
  {
    ParseResult const r = parseDocument("function foo() as integer\n"
                                        "  foo = 1\n"
                                        "end function\n"
                                        "sub bar()\n"
                                        "  threadcall foo()\n"
                                        "end sub\n");
    CHECK(fbcErrorFor(r, "expected-sub") == FbcDiagId::ExpectedSub);
    CHECK(diagnosticCount(r, "expected-sub") == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].message == "Expected sub, found 'foo'");
    }
    ParseResult const sub = parseDocument("sub foo()\n"
                                          "end sub\n"
                                          "sub bar()\n"
                                          "  threadcall foo()\n"
                                          "end sub\n");
    CHECK(sub.diagnostics.empty());
    ParseResult const unknown = parseDocument("sub bar()\n"
                                              "  threadcall nosuch()\n"
                                              "end sub\n");
    CHECK(diagnosticCount(unknown, "expected-sub") == 0);
  }
  // Enum members: `a =` is 11 (above) and a bare member stays silent. An
  // `a 5` line keeps the pre-existing boundary pair (the body closes on it),
  // where fbc answers 3 — recorded as a limit in FreeBASIC.md §12, not fixed
  // here.
  {
    ParseResult const ok = parseDocument("enum E\n"
                                         "  a\n"
                                         "  b = 5\n"
                                         "end enum\n");
    CHECK(ok.diagnostics.empty());
  }
  // No-cascade: one broken statement draws exactly one family diagnostic,
  // and the lines after it parse clean (no phantom structure errors).
  {
    ParseResult const r = parseDocument("const C\n"
                                        "const D\n"
                                        "dim x as integer = 1\n"
                                        "print \"ok\"\n");
    CHECK(diagnosticCount(r, "expected-eq") == 2);
    CHECK(diagnosticCount(r, "unterminated-block") == 0);
    CHECK(diagnosticCount(r, "stray-closer") == 0);
    // A failed expectation inside a procedure does not leak block state:
    // the closer still matches.
    ParseResult const inProc = parseDocument("sub f()\n"
                                             "  dim x as integer =\n"
                                             "end sub\n"
                                             "print \"ok\"\n");
    CHECK(diagnosticCount(inProc, "expected-expression") == 1);
    CHECK(inProc.diagnostics.size() == 1);
    CHECK(diagnosticCount(inProc, "unterminated-block") == 0);
    // A colon-separated second statement is still parsed.
    ParseResult const colon = parseDocument("const C: dim x as integer\n");
    CHECK(diagnosticCount(colon, "expected-eq") == 1);
    CHECK(find(colon.roots, "x", SymbolKind::Dim) != nullptr);
  }
}

int main() {
  // A realistic multi-construct program must parse with zero diagnostics.
  {
    const std::string src = "' FreeBASIC program\n"
                            "#include once \"fbgfx.bi\"\n"
                            "\n"
                            "'' Draws something\n"
                            "function clamp(byval v as single, lo as single, "
                            "hi as single) as single\n"
                            "    if v < lo then\n"
                            "        return lo\n"
                            "    end if\n"
                            "    return v\n"
                            "end function\n"
                            "\n"
                            "type vec2\n"
                            "    x as single\n"
                            "    y as single\n"
                            "end type\n"
                            "\n"
                            "enum keys\n"
                            "    key_esc = 1\n"
                            "    key_space = 2\n"
                            "end enum\n"
                            "\n"
                            "dim as vec2 p\n"
                            "dim i as integer\n"
                            "\n"
                            "namespace app\n"
                            "    sub greet()\n"
                            "        dim s as string = \"hello\"\n"
                            "        print s\n"
                            "    end sub\n"
                            "end namespace\n"
                            "\n"
                            "for i = 1 to 10\n"
                            "    select case i\n"
                            "    case 1\n"
                            "        print \"one\"\n"
                            "    case else\n"
                            "        print i\n"
                            "    end select\n"
                            "next i\n"
                            "\n"
                            "app.greet()\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "next-var-meaningless") == 1);
    CHECK(diagnosticCount(r, "for-next-variable-mismatch") == 0);
    CHECK(diagnosticCount(r, "unterminated-block") == 0);
    CHECK(diagnosticCount(r, "stray-closer") == 0);
    CHECK(r.lang == "fb");
    CHECK(r.roots.size() == 7); // clamp, vec2, keys, p, i, app, for-scope

    const Symbol *clamp = find(r.roots, "clamp", SymbolKind::Function);
    CHECK(clamp != nullptr);
    // three parameters plus the IF block's declaration scope inside the body
    CHECK(clamp->children.size() == 4);
    CHECK(clamp->children[0].kind == SymbolKind::Parameter);
    CHECK(clamp->children[0].name == "v");
    CHECK(clamp->children[3].kind == SymbolKind::Scope);
    CHECK(clamp->doc == " Draws something");

    const Symbol *vec2 = find(r.roots, "vec2", SymbolKind::Type);
    CHECK(vec2 != nullptr);
    CHECK(vec2->children.size() == 2);
    CHECK(vec2->children[0].name == "x");

    const Symbol *keys = find(r.roots, "keys", SymbolKind::Enum);
    CHECK(keys != nullptr);
    CHECK(keys->children.size() == 2);
    CHECK(keys->children[0].name == "key_esc");
    CHECK(keys->children[0].kind == SymbolKind::Const);

    CHECK(find(r.roots, "p", SymbolKind::Dim) != nullptr);
    CHECK(find(r.roots, "i", SymbolKind::Dim) != nullptr);

    const Symbol *app = find(r.roots, "app", SymbolKind::Namespace);
    CHECK(app != nullptr);
    CHECK(app->children.size() == 1);
    CHECK(app->children[0].name == "greet");
    CHECK(app->children[0].kind == SymbolKind::Sub);
    CHECK(app->children[0].children.size() == 1);
    CHECK(app->children[0].children[0].name == "s");

    // The module-level FOR block became a declaration-scope root (structure,
    // not a symbol: no key, so it never indexes or resolves).
    const Symbol *forScope = nullptr;
    for (const Symbol &root : r.roots) {
      if (root.kind == SymbolKind::Scope && root.name == "for") {
        forScope = &root;
      }
    }
    CHECK(forScope != nullptr);
    CHECK(forScope->key.empty());
    CHECK(forScope->children.size() == 1); // the SELECT block's scope
    CHECK(forScope->children[0].kind == SymbolKind::Scope);
    CHECK(forScope->children[0].name == "select");
    CHECK(forScope->children[0].key.empty());
  }

  // For-loop counters declared with `as` become loop-local Dims (fbc ground
  // truth: invisible after `next`). A header without `as` reuses an existing
  // variable (undeclared is error 42 in fbc, no auto-declare), so nothing is
  // registered for it.
  {
    ParseResult r = parseDocument("sub foo()\n"
                                  "    for i as integer = 0 to 3\n"
                                  "        print i\n"
                                  "    next i\n"
                                  "    for j = 1 to 5\n"
                                  "        print j\n"
                                  "    next j\n"
                                  "    for i as single = 0.5 to 2.5\n"
                                  "    next i\n"
                                  "end sub\n");
    CHECK(diagnosticCount(r, "next-var-meaningless") == 2);
    CHECK(diagnosticCount(r, "for-next-variable-mismatch") == 0);
    CHECK(diagnosticCount(r, "unterminated-block") == 0);
    CHECK(diagnosticCount(r, "stray-closer") == 0);
    const Symbol *foo = find(r.roots, "foo", SymbolKind::Sub);
    CHECK(foo != nullptr);
    // Three FOR declaration scopes: `as`-typed counter, bare reuse, `as`-typed
    // counter with the same name as the first (own container -> no dup).
    std::vector<const Symbol *> scopes;
    for (const Symbol &c : foo->children) {
      if (c.kind == SymbolKind::Scope && c.name == "for") {
        scopes.push_back(&c);
      }
    }
    CHECK(scopes.size() == 3);
    CHECK(scopes[0]->children.size() == 1);
    CHECK(scopes[0]->children[0].name == "i");
    CHECK(scopes[0]->children[0].kind == SymbolKind::Dim);
    CHECK(scopes[0]->children[0].loopVar);
    CHECK(scopes[0]->children[0].signature == "for i as integer = 0 to 3");
    CHECK(scopes[0]->children[0].doc.empty());
    CHECK(scopes[1]->children.empty()); // `for j = ...` reuses, no declaration
    CHECK(scopes[2]->children.size() == 1);
    CHECK(scopes[2]->children[0].name == "i");
    CHECK(scopes[2]->children[0].loopVar);
    CHECK(scopes[2]->children[0].signature == "for i as single = 0.5 to 2.5");
  }

  // Nested `for ... as` counters shadow with their own container: no
  // duplicate-definition diagnostic.
  {
    ParseResult r = parseDocument("for i as integer = 0 to 2\n"
                                  "    for i as integer = 0 to 2\n"
                                  "    next i\n"
                                  "next i\n");
    CHECK(diagnosticCount(r, "next-var-meaningless") == 2);
    CHECK(diagnosticCount(r, "for-next-variable-mismatch") == 0);
    CHECK(diagnosticCount(r, "unterminated-block") == 0);
    CHECK(diagnosticCount(r, "stray-closer") == 0);
    const Symbol *outer = nullptr;
    for (const Symbol &root : r.roots) {
      if (root.kind == SymbolKind::Scope && root.name == "for") {
        outer = &root;
      }
    }
    CHECK(outer != nullptr);
    CHECK(outer->children.size() == 2);
    CHECK(outer->children[0].name == "i");
    CHECK(outer->children[0].loopVar);
    CHECK(outer->children[1].kind == SymbolKind::Scope); // inner loop
    CHECK(outer->children[1].children.size() == 1);
    CHECK(outer->children[1].children[0].name == "i");
    CHECK(outer->children[1].children[0].loopVar);
  }

  // Type alias vs UDT vs one-line UDT.
  {
    ParseResult r = parseDocument("type pt\n"
                                  "x as integer\n"
                                  "end type\n"
                                  "type mybyte as byte\n"
                                  "type pt2 : a as double : end type\n");
    CHECK(r.diagnostics.empty());
    const Symbol *pt = find(r.roots, "pt", SymbolKind::Type);
    CHECK(pt != nullptr);
    CHECK(pt->children.size() == 1);
    CHECK(pt->children[0].name == "x");
    const Symbol *mb = find(r.roots, "mybyte", SymbolKind::Type);
    CHECK(mb != nullptr);
    CHECK(mb->children.empty());
    const Symbol *pt2 = find(r.roots, "pt2", SymbolKind::Type);
    CHECK(pt2 != nullptr);
    CHECK(pt2->children.size() == 1);
  }

  // Block matching diagnostics.
  {
    ParseResult r = parseDocument("sub foo()\nend sub\n");
    CHECK(r.diagnostics.empty());
  }
  {
    // Case-insensitive blocks, as fbc parses them: an all-caps structure is
    // clean and a closer matches its opener whatever the two spellings are.
    ParseResult r = parseDocument("SUB foo()\n  PRINT 1\nEND SUB\n");
    CHECK(r.diagnostics.empty());
    ParseResult mixed = parseDocument("Sub foo()\n  print 1\nend sUb\n");
    CHECK(mixed.diagnostics.empty());
    ParseResult blocks = parseDocument(
        "IF a THEN\n  PRINT 1\nEND IF\nFOR i = 1 TO 2\n  PRINT i\nNEXT\n");
    CHECK(blocks.diagnostics.empty());
  }
  {
    ParseResult r = parseDocument("sub foo()\n");
    CHECK(diagnosticCount(r, "unterminated-block") == 1);
  }
  {
    ParseResult r = parseDocument("end if\n");
    CHECK(diagnosticCount(r, "stray-closer") == 1);
  }
  {
    ParseResult r = parseDocument("for i = 1 to 3\nnext\nend for\n");
    CHECK(diagnosticCount(r, "invalid-end") == 1);
  }
  {
    ParseResult r = parseDocument("while 1\nwend\nend while\n");
    CHECK(diagnosticCount(r, "invalid-end") == 1);
  }
  {
    // END WHILE uses WEND, and END FOR uses NEXT; neither is ever legal.
    ParseResult r = parseDocument("do\nloop\nend while\n");
    CHECK(diagnosticCount(r, "invalid-end") == 1);
    CHECK(diagnosticCount(r, "stray-closer") == 0);
  }
  {
    ParseResult r = parseDocument("else\n");
    CHECK(diagnosticCount(r, "stray-closer") == 1);
  }
  {
    ParseResult r = parseDocument("case 1\n");
    CHECK(diagnosticCount(r, "stray-closer") == 1);
  }
  {
    // Mismatched closer: expected END IF, and IF stays nested.
    ParseResult r = parseDocument("if a then\nend select\n");
    CHECK(diagnosticCount(r, "closer-mismatch") == 1);
    CHECK(diagnosticCount(r, "unterminated-block") == 1);
  }

  // Each of the block-closer diagnostics carries fbc's own number for the
  // *specific* closer it names, so a reader who knows `error 125` learns
  // something from our output and the session can link the catalog's wiki
  // page. The numbers and spellings were re-probed against fbc 1.10.2:
  // `tools/fbc_catalog.tsv` is the imported copy this mapping is read against.
  {
    CHECK(fbcErrorFor(parseDocument("sub foo()\n"), "unterminated-block") ==
          FbcDiagId::ExpectedEndSub);
    CHECK(fbcErrorFor(parseDocument("function foo()\n"),
                      "unterminated-block") == FbcDiagId::ExpectedEndFunction);
    CHECK(fbcErrorFor(parseDocument("type t\n  as integer i\n"),
                      "unterminated-block") == FbcDiagId::ExpectedEndType);
    CHECK(fbcErrorFor(parseDocument("union u\n  as integer i\n"),
                      "unterminated-block") == FbcDiagId::ExpectedEndType);
    CHECK(fbcErrorFor(parseDocument("enum e\n  a = 1\n"),
                      "unterminated-block") == FbcDiagId::ExpectedEndEnum);
    CHECK(fbcErrorFor(parseDocument("namespace n\n  dim x as integer\n"),
                      "unterminated-block") == FbcDiagId::ExpectedEndNamespace);
    CHECK(fbcErrorFor(parseDocument("scope\n  dim x as integer\n"),
                      "unterminated-block") == FbcDiagId::ExpectedEndScope);
    CHECK(fbcErrorFor(parseDocument("extern \"C\"\n  declare sub foo()\n"),
                      "unterminated-block") == FbcDiagId::ExpectedEndExtern);
    CHECK(fbcErrorFor(parseDocument("for i = 1 to 3\n  print i\n"),
                      "unterminated-block") == FbcDiagId::ExpectedNext);
    CHECK(fbcErrorFor(parseDocument("while 1\n  print 1\n"),
                      "unterminated-block") == FbcDiagId::ExpectedWend);
    CHECK(fbcErrorFor(parseDocument("do\n  print 1\n"), "unterminated-block") ==
          FbcDiagId::ExpectedLoop);
    CHECK(fbcErrorFor(parseDocument("if a then\n  print 1\n"),
                      "unterminated-block") == FbcDiagId::ExpectedEndIf);
    CHECK(fbcErrorFor(parseDocument("select case a\n  case 1\n"),
                      "unterminated-block") == FbcDiagId::ExpectedEndSelect);
    CHECK(fbcErrorFor(parseDocument("type t\n  as integer i\nend type\nwith t\n"
                                    "  .i = 1\n"),
                      "unterminated-block") == FbcDiagId::ExpectedEndWith);
    CHECK(fbcErrorFor(parseDocument("asm\n  nop\n"), "unterminated-block") ==
          FbcDiagId::ExpectedEndAsm);
  }
  {
    // A closer with no opener: fbc names each one, and the record/union/enum
    // spellings collapse onto `IllegalEnd` because that is what fbc reports
    // for a bare `end type`/`end enum` (probed: "error 33: Illegal 'END'").
    CHECK(fbcErrorFor(parseDocument("next\n"), "stray-closer") ==
          FbcDiagId::NextWithoutFor);
    CHECK(fbcErrorFor(parseDocument("loop\n"), "stray-closer") ==
          FbcDiagId::LoopWithoutDo);
    CHECK(fbcErrorFor(parseDocument("wend\n"), "stray-closer") ==
          FbcDiagId::WendWithoutWhile);
    CHECK(fbcErrorFor(parseDocument("else\n"), "stray-closer") ==
          FbcDiagId::ElseWithoutIf);
    CHECK(fbcErrorFor(parseDocument("elseif 1 then\n"), "stray-closer") ==
          FbcDiagId::ElseIfWithoutIf);
    CHECK(fbcErrorFor(parseDocument("case 1\n"), "stray-closer") ==
          FbcDiagId::CaseWithoutSelect);
    CHECK(fbcErrorFor(parseDocument("end if\n"), "stray-closer") ==
          FbcDiagId::EndIfWithoutIf);
    CHECK(fbcErrorFor(parseDocument("end select\n"), "stray-closer") ==
          FbcDiagId::EndSelectWithoutSelect);
    CHECK(fbcErrorFor(parseDocument("end with\n"), "stray-closer") ==
          FbcDiagId::EndWithWithoutWith);
    CHECK(fbcErrorFor(parseDocument("end sub\n"), "stray-closer") ==
          FbcDiagId::EndSubWithoutSub);
    CHECK(fbcErrorFor(parseDocument("end namespace\n"), "stray-closer") ==
          FbcDiagId::EndNamespaceWithoutNamespace);
    CHECK(fbcErrorFor(parseDocument("end scope\n"), "stray-closer") ==
          FbcDiagId::EndScopeWithoutScope);
    CHECK(fbcErrorFor(parseDocument("end extern\n"), "stray-closer") ==
          FbcDiagId::EndExternWithoutExtern);
    CHECK(fbcErrorFor(parseDocument("end type\n"), "stray-closer") ==
          FbcDiagId::IllegalEnd);
    CHECK(fbcErrorFor(parseDocument("end enum\n"), "stray-closer") ==
          FbcDiagId::IllegalEnd);
  }
  {
    // `END FOR`/`END WHILE` are an illegal `END` (IllegalEnd), and the block
    // that the token proves is unterminated keeps its own expected-closer
    // identity.
    CHECK(fbcErrorFor(parseDocument("for i = 1 to 3\nnext\nend for\n"),
                      "invalid-end") == FbcDiagId::IllegalEnd);
    CHECK(fbcErrorFor(parseDocument("if a then\nend select\n"),
                      "closer-mismatch") == FbcDiagId::ExpectedEndIf);
    // The preprocessor closers carry the identities fbc names for them: a
    // stray #else/#endif is IllegalOutsideComp, a stray #endmacro is
    // SyntaxError, and an unterminated chain keeps its expected-closer
    // identity (ExpectedPpEndIf, ExpectedMacro) — all probed against 1.10.2
    // and present in tools/fbc_catalog.tsv.
    CHECK(fbcErrorFor(parseDocument("#endif\n"), "stray-closer") ==
          FbcDiagId::IllegalOutsideComp);
    CHECK(fbcErrorFor(parseDocument("#else\n"), "stray-closer") ==
          FbcDiagId::IllegalOutsideComp);
    CHECK(fbcErrorFor(parseDocument("#endmacro\n"), "stray-closer") ==
          FbcDiagId::SyntaxError);
    CHECK(fbcErrorFor(parseDocument("#if 1\nprint 1\n"),
                      "unterminated-block") == FbcDiagId::ExpectedPpEndIf);
    CHECK(fbcErrorFor(parseDocument("#macro m\nprint 1\n"),
                      "unterminated-block") == FbcDiagId::ExpectedMacro);
  }
  {
    // The diagnostics that predate the closer split now carry fbc's identity
    // for their specific trigger too, each re-probed against fbc 1.10.2: a
    // suffix on a keyword is SuffixIgnored, an unterminated string
    // NoClosingQuote, a duplicate definition DupDefinition, and a reserved
    // word as a member name is ExpectedIdentifier (a field),
    // KeywordFieldsNotAllowedInClasses (a field in a TYPE that has member
    // functions), or ExpectedEol (an enum member — fbc anchors its report on
    // the name, not a missing closer).
    CHECK(fbcErrorFor(parseDocument("print% 1\n"), "keyword-suffix") ==
          FbcDiagId::SuffixIgnored);
    CHECK(fbcErrorFor(parseDocument("print \"abc\n"), "unterminated-string") ==
          FbcDiagId::NoClosingQuote);
    CHECK(fbcErrorFor(parseDocument("dim x as integer\ndim x as integer\n"),
                      "duplicate-definition") == FbcDiagId::DupDefinition);
    CHECK(fbcErrorFor(parseDocument("type t\n  as integer and\nend type\n"),
                      "invalid-member-name") == FbcDiagId::ExpectedIdentifier);
    CHECK(fbcErrorFor(
              parseDocument("type t\n  declare sub f()\n  as integer next\n"
                            "end type\n"),
              "invalid-member-name") ==
          FbcDiagId::KeywordFieldsNotAllowedInClasses);
    CHECK(fbcErrorFor(parseDocument("enum e\n  next = 1\nend enum\n"),
                      "invalid-member-name") == FbcDiagId::ExpectedEol);
    // The two warnings number into the warning catalog: without fbcKind they
    // would render as `fbc error: 44` / `fbc error: 12` on the wire.
    for (char const *src : {"print% 1\n", "print \"abc\n"}) {
      ParseResult const r = parseDocument(src);
      bool sawWarningKind = false;
      for (auto const &d : r.diagnostics) {
        if (d.fbcKind == FbcMessageKind::Warning) {
          sawWarningKind = true;
        }
      }
      CHECK_MSG(sawWarningKind, "mapped warnings must carry the warning kind");
    }
  }

  // Line structures: single-line IF needs no closer.
  {
    ParseResult r = parseDocument("if a then print 1\nprint 2\n");
    CHECK(r.diagnostics.empty());
  }
  // One-line block with END IF still matches.
  {
    ParseResult r =
        parseDocument("if a then : print 1 : else : print 2 : end if\n");
    CHECK(r.diagnostics.empty());
  }

  // Duplicate declaration warning; forward declare + define is not a dup.
  {
    ParseResult r = parseDocument("dim x as integer\ndim x as string\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 1);
  }
  {
    ParseResult r = parseDocument("declare sub f()\nsub f()\nend sub\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 0);
  }

  // A variable *usage* is never a redefinition (BUGS.md).
  {
    ParseResult r = parseDocument("dim x as integer\n"
                                  "x = 1\n"
                                  "print \"X is: \", x\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 0);
    CHECK(r.diagnostics.empty());
  }

  // Declaration-scope blocks (scope/if/for/while/do/select/with) can reuse a
  // name declared in the enclosing scope: the block-local Dim dies at the
  // closer (fbc-probed), so it is not a duplicate and a fresh module-level
  // Dim after the block is not one either.
  {
    ParseResult r = parseDocument("dim x as integer = 1\n"
                                  "scope\n"
                                  "    dim x as string = \"Hello\"\n"
                                  "end scope\n"
                                  "dim x as integer\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 1); // only the last:
    CHECK(r.diagnostics[0].range.beg ==
          static_cast<uint32_t>(std::string("dim x as integer = 1\n"
                                            "scope\n"
                                            "    dim x as string = "
                                            "\"Hello\"\n"
                                            "end scope\n")
                                    .size() +
                                4));
  }
  {
    ParseResult r = parseDocument("if true then\n"
                                  "    dim x as string\n"
                                  "end if\n"
                                  "dim x as integer\n");
    CHECK(r.diagnostics.empty());
  }
  {
    ParseResult r = parseDocument("dim x as integer\n"
                                  "for i = 1 to 3\n"
                                  "    dim x as string\n"
                                  "next i\n"
                                  "while false\n"
                                  "    dim x as string\n"
                                  "wend\n"
                                  "do\n"
                                  "    dim x as string\n"
                                  "loop\n"
                                  "select case 1\n"
                                  "case 1\n"
                                  "    dim x as string\n"
                                  "end select\n"
                                  "with p\n"
                                  "    dim x as string\n"
                                  "end with\n");
    CHECK(r.diagnostics.empty());
  }

  // A same-scope redefinition stays a duplicate even inside a block, and a
  // module-level redefinition after a shadowing block still hits the
  // module-level declaration.
  {
    ParseResult r = parseDocument("scope\n"
                                  "    dim x as string\n"
                                  "    dim x as integer\n"
                                  "end scope\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 1);
  }
  {
    ParseResult r = parseDocument("dim x as integer\n"
                                  "if true then\n"
                                  "    dim x as string\n"
                                  "end if\n"
                                  "dim x as integer\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 1);
    CHECK(r.diagnostics[0].range.beg ==
          static_cast<uint32_t>(std::string("dim x as integer\n"
                                            "if true then\n"
                                            "    dim x as string\n"
                                            "end if\n")
                                    .size() +
                                4));
  }

  // EXTERN is not a declaration scope: a Dim inside leaks to module scope and
  // duplicates a module-level declaration (fbc-probed, error 4).
  {
    ParseResult r = parseDocument("extern \"C\"\n"
                                  "    dim x as integer\n"
                                  "end extern\n"
                                  "dim x as integer\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 1);
  }

  // The BUGS.md function example: only `dim y` twice in the same procedure is
  // a duplicate; the loop iterator, the module-level `x`, and every usage are
  // not.
  {
    ParseResult r = parseDocument("function do_something() as integer\n"
                                  "    dim x as integer = 1\n"
                                  "    dim y as integer\n"
                                  "    for x = 1 to 100\n"
                                  "        y = x + 1\n"
                                  "    next x\n"
                                  "    dim y as integer\n"
                                  "    y = x * 2\n"
                                  "    return y\n"
                                  "end function\n"
                                  "dim x as integer\n"
                                  "x = do_something()\n"
                                  "print \"X is:\", x\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 1);
  }

  // Commas inside a parenthesized initializer are *argument* separators, not
  // declaration-list separators. A UDT-literal / member-access initializer
  // (`dim as Vector3 b = type(a.x, .sectors(0).h, a.y)`) produced false
  // "duplicate definition" warnings because every `,` (even at paren depth > 0)
  // re-armed the atName state, registering `.sectors` and the second `a` as
  // new definitions. Only the declared names may land in the symbol tree.
  {
    ParseResult r = parseDocument("dim as single a = 1\n"
                                  "dim as Vector3 b = type(a.x, "
                                  ".sectors(0).floorHeight, a.y)\n"
                                  "dim as Vector3 c = type(b.x, "
                                  ".sectors(0).ceilingHeight, b.y)\n");
    CHECK(r.diagnostics.empty());
    CHECK(r.roots.size() == 3);
    const Symbol *b = find(r.roots, "b", SymbolKind::Dim);
    CHECK(b != nullptr);
    CHECK(b->children.empty()); // a, sectors, a.y: never definitions
  }
  {
    ParseResult r = parseDocument("dim as Integer c = 0\n"
                                  "dim as Integer d = Calc(c, c + 1)\n");
    CHECK(r.diagnostics.empty());
  }
  // The same nesting that produced the report: WITH + FOR + IF blocks with
  // `type(v.x, .sectors(i).h, v.y)` initializers parse clean, and a loop-local
  // Dim reused across the loop body is not a duplicate.
  {
    ParseResult r = parseDocument("type V2\n"
                                  "    as single x, y\n"
                                  "end type\n"
                                  "type V3\n"
                                  "    as single x, y, z\n"
                                  "end type\n"
                                  "type Sector\n"
                                  "    as single floorHeight\n"
                                  "    as single ceilingHeight\n"
                                  "end type\n"
                                  "type Map\n"
                                  "    as V2 vertices(10)\n"
                                  "    as Sector sectors(10)\n"
                                  "end type\n"
                                  "sub s(map as Map, secIndex as integer)\n"
                                  "    with map\n"
                                  "        for i as integer = 0 to 3\n"
                                  "            dim as V2 p = .vertices(i)\n"
                                  "            if p.x <> 0 then\n"
                                  "                dim as V3 bl = type(p.x, "
                                  ".sectors(secIndex).floorHeight, p.y)\n"
                                  "                dim as V3 tr = type(p.x, "
                                  ".sectors(secIndex).ceilingHeight, p.y)\n"
                                  "            end if\n"
                                  "        next i\n"
                                  "    end with\n"
                                  "end sub\n");
    CHECK(diagnosticCount(r, "next-var-meaningless") == 1);
    CHECK(diagnosticCount(r, "for-next-variable-mismatch") == 0);
    CHECK(diagnosticCount(r, "unterminated-block") == 0);
    CHECK(diagnosticCount(r, "stray-closer") == 0);
  }

  // TYPE members are captured as the declared *member* names, never the type
  // names: `as Wall walls(MAX_WALLS - 1)` registers `walls`, comma lists
  // register every member, and each member's signature carries its full
  // declaration line so the declared type survives for hover/resolve.
  // Prototype lines (`declare constructor(...)`, as raymath.bi uses) declare
  // no fields, so their parameter/name identifiers register nothing.
  {
    ParseResult r = parseDocument("type Vec\n"
                                  "    x as single\n"
                                  "    as Wall walls(10)\n"
                                  "    as integer a, b\n"
                                  "    declare constructor(x as single, "
                                  "y as single)\n"
                                  "    declare sub Init()\n"
                                  "end type\n");
    CHECK(r.diagnostics.empty());
    const Symbol *vec = find(r.roots, "vec", SymbolKind::Type);
    CHECK(vec != nullptr);
    const Symbol *x = find(vec->children, "x", SymbolKind::Variable);
    CHECK(x != nullptr && x->signature == "x as single");
    const Symbol *walls = find(vec->children, "walls", SymbolKind::Variable);
    CHECK(walls != nullptr && walls->signature == "as Wall walls(10)");
    const Symbol *a = find(vec->children, "a", SymbolKind::Variable);
    const Symbol *b = find(vec->children, "b", SymbolKind::Variable);
    CHECK(a != nullptr && b != nullptr);
    CHECK(a->signature == "as integer a, b");
    // The type name and the declare prototypes must not leak into the fields.
    CHECK(find(vec->children, "wall", SymbolKind::Variable) == nullptr);
    CHECK(find(vec->children, "x", SymbolKind::Const) == nullptr);
    CHECK(vec->children.size() == 5); // 4 fields + the declared Sub prototype
    CHECK(find(vec->children, "init", SymbolKind::Sub) != nullptr);
  }

  // Parameters carry their full declaration text (modifiers + type) so
  // member-access resolution can recover the declared type of a base variable.
  {
    ParseResult r =
        parseDocument("sub s(byref map as Map, secIndex as integer)\n"
                      "end sub\n");
    const Symbol *s = find(r.roots, "s", SymbolKind::Sub);
    CHECK(s != nullptr);
    const Symbol *map = find(s->children, "map", SymbolKind::Parameter);
    CHECK(map != nullptr);
    CHECK(map->signature == "byref map as Map");
    const Symbol *si = find(s->children, "secindex", SymbolKind::Parameter);
    CHECK(si != nullptr && si->signature == "secIndex as integer");
  }
  // declares both, and a multi-dim array `DIM grid(0 to 5, 0 to 5)` never
  // splits on the comma inside its bounds.
  {
    ParseResult r = parseDocument("dim a = 1, b = 2\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 0);
    CHECK(r.roots.size() == 2);
    CHECK(find(r.roots, "a", SymbolKind::Dim) != nullptr);
    CHECK(find(r.roots, "b", SymbolKind::Dim) != nullptr);
  }
  {
    ParseResult r = parseDocument("dim grid(0 to 5, 0 to 5) as Integer\n");
    CHECK(diagnosticCount(r, "duplicate-definition") == 0);
    CHECK(r.roots.size() == 1);
    CHECK(find(r.roots, "grid", SymbolKind::Dim) != nullptr);
  }

  // Strings and continuations.
  {
    ParseResult r = parseDocument("dim s as string\nprint \"abc\n");
    CHECK(diagnosticCount(r, "unterminated-string") == 1);
  }
  {
    ParseResult r = parseDocument("dim x = 1 + _\n    2\n");
    CHECK(r.diagnostics.empty());
  }

  // Preprocessor blocks.
  {
    ParseResult r = parseDocument("#if __FB_DEBUG__\nprint 1\n#endif\n");
    CHECK(r.diagnostics.empty());
  }
  {
    ParseResult r = parseDocument("#endif\n");
    CHECK(diagnosticCount(r, "stray-closer") == 1);
  }

  // Dialect detection.
  {
    ParseResult r = parseDocument("#LANG \"qb\"\nx = 1\nprint x\n");
    CHECK(r.lang == "qb");
    CHECK(diagnosticCount(r, "lang-mode") == 1);
  }
  {
    ParseResult r = parseDocument("#lang \"fb\"\nprint 1\n");
    CHECK(r.lang == "fb");
    CHECK(r.diagnostics.empty());
  }
  {
    ParseResult r = parseDocument("#lang \"future\"\nprint 1\n");
    CHECK(r.lang == "fb");
    CHECK(r.diagnostics.empty());
  }
  {
    ParseResult r = parseDocument("'$lang: \"qb\"\nx = 1\nprint x\n");
    CHECK(r.lang == "qb");
    CHECK(diagnosticCount(r, "lang-mode") == 1);
  }
  {
    ParseResult r = parseDocument("rem $lang : \"qb\"\nx = 1\nprint x\n");
    CHECK(r.lang == "qb");
    CHECK(diagnosticCount(r, "lang-mode") == 1);
  }

  // Keyword documentation URLs follow the wiki's real page names.
  {
    CHECK(keywordDocsUrl("dim") == "https://www.freebasic.net/wiki/KeyPgDim");
    CHECK(keywordDocsUrl("DIM") == "https://www.freebasic.net/wiki/KeyPgDim");
    CHECK(keywordDocsUrl("print") ==
          "https://www.freebasic.net/wiki/KeyPgPrint");
    CHECK(keywordDocsUrl("and") == "https://www.freebasic.net/wiki/KeyPgOpAnd");
    CHECK(keywordDocsUrl("if") == "https://www.freebasic.net/wiki/KeyPgIfthen");
    CHECK(keywordDocsUrl("select") ==
          "https://www.freebasic.net/wiki/KeyPgSelectcase");
    CHECK(keywordDocsUrl("new") == "https://www.freebasic.net/wiki/KeyPgOpNew");
    CHECK(keywordDocsUrl("pointer") ==
          "https://www.freebasic.net/wiki/KeyPgPtr");
    CHECK(keywordDocsUrl("protected") ==
          "https://www.freebasic.net/wiki/KeyPgVisProtected");
    CHECK(keywordDocsUrl("andalso") ==
          "https://www.freebasic.net/wiki/KeyPgOpAndAlso");
    CHECK(keywordDocsUrl("counter").empty());
  }

  // `Enum <name> explicit` (KeyPgEnum): the optional `explicit` keyword is
  // consumed on the declaration header, so it is *not* captured as a spurious
  // member; the Enum root gets the flag, and the header signature stays a
  // single line. A plain `enum` leaves the flag unset. `explicit` itself is
  // reserved globally (fbc errors 4/3, probe-verified) and its docs page is
  // the enum's.
  {
    ParseResult r = parseDocument("enum my_enum explicit\n"
                                  "    value_1 = 1\n"
                                  "    value_2 = 2\n"
                                  "end enum\n"
                                  "\n"
                                  "enum color\n"
                                  "    red = 1\n"
                                  "    green\n"
                                  "end enum\n");
    CHECK(r.diagnostics.empty());
    const Symbol *me = find(r.roots, "my_enum", SymbolKind::Enum);
    CHECK(me != nullptr);
    CHECK(me->explicitEnum);
    CHECK(me->signature == "enum my_enum explicit");
    CHECK(me->children.size() == 2);
    CHECK(me->children[0].name == "value_1");
    CHECK(me->children[0].kind == SymbolKind::Const);
    bool sawExplicit = false;
    for (const Symbol &c : me->children) {
      sawExplicit = sawExplicit || c.name == "explicit";
    }
    CHECK(!sawExplicit);
    const Symbol *color = find(r.roots, "color", SymbolKind::Enum);
    CHECK(color != nullptr);
    CHECK(!color->explicitEnum);
    CHECK(color->signature == "enum color"); // header not padded with members
    CHECK(color->children.size() == 2);
    CHECK(isReservedWord("explicit"));
    CHECK(keywordDocsUrl("explicit") ==
          "https://www.freebasic.net/wiki/KeyPgEnum");
  }

  // TYPE access sections (FreeBASIC.md §7 Access sections):
  // `Private:`/`Public:`/`Protected:` inside a TYPE body gate every member
  // declared after them until the next section; members default to Public.
  // Union bodies reject the section syntax (fbc: syntax error) and enum
  // members stay Public.
  {
    ParseResult r = parseDocument("type position\n"
                                  "    x as integer\n"
                                  "    private:\n"
                                  "    secret as integer\n"
                                  "    declare sub touch()\n"
                                  "    protected:\n"
                                  "    guard as integer\n"
                                  "    public:\n"
                                  "    y as integer\n"
                                  "end type\n"
                                  "\n"
                                  "type box\n"
                                  "    private: a as integer\n"
                                  "end type\n");
    CHECK(r.diagnostics.empty());
    const Symbol *pos = find(r.roots, "position", SymbolKind::Type);
    CHECK(pos != nullptr);
    // x, secret, touch (declared Sub), guard, y.
    CHECK(pos->children.size() == 5);
    struct {
      const char *key;
      Access access;
    } const expected[] = {
        {"x", Access::Public},      {"secret", Access::Private},
        {"touch", Access::Private}, {"guard", Access::Protected},
        {"y", Access::Public},
    };
    for (auto const &e : expected) {
      const Symbol *m = find(pos->children, e.key, SymbolKind::Variable);
      if (m == nullptr) {
        m = find(pos->children, e.key, SymbolKind::Sub);
      }
      CHECK(m != nullptr);
      CHECK(m->access == e.access);
    }
    // The section gate is carried by the container, so a `private:` on the
    // same line as the first member applies to it too.
    const Symbol *box = find(r.roots, "box", SymbolKind::Type);
    CHECK(box != nullptr);
    const Symbol *a = find(box->children, "a", SymbolKind::Variable);
    CHECK(a != nullptr && a->access == Access::Private);
  }
  {
    // Members default to Public; `protected` is a real reserved keyword but an
    // ordinary member name still lexes and captures as a field.
    ParseResult r = parseDocument("type t\n"
                                  "    as integer protected\n"
                                  "end type\n");
    CHECK(r.diagnostics.empty());
    const Symbol *t = find(r.roots, "t", SymbolKind::Type);
    CHECK(t != nullptr);
    const Symbol *p = find(t->children, "protected", SymbolKind::Variable);
    CHECK(p != nullptr && p->access == Access::Public);
    CHECK(isReservedWord("protected"));
    CHECK(keywordDocsUrl("protected") ==
          "https://www.freebasic.net/wiki/KeyPgVisProtected");
    // An uppercase member name is the same fact: keywords classify regardless
    // of case, and a type's field slot still captures them.
    ParseResult upper = parseDocument("type t2\n"
                                      "    as integer PROTECTED\n"
                                      "end type\n");
    CHECK(upper.diagnostics.empty());
    const Symbol *t2 = find(upper.roots, "t2", SymbolKind::Type);
    CHECK(t2 != nullptr);
    CHECK(find(t2->children, "protected", SymbolKind::Variable) != nullptr);
  }
  {
    // Outside a TYPE body the section colon declares nothing (fbc accepts the
    // line only inside a type): no member is registered, and nothing crashes.
    ParseResult r = parseDocument("private:\n"
                                  "dim x as integer\n");
    CHECK(r.diagnostics.empty());
    CHECK(r.roots.size() == 1);
  }
  {
    // Access sections are TYPE-only, so `private:` in a Union body is a
    // statement the body cannot accept: the missing `END UNION` belongs right
    // above it (fbc: `error 17: found 'private'`, then `error 19: Expected
    // 'END UNION' in 'private:'`). The squiggle stays on the opener and
    // `closerAt` is the insertion point the M12 fix uses.
    std::string const src = "union u\n"
                            "    private:\n"
                            "    a as integer\n"
                            "end union\n";
    ParseResult r = parseDocument(src);
    const Symbol *u = find(r.roots, "u", SymbolKind::Union);
    CHECK(u != nullptr);
    CHECK(u->children.empty()); // closed before the access section
    CHECK(r.diagnostics.size() == 2);
    CHECK(r.diagnostics[0].code == "unterminated-block");
    CHECK(r.diagnostics[0].range.beg == 0); // anchored at the opener
    CHECK(r.diagnostics[0].closerAt.value_or(0) ==
          static_cast<std::uint32_t>(src.find("private:")));
    CHECK(r.diagnostics[1].code == "stray-closer");
  }
  {
    // A record body is a member list, so the first statement it cannot accept
    // is where the closer belongs (fbc anchors `error 19` there). The block
    // ends at that statement, the statement is parsed in the enclosing scope —
    // nothing of it is captured as a member — and the report carries the offset
    // the M12 fix inserts at, distinct from its range, which stays on the
    // opener.
    std::string const src = "type point\n"
                            "  x as single\n"
                            "  y as single\n"
                            "print 1\n"
                            "dim p as point\n";
    ParseResult r = parseDocument(src);
    const Symbol *t = find(r.roots, "point", SymbolKind::Type);
    CHECK(t != nullptr);
    CHECK(t->children.size() == 2); // the fields, and nothing after them
    std::uint32_t const at = static_cast<std::uint32_t>(src.find("print 1"));
    CHECK(t->range.end == at);
    // `print 1` is a statement, `dim p as point` a module-level declaration:
    // both reached the enclosing scope, so the module sees the latter and the
    // record does not.
    CHECK(find(r.roots, "p", SymbolKind::Dim) != nullptr);
    CHECK(find(t->children, "p", SymbolKind::Variable) == nullptr);
    CHECK(r.diagnostics.size() == 1);
    CHECK(r.diagnostics[0].code == "unterminated-block");
    CHECK(r.diagnostics[0].range.beg == 0);
    CHECK(r.diagnostics[0].closerAt.value_or(0) == at);
  }
  {
    // The reported case, and the one member the language cannot have at all: a
    // field whose type is the record declaring it is fbc's `error 88`, so the
    // body ends at `dim p as point` and not at the statement after it. The
    // consequence worth pinning is the one the bug report is really about: the
    // module-level `dim p as point` is no longer a field of `point`, so
    // nothing later collides with it.
    std::string const src = "type point\n"
                            "    x as single\n"
                            "    y as single\n"
                            "\n"
                            "dim p as point\n"
                            "p.x = 1.5\n"
                            "print p.y\n";
    ParseResult r = parseDocument(src);
    const Symbol *t = find(r.roots, "point", SymbolKind::Type);
    std::uint32_t const at = static_cast<std::uint32_t>(src.find("dim p"));
    CHECK(t != nullptr);
    CHECK(t != nullptr && t->children.size() == 2);
    CHECK(t != nullptr && t->range.end == at);
    CHECK(find(r.roots, "p", SymbolKind::Dim) != nullptr);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].code == "unterminated-block");
      CHECK(r.diagnostics[0].closerAt.value_or(0) == at);
    }
  }
  {
    // The two forms that escape `error 88` are the two with no per-instance
    // storage of their own — `ptr`, the documented workaround, and `static` —
    // and both compile in fbc, so neither may become a boundary. An array
    // dimension does not help (`dim p as point(10)` is `error 88`), and
    // neither does a type suffix, which `fb` ignores (warning 44). The field
    // is not named `next`: the server refuses conditional field names in a
    // record a `Static` arms, which fbc rejects with `error 238`
    // and this body now diagnoses.
    std::string const src = "type node\n"
                            "  tail as node ptr\n"
                            "  static total as node\n"
                            "  label as string\n"
                            "end type\n";
    ParseResult r = parseDocument(src);
    CHECK(r.diagnostics.empty());
    const Symbol *n = find(r.roots, "node", SymbolKind::Type);
    CHECK(n != nullptr);
    CHECK(n != nullptr && n->children.size() == 3);
  }
  {
    // Nested records nest the failure: the innermost body cannot accept the
    // statement, and neither can the one around it. Both closers belong on that
    // same line, reported innermost first — which is the order the fixes nest
    // in when applied one after the other.
    std::string const src = "type outer\n"
                            "  type inner\n"
                            "    n as integer\n"
                            "print 1\n";
    ParseResult r = parseDocument(src);
    const Symbol *outer = find(r.roots, "outer", SymbolKind::Type);
    CHECK(outer != nullptr);
    const Symbol *inner = find(outer->children, "inner", SymbolKind::Type);
    CHECK(inner != nullptr);
    std::uint32_t const at = static_cast<std::uint32_t>(src.find("print 1"));
    CHECK(inner->range.end == at);
    CHECK(outer->range.end == at);
    CHECK(r.diagnostics.size() == 2);
    if (r.diagnostics.size() == 2) {
      CHECK(r.diagnostics[0].code == "unterminated-block");
      CHECK(r.diagnostics[1].code == "unterminated-block");
      CHECK(r.diagnostics[1].closerAt.value_or(0) == at);
    }
  }
  {
    // A closer that does not match the innermost block is the same evidence
    // from the other side: the block ends *before* it, and the closer then
    // closes the block underneath — which is how the nesting survives the fix.
    // fbc reads it the same way (`error 13: Expected 'NEXT', found 'end' in
    // 'end sub'`).
    std::string const src = "sub s()\n"
                            "  if x then\n"
                            "    print 2\n"
                            "end sub\n";
    ParseResult r = parseDocument(src);
    std::uint32_t const at = static_cast<std::uint32_t>(src.find("end sub"));
    const Symbol *s = find(r.roots, "s", SymbolKind::Sub);
    CHECK(s != nullptr);
    // The `end sub` that closed it is the last thing in the buffer, so the
    // sub's own range ends there.
    std::uint32_t const closerEnd = at + 7; // "end sub"
    CHECK(s->range.end == closerEnd);
    CHECK(r.blockRanges.size() == 2);
    if (r.blockRanges.size() == 2) {
      // The `if` ends at the mismatching closer, not at the buffer end.
      CHECK(r.blockRanges[0].end == at);
      CHECK(r.blockRanges[1].end == closerEnd);
    }
    CHECK(r.diagnostics.size() == 2);
    if (r.diagnostics.size() == 2) {
      CHECK(r.diagnostics[0].code == "closer-mismatch");
      CHECK(r.diagnostics[1].code == "unterminated-block");
      CHECK(r.diagnostics[1].closerAt.value_or(0) == at);
    }
  }
  {
    // In a record body the same mismatch *is* the body boundary, so it is
    // reported once: the `unterminated-block` already names the closer that was
    // expected, and a `closer-mismatch` naming it would say the same thing
    // twice.
    std::string const src = "type t\n"
                            "  n as integer\n"
                            "end sub\n";
    ParseResult r = parseDocument(src);
    CHECK(r.diagnostics.size() == 1);
    if (r.diagnostics.size() == 1) {
      CHECK(r.diagnostics[0].code == "unterminated-block");
      CHECK(r.diagnostics[0].closerAt.value_or(0) ==
            static_cast<std::uint32_t>(src.find("end sub")));
    }
  }
  {
    // `END FOR` / `END WHILE` cannot close anything (fbc: `error 33: Illegal
    // 'END'`), which makes them the same evidence as a mismatching closer: the
    // block ends before the token, and no second closer is owed.
    std::string const src = "for i = 1 to 3\n"
                            "  print i\n"
                            "end for\n";
    ParseResult r = parseDocument(src);
    CHECK(r.diagnostics.size() == 2);
    if (r.diagnostics.size() == 2) {
      CHECK(r.diagnostics[0].code == "invalid-end");
      CHECK(r.diagnostics[1].code == "unterminated-block");
      CHECK(r.diagnostics[1].closerAt.value_or(0) ==
            static_cast<std::uint32_t>(src.find("end for")));
    }
  }
  {
    // A block the user is still typing in (no closer yet) still gets a range
    // covering the rest of the source, so containment lookups work while
    // editing. Regression: until this fix, an unclosed block kept
    // range.end == 0 and deepestNesting/innermostScope rejected every offset
    // inside it — hovering, completion, or definition inside a half-typed
    // procedure found nothing.
    std::string const src = "sub s()\n"
                            "    dim q\n"
                            "    print q\n";
    ParseResult r = parseDocument(src);
    CHECK(r.diagnostics.size() == 1); // the unterminated-block error stays
    const Symbol *s = find(r.roots, "s", SymbolKind::Sub);
    CHECK(s != nullptr);
    CHECK(s->range.end == src.size());
    const Symbol *q = find(s->children, "q", SymbolKind::Dim);
    CHECK(q != nullptr);
  }

  {
    // M15: the two type-graph edges. `Extends` is FreeBASIC's only inheritance
    // form (fbc rejects `type b : a`, and there is no `interface` keyword), and
    // a member procedure is declared inside the type and defined at module
    // level qualified by the type name (fbc error 17 rejects a definition in
    // the type body).
    std::string const src = "type t extends object\n"
                            "  declare sub go()\n"
                            "  n as integer\n"
                            "end type\n"
                            "sub t.go()\n"
                            "end sub\n";
    ParseResult r = parseDocument(src);
    const Symbol *t = find(r.roots, "t", SymbolKind::Type);
    CHECK(t != nullptr);
    // The `extends` keyword was a bare keyword token on the type's opener line,
    // and the TYPE-body member-capture path used to register it as a Variable
    // field. Two fields is the correct count; three was the bug.
    CHECK(t->children.size() == 2);
    CHECK(find(t->children, "extends", SymbolKind::Variable) == nullptr);
    CHECK(find(t->children, "go", SymbolKind::Sub) != nullptr);
    CHECK(find(t->children, "n", SymbolKind::Variable) != nullptr);
    CHECK(t->extendsKey == "object");
    // `sub t.go()` used to be a module root named and keyed `t` — the same key
    // as the type it implements a member of, so documentSymbol listed `t` twice
    // and codeLens drew a "0 references" lens for the phantom. It is now keyed
    // by the member and carries the owner as the other half of the edge.
    const Symbol *impl = find(r.roots, "go", SymbolKind::Sub);
    CHECK(impl != nullptr);
    CHECK(impl->ownerKey == "t");
    CHECK(impl->name == "go");
    // The selection is the *member* token, not the `t` qualifier. That is what
    // leaves the `t` token free to resolve to the type, so a rename of the type
    // still updates `sub t.go()` — there was never a rename bug here, only a
    // display and addressability one.
    CHECK(impl->selection.beg == src.rfind("sub t.go()") + 6); // the member
    CHECK(impl->selection.end - impl->selection.beg == 2);     // token, not
                                                               // the type
  }
  {
    // A user-defined base, and a Union: the same edge on both openers.
    std::string const src = "union u extends a\n"
                            "  x as integer\n"
                            "end union\n"
                            "type b extends u\n"
                            "  y as integer\n"
                            "end type\n"
                            "type plain\n"
                            "  z as integer\n"
                            "end type\n";
    ParseResult r = parseDocument(src);
    const Symbol *u = find(r.roots, "u", SymbolKind::Union);
    CHECK(u != nullptr);
    CHECK(u->extendsKey == "a");
    CHECK(u->children.size() == 1);
    const Symbol *b = find(r.roots, "b", SymbolKind::Type);
    CHECK(b != nullptr);
    CHECK(b->extendsKey == "u");
    const Symbol *p = find(r.roots, "plain", SymbolKind::Type);
    CHECK(p != nullptr);
    CHECK(p->extendsKey.empty());
  }
  {
    // The qualifier form is the same for all three member-procedure kinds, and
    // a plain module-level procedure keeps its own name and no owner.
    std::string const src = "type s\n"
                            "  declare function val() as integer\n"
                            "end type\n"
                            "function s.val() as integer\n"
                            "end function\n"
                            "property s.p as integer\n"
                            "end property\n"
                            "sub plain()\n"
                            "end sub\n";
    ParseResult r = parseDocument(src);
    const Symbol *v = find(r.roots, "val", SymbolKind::Function);
    CHECK(v != nullptr);
    CHECK(v != nullptr && v->ownerKey == "s");
    const Symbol *p = find(r.roots, "p", SymbolKind::Property);
    CHECK(p != nullptr);
    CHECK(p != nullptr && p->ownerKey == "s");
    const Symbol *plain = find(r.roots, "plain", SymbolKind::Sub);
    CHECK(plain != nullptr);
    CHECK(plain != nullptr && plain->ownerKey.empty());
    // No root claims the key `s` twice any more.
    int keyedS = 0;
    for (const auto &s : r.roots) {
      if (s.key == "s") {
        ++keyedS;
      }
    }
    CHECK(keyedS == 1);
  }
  {
    // Deliberate limit (FreeBASIC.md §12): a Constructor/Destructor member's
    // implementation is spelled `constructor t()` with no dot, which the parser
    // cannot tell from a module constructor — fbc accepts both, and guessing
    // wrong would rewrite a real declaration. Left exactly as before: a root
    // named after the type.
    std::string const src = "type t\n"
                            "  n as integer\n"
                            "end type\n"
                            "constructor t()\n"
                            "end constructor\n";
    ParseResult r = parseDocument(src);
    const Symbol *c = find(r.roots, "t", SymbolKind::Constructor);
    CHECK(c != nullptr);
    CHECK(c != nullptr && c->ownerKey.empty());
  }
  {
    // A malformed `extends` clause invents no edge and no diagnostic here, and
    // does not swallow the body's first field: `extends` is consumed, the
    // missing base is simply not recorded.
    std::string const src = "type w extends\n"
                            "  n as integer\n"
                            "end type\n";
    ParseResult r = parseDocument(src);
    const Symbol *w = find(r.roots, "w", SymbolKind::Type);
    CHECK(w != nullptr);
    CHECK(w != nullptr && w->extendsKey.empty());
    CHECK(w != nullptr && w->children.size() == 1);
    CHECK(w != nullptr && w->children[0].name == "n");
  }
  {
    // The one-line form still parses, qualifier and all.
    std::string const src = "type q extends p : n as integer : end type\n";
    ParseResult r = parseDocument(src);
    const Symbol *q = find(r.roots, "q", SymbolKind::Type);
    CHECK(q != nullptr);
    CHECK(q != nullptr && q->extendsKey == "p");
    CHECK(q != nullptr && q->children.size() == 1);
  }

  {
    // A reserved word fbc refuses as a field name (`error 14`): reported on the
    // name token, and *not* registered — a member the compiler does not create
    // must not be offered in completion or resolved from `v.name`.
    std::string const src = "type point\n"
                            "  as integer and\n"
                            "end type\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "invalid-member-name") == 1);
    CHECK(diagnosticCount(r, "unterminated-block") == 0);
    CHECK(r.diagnostics[0].range.beg == static_cast<uint32_t>(src.find("and")));
    CHECK(r.diagnostics[0].range.end ==
          static_cast<uint32_t>(src.find("and") + 3));
    const Symbol *point = find(r.roots, "point", SymbolKind::Type);
    CHECK(point != nullptr);
    CHECK(point != nullptr && point->children.empty());
  }
  {
    // The `dim <name>` spelling asks the same question and gets the same
    // answer, or `dim and as integer` would sit in the code with no diagnostic
    // while `as integer and` had one.
    ParseResult r = parseDocument("type point\n"
                                  "  dim and as integer\n"
                                  "end type\n");
    CHECK(diagnosticCount(r, "invalid-member-name") == 1);
    CHECK(diagnosticCount(r, "unterminated-block") == 0);
  }
  {
    // A union body is a record body for this purpose, and the 349 words fbc
    // does accept stay members: `next` is the canonical linked-list field.
    std::string const src = "type node\n"
                            "  as integer next\n"
                            "end type\n"
                            "union bits\n"
                            "  as integer and\n"
                            "end union\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "invalid-member-name") == 1);
    const Symbol *node = find(r.roots, "node", SymbolKind::Type);
    CHECK(node != nullptr);
    CHECK(node != nullptr && node->children.size() == 1);
    CHECK(node != nullptr && node->children[0].name == "next");
    const Symbol *bits = find(r.roots, "bits", SymbolKind::Union);
    CHECK(bits != nullptr);
    CHECK(bits != nullptr && bits->children.empty());
  }
  {
    // A reserved word fbc accepts as an enum member is a member, and the body
    // stays open. Every one of these used to report an unterminated enum plus a
    // stray closer for source fbc compiles clean.
    std::string const src = "enum keys\n"
                            "  access\n"
                            "  print\n"
                            "  stop = 1\n"
                            "  data = 2\n"
                            "end enum\n";
    ParseResult r = parseDocument(src);
    CHECK(r.diagnostics.empty());
    const Symbol *keys = find(r.roots, "keys", SymbolKind::Enum);
    CHECK(keys != nullptr);
    CHECK(keys != nullptr && keys->children.size() == 4);
    if (keys != nullptr && keys->children.size() == 4) {
      CHECK(keys->children[0].key == "access");
      CHECK(keys->children[1].key == "print");
      CHECK(keys->children[2].key == "stop");
      CHECK(keys->children[2].kind == SymbolKind::Const);
    }
  }
  {
    // A reserved word fbc refuses as an enum member keeps the boundary it had —
    // the closer belongs on that line, and the `unterminated-block` fix inserts
    // it there — and gains the name that says why. Two diagnostics, not one:
    // the boundary says where, the name says which word.
    std::string const src = "enum colors\n"
                            "  sub\n"
                            "end enum\n";
    ParseResult r = parseDocument(src);
    CHECK(diagnosticCount(r, "invalid-member-name") == 1);
    CHECK(r.diagnostics[0].code == "invalid-member-name");
    CHECK(r.diagnostics[0].range.beg == static_cast<uint32_t>(src.find("sub")));
    // The enum's own unterminated-block anchors the closer fix on the offending
    // line, which is where `END ENUM` belongs. A second one follows for the
    // `sub` block, because the boundary hands the line back to the enclosing
    // scope and `sub` really does open a block there — that is the boundary
    // doing its job, not the same report twice, so the count is not asserted.
    bool enumCloserAnchored = false;
    for (const auto &d : r.diagnostics) {
      if (d.code == "unterminated-block" &&
          d.message == "Expected 'END ENUM'") {
        enumCloserAnchored =
            d.closerAt.value_or(0) == static_cast<uint32_t>(src.find("sub"));
      }
    }
    CHECK(enumCloserAnchored);
  }
  {
    // The two tables, checked against the parser over the whole catalog rather
    // than by a spot-check of a few words: for each reserved word, the record
    // and enum answers must be what the tables say. This is the test that keeps
    // the two in step — a table edit that the parser does not implement fails
    // here, and so does a parser change the probe contradicts.
    //
    // `rem` is asked of the table and not of the parser, because the lexer is
    // the layer that answers it (a `rem` line is a comment). It is checked
    // separately below.
    for (std::string_view const w : reservedWords()) {
      if (w == "rem") {
        continue;
      }
      std::string const word(w);
      ParseResult const field = parseDocument("type keyword_test\n"
                                              "  as integer first_field\n"
                                              "  as integer " +
                                              word +
                                              "\n"
                                              "end type\n");
      if (diagnosticCount(field, "invalid-member-name") !=
          (isNeverFieldName(w) ? 1 : 0)) {
        std::printf("FAIL field name \"%s\": %d invalid-member-name, "
                    "isNeverFieldName=%d\n",
                    word.c_str(), diagnosticCount(field, "invalid-member-name"),
                    isNeverFieldName(w) ? 1 : 0);
        ++failures;
      }
      // And the member itself: a word the probe says is legal is a member fbc
      // creates, and one we drop is one completion and hover cannot offer. This
      // is the half that caught `redim`, `local` and `as` — legal names that a
      // statement opener or a second type-introducer claimed first.
      if (!isNeverFieldName(w) && !declares(field, word)) {
        std::printf("FAIL field name \"%s\": no such member\n", word.c_str());
        ++failures;
      }
      ParseResult const en =
          parseDocument("enum e\n  " + word + "\nend enum\n");
      if (diagnosticCount(en, "invalid-member-name") !=
          (isLegalEnumMemberName(w) ? 0 : 1)) {
        std::printf("FAIL enum member \"%s\": %d invalid-member-name, "
                    "isLegalEnumMemberName=%d\n",
                    word.c_str(), diagnosticCount(en, "invalid-member-name"),
                    isLegalEnumMemberName(w) ? 1 : 0);
        ++failures;
      }
      if (isLegalEnumMemberName(w) && !declares(en, word)) {
        std::printf("FAIL enum member \"%s\": no such member\n", word.c_str());
        ++failures;
      }
    }
  }
  {
    // `rem` is a comment, so the table's "not a legal enum member name" is
    // answered before the parser is asked: no member, and no diagnostic on a
    // word the lexer never called a keyword. fbc's complaint about the same
    // source is about the *body* being empty (`error 256`), which is a check
    // this server does not make (FreeBASIC.md §12).
    for (char const *const body : {"  rem\n", "  rem note\n", "  rem = 1\n"}) {
      std::string const src = std::string("enum e\n") + body + "end enum\n";
      ParseResult const r = parseDocument(src);
      if (!r.diagnostics.empty()) {
        std::printf("FAIL enum body \"%s\": %zu diagnostics, want 0\n", body,
                    r.diagnostics.size());
        ++failures;
      }
      const Symbol *e = find(r.roots, "e", SymbolKind::Enum);
      if (e == nullptr || !e->children.empty()) {
        std::printf("FAIL enum body \"%s\": not an empty enum\n", body);
        ++failures;
      }
    }
    // A record field named `rem` is a different question and fbc accepts it,
    // because the word is not at the start of a line there: `as integer rem`
    // lexes as a type and a keyword, not as a comment.
    ParseResult const field = parseDocument("type t\n"
                                            "  as integer first_field\n"
                                            "  as integer rem\n"
                                            "end type\n");
    CHECK(field.diagnostics.empty());
    const Symbol *t = find(field.roots, "t", SymbolKind::Type);
    CHECK(t != nullptr);
    CHECK(t != nullptr && t->children.size() == 2);
    if (t != nullptr && t->children.size() == 2) {
      CHECK(t->children[1].key == "rem");
    }
  }
  {
    // The type half of a field declaration is a chain, not one word: in
    // `as integer ptr the_data`, `ptr` is the pointer modifier and
    // `the_data` is the field (probed; every line below compiles under fbc
    // 1.10.2). The never-field report used to fire on the *modifier* —
    // naming `ptr` as though it were the field name — and dropped the field
    // the line does declare, so completion and hover had no `the_data`.
    // Both declaration spellings are covered: the bare `As` form and the
    // `Dim` one, which reaches the same chain question through
    // handleVarDecls, name-first (`dim x as integer ptr`) and type-first
    // (`dim as integer ptr a, b` — the way to declare several fields of one
    // pointer type).
    struct ChainCase {
      char const *line;
      char const *first;
      char const *second; // "" when the declaration has one name
    };
    constexpr ChainCase kChainCases[] = {
        {"as integer ptr the_data", "the_data", ""},
        {"as integer ptr ptr m", "m", ""},
        {"as integer const ptr c1", "c1", ""},
        {"as const integer c", "c", ""},
        {"as zstring ptr s1", "s1", ""},
        {"as udt ptr u", "u", ""},
        {"x as integer ptr", "x", ""},
        {"dim x as integer ptr", "x", ""},
        {"dim as integer ptr q", "q", ""},
        {"dim as integer ptr a, b", "a", "b"},
        {"dim as const integer c", "c", ""},
        {"dim as udt ptr u1, u2", "u1", "u2"},
    };
    for (ChainCase const &c : kChainCases) {
      std::string const src = std::string("type udt\n"
                                          "  as integer z\n"
                                          "end type\n"
                                          "type t\n  ") +
                              c.line + "\nend type\n";
      ParseResult const r = parseDocument(src);
      if (!r.diagnostics.empty()) {
        std::printf("FAIL type chain \"%s\": %zu diagnostic(s), first %s\n",
                    c.line, r.diagnostics.size(),
                    r.diagnostics.front().code.c_str());
        ++failures;
      }
      if (!declares(r, c.first)) {
        std::printf("FAIL type chain \"%s\": no member \"%s\"\n", c.line,
                    c.first);
        ++failures;
      }
      if (c.second[0] != '\0' && !declares(r, c.second)) {
        std::printf("FAIL type chain \"%s\": no member \"%s\"\n", c.line,
                    c.second);
        ++failures;
      }
      // The modifier is not a member — nothing in the chain leaks a name
      // the compiler never created.
      CHECK(!declares(r, "ptr"));
    }
    // And the report the audit above asks for still stands where the chain
    // is dangling: fbc's own answer to `as integer ptr` with no field behind
    // it is `error 14: Expected identifier`.
    ParseResult const bare = parseDocument("type t\n"
                                           "  as integer ptr\n"
                                           "end type\n");
    CHECK(diagnosticCount(bare, "invalid-member-name") == 1);
    CHECK(!declares(bare, "ptr"));
  }
  {
    // A member name that a statement opener would otherwise claim. The tables
    // say these words are legal names, and a member we drop is a member
    // completion and hover cannot offer — the diagnostic wave found this by
    // auditing every reserved word for a member it should have produced and did
    // not. `redim` and `local` are var-decl openers; in an enum body they are
    // just a name, and the opener handler used to swallow the whole line.
    for (char const *const w : {"redim", "local"}) {
      for (char const *const tail : {"", " = 1"}) {
        std::string const src =
            std::string("enum e\n  ") + w + tail + "\nend enum\n";
        ParseResult const r = parseDocument(src);
        if (!r.diagnostics.empty() || r.roots.size() != 1 ||
            r.roots.front().children.size() != 1) {
          std::printf("FAIL enum member \"%s%s\": not one clean member\n", w,
                      tail);
          ++failures;
          continue;
        }
        CHECK(r.roots.front().children.front().key == w);
        CHECK(r.roots.front().children.front().kind == SymbolKind::Const);
      }
    }
    // Same shape in a record: the type-introducer `as` is also a legal field
    // name, and `as integer as` is the only way to spell it.
    ParseResult const f = parseDocument("type t\n"
                                        "  as integer first_field\n"
                                        "  as integer as\n"
                                        "end type\n");
    CHECK(f.diagnostics.empty());
    const Symbol *t = find(f.roots, "t", SymbolKind::Type);
    CHECK(t != nullptr);
    CHECK(t != nullptr && t->children.size() == 2);
    if (t != nullptr && t->children.size() == 2) {
      CHECK(t->children[1].key == "as");
    }
  }
  {
    // An enum member is `name` or `name = expr`: the tail is part of the member
    // definition, so a line that starts with a fine name can still be no member
    // at all. fbc's `error 3` is about the line, not the word.
    ParseResult r = parseDocument("enum e\n"
                                  "  a 1\n"
                                  "end enum\n");
    CHECK(diagnosticCount(r, "unterminated-block") == 1);
    CHECK(diagnosticCount(r, "invalid-member-name") == 0);
  }
  {
    // The negative test: a conditional field name beside a member procedure.
    // `cva_arg` is legal in a plain record (probe: a plain `type` body
    // accepts every conditional word), but fbc refuses it here with
    // `error 238` anchored on `end type` (FreeBASIC.md §7); this server
    // refuses it at close too, with the diagnostic on the field word, and
    // drops the member as fbc drops it — completion and hover must not offer
    // a field fbc rejects. The trigger is the un-modeled
    // `declare constructor()`, which arms the body through the
    // wait-until-close check: `declare constructor()` declares no field,
    // then `cva_arg` is captured, then `end type` closes and the field is
    // refused.
    ParseResult const r = parseDocument("type t\n"
                                        "  declare constructor()\n"
                                        "  cva_arg as integer\n"
                                        "end type\n");
    CHECK(diagnosticCount(r, "invalid-member-name") == 1);
    const Symbol *t = find(r.roots, "t", SymbolKind::Type);
    CHECK(t != nullptr);
    CHECK(t != nullptr && t->children.empty());
  }
  {
    // The other order, which the close-time check exists for: the field sits
    // *before* the trigger (the probe compiles the field first in every body).
    // fbc rejects both orders — `error 238` is about the body, not the line —
    // and a capture-time-only check would have accepted this one.
    ParseResult const r = parseDocument("type t\n"
                                        "  va_first as integer\n"
                                        "  declare sub go()\n"
                                        "end type\n");
    CHECK(diagnosticCount(r, "invalid-member-name") == 1);
    const Symbol *t = find(r.roots, "t", SymbolKind::Type);
    CHECK(t != nullptr);
    // The declared member procedure itself survives the drop: only the
    // conditional field goes, and `go` stays registered for completion.
    CHECK(t != nullptr && t->children.size() == 1);
    if (t != nullptr && t->children.size() == 1) {
      CHECK(t->children[0].key == "go");
      CHECK(t->children[0].kind == SymbolKind::Sub);
    }
  }
  {
    // The same word in a plain record is a legal field and is captured, not
    // dropped: the diagnostic is about the body, never the name.
    ParseResult const r = parseDocument("type t\n"
                                        "  cva_arg as integer\n"
                                        "  next as node ptr\n"
                                        "end type\n");
    CHECK(r.diagnostics.empty());
    const Symbol *t = find(r.roots, "t", SymbolKind::Type);
    CHECK(t != nullptr);
    CHECK(t != nullptr && t->children.size() == 2);
    if (t != nullptr && t->children.size() == 2) {
      CHECK(t->children[0].key == "cva_arg");
      CHECK(t->children[1].key == "next");
    }
  }
  {
    // `function` and `sub` as function-pointer *type* names. fbc builds
    // a function-pointer type out of `As function() As Integer` and `As sub()`
    // (probed: both compile at module level and inside a record body), so the
    // record member path must read them as the type half of the field, not as
    // the field name — without both rows in `kBuiltinTypes`,
    // `as function() as integer p` registered a member named `function` and
    // dropped `p`.
    ParseResult const r = parseDocument("type t\n"
                                        "  as function() as integer p\n"
                                        "  as sub() q\n"
                                        "end type\n");
    CHECK(r.diagnostics.empty());
    const Symbol *t = find(r.roots, "t", SymbolKind::Type);
    CHECK(t != nullptr);
    CHECK(t != nullptr && t->children.size() == 2);
    if (t != nullptr && t->children.size() == 2) {
      CHECK(t->children[0].key == "p");
      CHECK(t->children[1].key == "q");
      CHECK(t->children[0].kind == SymbolKind::Variable);
    }
  }

  {
    // `TYPE AS ...` spellings, fbc 1.10.2 probed — the root of a cascade of
    // phantom reports: reading `as` as a *name* pushed a record body no
    // `end type` belonged to, so every
    // statement below it parsed as a member list.
    //   module scope: `type as <type> <name>` is an alias named after the
    //     type; `type as long` with no name behind it is fbc error 14 and
    //     opens nothing at all;
    //   record body:  `type as ulong` is a *field* named `type`;
    //   record body:  `type cb as sub(...)` is an alias — not a member, and
    //     its parameter list is not a member list.
    ParseResult const alias =
        parseDocument("type as rAudioBuffer rAudioBuffer_\n"
                      "type as long\n");
    CHECK(alias.diagnostics.empty());
    CHECK(find(alias.roots, "raudiobuffer_", SymbolKind::Type) != nullptr);
    // The nameless spelling opened no body: a closer with nothing to close
    // is a stray, which is exactly what a body it did open would swallow.
    ParseResult const noName = parseDocument("type as long\n"
                                             "end type\n");
    CHECK(diagnosticCount(noName, "stray-closer") == 1);
    CHECK(noName.roots.empty());

    ParseResult const rec = parseDocument("type automation_event\n"
                                          "  frame as ulong\n"
                                          "  type as ulong\n"
                                          "  cva_end as ulong\n"
                                          "end type\n");
    CHECK(rec.diagnostics.empty());
    const Symbol *ae = find(rec.roots, "automation_event", SymbolKind::Type);
    CHECK(ae != nullptr && ae->children.size() == 3);
    if (ae != nullptr && ae->children.size() == 3) {
      CHECK(ae->children[1].key == "type");
      CHECK(ae->children[1].kind == SymbolKind::Variable);
    }

    ParseResult const inBody = parseDocument("type t\n"
                                             "  type cb as sub(byval a as "
                                             "long)\n"
                                             "  x as long\n"
                                             "end type\n");
    CHECK(inBody.diagnostics.empty());
    CHECK(declares(inBody, "cb"));
    CHECK(!declares(inBody, "byval"));

    // After a member procedure's signature the line holds only its return
    // type — `ptr` there is a modifier, never a field name.
    ParseResult const decl = parseDocument("type t\n"
                                           "  declare function f() as const "
                                           "zstring ptr\n"
                                           "  x as long\n"
                                           "end type\n");
    CHECK(decl.diagnostics.empty());
    CHECK(declares(decl, "f"));
    CHECK(!declares(decl, "ptr"));
  }
  {
    // An unnamed *declaration* has no name to duplicate: two bare `enum`s
    // used to collide on the empty key and warn `duplicate definition: ''`
    // at 1:1 (twenty times inside raylib.bi's first bogus body alone). Only
    // that own key is skipped — a member named twice *within one* enum is
    // still fbc's `error 4` (probed), while the same member in two separate
    // enums compiles clean at the declaration (fbc's `error 255` comes at
    // the *use*, FreeBASIC.md §7/§12.19 — a resolve-time report this
    // parse-time check cannot see), so member keys staying per-container
    // matches fbc either way.
    ParseResult const anon = parseDocument("enum\n"
                                           "  log_none\n"
                                           "end enum\n"
                                           "enum\n"
                                           "  log_all\n"
                                           "end enum\n");
    CHECK(anon.diagnostics.empty());
    ParseResult const inBlock = parseDocument("enum\n"
                                              "  log_all\n"
                                              "  log_all\n"
                                              "end enum\n");
    CHECK(diagnosticCount(inBlock, "duplicate-definition") == 1);
    ParseResult const crossBlock = parseDocument("enum\n"
                                                 "  log_all\n"
                                                 "end enum\n"
                                                 "enum\n"
                                                 "  log_all\n"
                                                 "end enum\n");
    CHECK(crossBlock.diagnostics.empty());
  }
  {
    // An enum member *list*: `a, b, c = 5, d` on one line, and a comma at
    // the line end continuing the list on the next (probed; both compile,
    // and rlgl.bi's attribute enums are written the second way). The line
    // shape is a boundary only when it is not a list — `a 1`, `a(3)`.
    ParseResult const lists = parseDocument("enum e\n"
                                            "  a, b, c = 5, d\n"
                                            "  e1 = g(1, 2),\n"
                                            "  e2\n"
                                            "end enum\n");
    CHECK(lists.diagnostics.empty());
    CHECK(diagnosticCount(lists, "unterminated-block") == 0);
    // lean-ctx: only the first name of a multi-name line registers today
    // (`a`, `e1`, `e2` above) — a miss on `b`/`c`/`d`, not a false report.
  }
  {
    // An initializer's braces separate elements, not declarations: every
    // comma inside `{ ... }` used to re-arm the name scan, so
    // `dim x(0 to 2) as single = { lgt, lgt, lgt }` registered `lgt` once
    // per element and warned duplicate.
    ParseResult const br = parseDocument("sub f()\n"
                                         "  dim as single lgt = 1\n"
                                         "  dim x(0 to 2) as single = { lgt, "
                                         "lgt, lgt }\n"
                                         "  dim y(0 to 2) as single = { 1, 2, "
                                         "3 }\n"
                                         "end sub\n");
    CHECK(br.diagnostics.empty());
  }
  {
    // Each branch of an `if` and each `case` of a `select` is its own
    // declaration scope (probed: `dim p` in a `then` and again in its `else`
    // compiles; twice in *one* branch is still error 4). Siblings reuse a
    // name without colliding, same-branch re-declaration still warns.
    ParseResult const branches = parseDocument("sub f(t as single)\n"
                                               "  if t < 1 then\n"
                                               "    dim as single p = t\n"
                                               "  elseif t < 2 then\n"
                                               "    dim as single p = t\n"
                                               "  else\n"
                                               "    dim as single p = t\n"
                                               "  end if\n"
                                               "  select case t\n"
                                               "    case 1\n"
                                               "      dim as single q = 1\n"
                                               "    case 2\n"
                                               "      dim as single q = 2\n"
                                               "  end select\n"
                                               "end sub\n");
    CHECK(branches.diagnostics.empty());
    ParseResult const sameBranch = parseDocument("sub f(t as single)\n"
                                                 "  if t < 1 then\n"
                                                 "    dim as single p = t\n"
                                                 "    dim as single p = t\n"
                                                 "  end if\n"
                                                 "end sub\n");
    CHECK(diagnosticCount(sameBranch, "duplicate-definition") == 1);
  }

  TestStep2bParsingFixes();
  TestQualifiedTypeFirstVarDecls();
  TestKeywordSuffixWarning();
  TestIdentifierSuffixIdentity();
  TestAnonymousDeclarationsNameAndSelect();
  TestPreprocessorSymbolsAndBranches();
  TestModuleConstructors();
  TestAsmBlockRawBody();
  TestModuleLevelStatementRanges();
  TestSelectFamily();
  TestLoopFamily();
  TestExitContinueFamily();
  TestPlacementAndBranchCrossing();
  TestPunctuatorFamily();

  if (failures == 0) {
    std::printf("parser_checks: all passed\n");
    return 0;
  }
  std::printf("parser_checks: %d failures\n", failures);
  return 1;
}
