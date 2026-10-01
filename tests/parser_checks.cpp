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

static int diagnosticCount(const ParseResult &r, const char *code) {
  int n = 0;
  for (const auto &d : r.diagnostics) {
    if (d.code == code) {
      ++n;
    }
  }
  return n;
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
  // declaration-list separators. The UDT-literal / member-access pattern from
  // drd/temp/src/engine.bas (`dim as Vector3 b = type(a.x, .sectors(0).h,
  // a.y)`) produced 37 false "duplicate definition" warnings because every `,`
  // (even at paren depth > 0) re-armed the atName state, registering
  // `.sectors` and the second `a` as new definitions. Only the declared
  // names may land in the symbol tree.
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
          "https://www.freebasic.net/wiki/KeyPgProtected");
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
    // neither does a type suffix, which `fb` ignores (warning 44).
    std::string const src = "type node\n"
                            "  next as node ptr\n"
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

  if (failures == 0) {
    std::printf("parser_checks: all passed\n");
    return 0;
  }
  std::printf("parser_checks: %d failures\n", failures);
  return 1;
}
