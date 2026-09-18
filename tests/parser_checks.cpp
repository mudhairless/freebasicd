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

  // A declaration-list comma at paren depth 0 still splits: `DIM a = 1, b = 2`
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

  if (failures == 0) {
    std::printf("parser_checks: all passed\n");
    return 0;
  }
  std::printf("parser_checks: %d failures\n", failures);
  return 1;
}
