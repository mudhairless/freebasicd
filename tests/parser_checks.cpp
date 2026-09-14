// Parser checks: symbol tree, block matching, diagnostics. Byte-offset.

#include <cstdio>
#include <string>

#include "parser.h"
#include "symbols.h"

using namespace fblang;

static int failures = 0;

#define CHECK(cond)                                                                              \
    do                                                                                           \
    {                                                                                            \
        if (!(cond))                                                                             \
        {                                                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                          \
            ++failures;                                                                          \
        }                                                                                        \
    } while (0)

static int diagnosticCount(const ParseResult& r, const char* code)
{
    int n = 0;
    for (const auto& d : r.diagnostics)
    {
        if (d.code == code)
        {
            ++n;
        }
    }
    return n;
}

static const Symbol* find(const std::vector<Symbol>& scope, const std::string& key, SymbolKind k)
{
    for (const auto& s : scope)
    {
        if (s.key == key && s.kind == k)
        {
            return &s;
        }
    }
    return nullptr;
}

int main()
{
    // A realistic multi-construct program must parse with zero diagnostics.
    {
        const std::string src =
            "' FreeBASIC program\n"
            "#include once \"fbgfx.bi\"\n"
            "\n"
            "'' Draws something\n"
            "function clamp(byval v as single, lo as single, hi as single) as single\n"
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
        CHECK(r.roots.size() == 6);  // clamp, vec2, keys, p, i, app

        const Symbol* clamp = find(r.roots, "clamp", SymbolKind::Function);
        CHECK(clamp != nullptr);
        CHECK(clamp->children.size() == 3);
        CHECK(clamp->children[0].kind == SymbolKind::Parameter);
        CHECK(clamp->children[0].name == "v");
        CHECK(clamp->doc == " Draws something");

        const Symbol* vec2 = find(r.roots, "vec2", SymbolKind::Type);
        CHECK(vec2 != nullptr);
        CHECK(vec2->children.size() == 2);
        CHECK(vec2->children[0].name == "x");

        const Symbol* keys = find(r.roots, "keys", SymbolKind::Enum);
        CHECK(keys != nullptr);
        CHECK(keys->children.size() == 2);
        CHECK(keys->children[0].name == "key_esc");
        CHECK(keys->children[0].kind == SymbolKind::Const);

        CHECK(find(r.roots, "p", SymbolKind::Dim) != nullptr);
        CHECK(find(r.roots, "i", SymbolKind::Dim) != nullptr);

        const Symbol* app = find(r.roots, "app", SymbolKind::Namespace);
        CHECK(app != nullptr);
        CHECK(app->children.size() == 1);
        CHECK(app->children[0].name == "greet");
        CHECK(app->children[0].kind == SymbolKind::Sub);
        CHECK(app->children[0].children.size() == 1);
        CHECK(app->children[0].children[0].name == "s");
    }

    // Type alias vs UDT vs one-line UDT.
    {
        ParseResult r = parseDocument(
            "type pt\n"
            "x as integer\n"
            "end type\n"
            "type mybyte as byte\n"
            "type pt2 : a as double : end type\n");
        CHECK(r.diagnostics.empty());
        const Symbol* pt = find(r.roots, "pt", SymbolKind::Type);
        CHECK(pt != nullptr);
        CHECK(pt->children.size() == 1);
        CHECK(pt->children[0].name == "x");
        const Symbol* mb = find(r.roots, "mybyte", SymbolKind::Type);
        CHECK(mb != nullptr);
        CHECK(mb->children.empty());
        const Symbol* pt2 = find(r.roots, "pt2", SymbolKind::Type);
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
        ParseResult r = parseDocument("if a then : print 1 : else : print 2 : end if\n");
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

    if (failures == 0)
    {
        std::printf("parser_checks: all passed\n");
        return 0;
    }
    std::printf("parser_checks: %d failures\n", failures);
    return 1;
}