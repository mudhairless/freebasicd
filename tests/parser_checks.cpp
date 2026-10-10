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

// The fbc number carried by the first diagnostic with this routing code, or 0
// when the parse reported none (or reported one fbc has no number for).
static int fbcErrorFor(const ParseResult &r, const char *code) {
  for (const auto &d : r.diagnostics) {
    if (d.code == code) {
      return d.fbcError;
    }
  }
  return 0;
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

  // A suffix on a variable is part of the identifier (`x%` and `x` are two
  // variables in FreeBASIC), so it is not a keyword warning.
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
    CHECK(r.diagnostics.empty());
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
    CHECK(r.diagnostics.empty());
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
    CHECK(r.diagnostics.empty());
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
          125);
    CHECK(fbcErrorFor(parseDocument("function foo()\n"),
                      "unterminated-block") == 126);
    CHECK(fbcErrorFor(parseDocument("type t\n  as integer i\n"),
                      "unterminated-block") == 19);
    CHECK(fbcErrorFor(parseDocument("union u\n  as integer i\n"),
                      "unterminated-block") == 19);
    CHECK(fbcErrorFor(parseDocument("enum e\n  a = 1\n"),
                      "unterminated-block") == 74);
    CHECK(fbcErrorFor(parseDocument("namespace n\n  dim x as integer\n"),
                      "unterminated-block") == 121);
    CHECK(fbcErrorFor(parseDocument("scope\n  dim x as integer\n"),
                      "unterminated-block") == 95);
    CHECK(fbcErrorFor(parseDocument("extern \"C\"\n  declare sub foo()\n"),
                      "unterminated-block") == 124);
    CHECK(fbcErrorFor(parseDocument("for i = 1 to 3\n  print i\n"),
                      "unterminated-block") == 13);
    CHECK(fbcErrorFor(parseDocument("while 1\n  print 1\n"),
                      "unterminated-block") == 30);
    CHECK(fbcErrorFor(parseDocument("do\n  print 1\n"), "unterminated-block") ==
          29);
    CHECK(fbcErrorFor(parseDocument("if a then\n  print 1\n"),
                      "unterminated-block") == 32);
    CHECK(fbcErrorFor(parseDocument("select case a\n  case 1\n"),
                      "unterminated-block") == 35);
    CHECK(fbcErrorFor(parseDocument("type t\n  as integer i\nend type\nwith t\n"
                                    "  .i = 1\n"),
                      "unterminated-block") == 60);
    CHECK(fbcErrorFor(parseDocument("asm\n  nop\n"), "unterminated-block") ==
          45);
  }
  {
    // A closer with no opener: fbc names each one, and the record/union/enum
    // spellings collapse onto `33 ILLEGALEND` because that is what fbc reports
    // for a bare `end type`/`end enum` (probed: "error 33: Illegal 'END'").
    CHECK(fbcErrorFor(parseDocument("next\n"), "stray-closer") == 107);
    CHECK(fbcErrorFor(parseDocument("loop\n"), "stray-closer") == 106);
    CHECK(fbcErrorFor(parseDocument("wend\n"), "stray-closer") == 108);
    CHECK(fbcErrorFor(parseDocument("else\n"), "stray-closer") == 117);
    CHECK(fbcErrorFor(parseDocument("elseif 1 then\n"), "stray-closer") == 116);
    CHECK(fbcErrorFor(parseDocument("case 1\n"), "stray-closer") == 118);
    CHECK(fbcErrorFor(parseDocument("end if\n"), "stray-closer") == 110);
    CHECK(fbcErrorFor(parseDocument("end select\n"), "stray-closer") == 111);
    CHECK(fbcErrorFor(parseDocument("end with\n"), "stray-closer") == 109);
    CHECK(fbcErrorFor(parseDocument("end sub\n"), "stray-closer") == 112);
    CHECK(fbcErrorFor(parseDocument("end namespace\n"), "stray-closer") == 114);
    CHECK(fbcErrorFor(parseDocument("end scope\n"), "stray-closer") == 113);
    CHECK(fbcErrorFor(parseDocument("end extern\n"), "stray-closer") == 115);
    CHECK(fbcErrorFor(parseDocument("end type\n"), "stray-closer") == 33);
    CHECK(fbcErrorFor(parseDocument("end enum\n"), "stray-closer") == 33);
  }
  {
    // `END FOR`/`END WHILE` are an illegal `END` (error 33), and the block that
    // the token proves is unterminated keeps its own expected-closer number.
    CHECK(fbcErrorFor(parseDocument("for i = 1 to 3\nnext\nend for\n"),
                      "invalid-end") == 33);
    CHECK(fbcErrorFor(parseDocument("if a then\nend select\n"),
                      "closer-mismatch") == 32);
    // The preprocessor blocks are ours, not fbc's: its catalog covers neither
    // #if nor #macro, so no number and no wiki link ride along.
    CHECK(fbcErrorFor(parseDocument("#endif\n"), "stray-closer") == 0);
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
    CHECK(r.diagnostics.empty());
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
  TestKeywordSuffixWarning();
  TestAnonymousDeclarationsNameAndSelect();

  if (failures == 0) {
    std::printf("parser_checks: all passed\n");
    return 0;
  }
  std::printf("parser_checks: %d failures\n", failures);
  return 1;
}
