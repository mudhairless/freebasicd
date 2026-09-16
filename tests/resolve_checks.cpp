// Identifier resolution checks: FreeBASIC scoping over a parsed document.
// Byte-offset and LSP-agnostic.

#include <algorithm>
#include <cstdio>
#include <string>

#include "lexer.h"
#include "parser.h"
#include "resolve.h"
#include "symbols.h"

using namespace fblang;

static int failures = 0;

#define CHECK(cond)                                                                     \
    do                                                                                  \
    {                                                                                   \
        if (!(cond))                                                                    \
        {                                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)

#define CHECK_MSG(cond, msg)                                                            \
    do                                                                                  \
    {                                                                                   \
        if (!(cond))                                                                    \
        {                                                                               \
            std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, msg);       \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)

static std::string const kDoc =
    "dim total as integer\n"
    "total = total + 1\n"
    "sub bump(n as integer)\n"
    "    dim total as integer\n"
    "    total = n\n"
    "    n = 2\n"
    "end sub\n";

static void TestScopingResolvesCorrectly()
{
    auto parse = parseDocument(kDoc);
    CHECK(parse.roots.size() == 2);  // dim total, sub bump

    std::size_t const dimTotal = kDoc.find("dim total");
    std::size_t const usage = kDoc.find("total = total");
    std::size_t const localDim = kDoc.find("dim total", kDoc.find("sub bump"));
    std::size_t const localUsage = kDoc.find("total = n");
    std::size_t const nUsage = kDoc.find("n = 2");

    Symbol const* moduleTotal = resolveAt(parse, kDoc, dimTotal + 4);
    CHECK(moduleTotal && moduleTotal->kind == SymbolKind::Dim && moduleTotal->name == "total");

    Symbol const* usageTotal = resolveAt(parse, kDoc, usage);
    CHECK_MSG(usageTotal == moduleTotal, "module-level usage must resolve to the module dim");

    Symbol const* local = resolveAt(parse, kDoc, localDim + 8);
    CHECK(local && local->kind == SymbolKind::Dim && local->name == "total");

    Symbol const* localUse = resolveAt(parse, kDoc, localUsage + 4);
    CHECK_MSG(localUse == local, "a usage inside the sub must resolve to the local dim, shadowing module");

    Symbol const* param = resolveAt(parse, kDoc, nUsage);
    CHECK(param && param->kind == SymbolKind::Parameter && param->name == "n");
}

static void TestUnknownAndNonIdentifiersResolveNull()
{
    auto parse = parseDocument(kDoc);

    std::size_t const unknown = kDoc.find("1\nsub");  // the literal `1` is a number
    CHECK_MSG(resolveAt(parse, kDoc, unknown) == nullptr, "numbers must be unresolvable");

    std::size_t const keyword = kDoc.find("dim total");
    CHECK_MSG(resolveAt(parse, kDoc, keyword + 2) == nullptr, "keywords must be unresolvable");
}

static void TestOccurrences()
{
    auto parse = parseDocument(kDoc);

    std::size_t const dimTotal = kDoc.find("dim total");
    Symbol const* moduleTotal = resolveAt(parse, kDoc, dimTotal + 4);
    auto refs = occurrencesOf(parse, kDoc, *moduleTotal);
    CHECK_MSG(refs.size() == 2, "module total must be referenced exactly twice");
    for (auto const& r : refs)
    {
        CHECK_MSG(r.beg >= kDoc.find("total = total") && r.beg < kDoc.find("sub bump"),
                  "both references sit in the module-level statement");
    }

    std::size_t const nUsage = kDoc.find("n = 2");
    Symbol const* param = resolveAt(parse, kDoc, nUsage);
    auto paramRefs = occurrencesOf(parse, kDoc, *param);
    CHECK_MSG(paramRefs.size() == 2, "param n must be referenced twice (total=n and n=2)");
    CHECK_MSG(paramRefs.size() >= 1 && paramRefs[0].beg < paramRefs[1].beg,
              "references must be sorted");
}

static void TestAnalyze()
{
    AnalyzedDoc const doc = analyze(kDoc);

    Symbol const* moduleTotal =
        resolveAt(doc, static_cast<std::uint32_t>(kDoc.find("dim total") + 4));
    CHECK(moduleTotal && moduleTotal->name == "total");
    CHECK_MSG(moduleTotal->moduleScope, "the module-level dim is a file-root decl");

    std::vector<Occurrence> const refs = occurrencesOf(doc, *moduleTotal);
    CHECK_MSG(refs.size() == 2, "module total must be referenced exactly twice");
    for (auto const& r : refs)
    {
        CHECK_MSG(r.range.beg >= kDoc.find("total = total") && r.range.beg < kDoc.find("sub bump"),
                  "both references sit in the module-level statement");
        CHECK_MSG(r.moduleScope, "module-level usages carry the module-scope site flag");
    }

    std::size_t const localDimStart = kDoc.find("dim total", kDoc.find("sub bump"));
    Symbol const* local = resolveAt(doc, static_cast<std::uint32_t>(localDimStart + 8));
    CHECK(local && local->kind == SymbolKind::Dim && local->name == "total");
    CHECK_MSG(!local->moduleScope, "a dim inside the sub is not file-scoped");

    std::vector<Occurrence> const localRefs = occurrencesOf(doc, *local);
    CHECK_MSG(localRefs.size() == 1, "the sub-local total has exactly one usage");
    CHECK_MSG(!localRefs[0].moduleScope, "that usage sits inside the sub block");

    Symbol const* param = resolveAt(doc, static_cast<std::uint32_t>(kDoc.find("n = 2")));
    CHECK(param && param->kind == SymbolKind::Parameter && param->name == "n");
    CHECK_MSG(occurrencesOf(doc, *param).size() == 2, "param n keeps both of its usages");
}

static void TestAnalyzeIncludes()
{
    std::string const src =
        "#define X 1\n"
        "#include once \"a.bi\"\n"
        "#include \"b.bi\"\n"
        "#include c.bi\n"
        "#includeonce d.bi\n";
    AnalyzedDoc const doc = analyze(src);
    CHECK(doc.includes.size() == 3);

    CHECK(doc.includes[0].literal == "a.bi");
    CHECK(doc.includes[0].once);
    CHECK(doc.includes[0].line.beg == src.find("#include once"));
    CHECK(doc.includes[0].target.beg == src.find("a.bi"));
    CHECK(doc.includes[0].target.end == src.find("a.bi") + 4);

    CHECK(doc.includes[1].literal == "b.bi");
    CHECK(!doc.includes[1].once);
    CHECK(doc.includes[1].line.beg == src.find("#include \"b.bi\""));
    CHECK(doc.includes[1].target.beg == src.find("b.bi"));
    CHECK(doc.includes[1].target.end == src.find("b.bi") + 4);

    CHECK(doc.includes[2].literal == "c.bi");
    CHECK(!doc.includes[2].once);
    CHECK(doc.includes[2].line.beg == src.find("#include c.bi"));
    CHECK(doc.includes[2].target.beg == src.find("c.bi"));
    CHECK(doc.includes[2].target.end == src.find("c.bi") + 4);
}

static void TestAnalyzePragmaOnce()
{
    AnalyzedDoc const on = analyze("#pragma once\ndim guard as integer\n");
    CHECK_MSG(on.pragmaOnce, "a #pragma once line must set the pragmaOnce flag");

    AnalyzedDoc const ws = analyze("  #pragma once\n");
    CHECK_MSG(ws.pragmaOnce, "whitespace before #pragma once must still be detected");

    AnalyzedDoc const withOnce = analyze("#include once \"a.bi\"\n");
    CHECK_MSG(!withOnce.pragmaOnce, "#include once is an edge flag, not a #pragma once");

    AnalyzedDoc const commented = analyze("' #pragma once\n");
    CHECK_MSG(!commented.pragmaOnce, "a comment quoting #pragma once must not set the flag");

    AnalyzedDoc const other = analyze("#pragma push\n#cmdline \"-d foo\"\n");
    CHECK_MSG(!other.pragmaOnce, "a #pragma with a different directive must not set the flag");
}

// The §12.2 storage gate (FreeBASIC.md §8): from inside any block, a plain
// module-level Dim is not visible — only `Dim Shared` module declarations are.
// At module level everything is. This is fbc-probe-verified (error 42).
static void TestStorageGate()
{
    std::string const src =
        "dim g as integer\n"
        "dim shared s as integer\n"
        "sub run()\n"
        "    dim t as integer\n"
        "    g = 1\n"
        "    s = 2\n"
        "    static shared st as integer\n"
        "    st = 3\n"
        "end sub\n";
    AnalyzedDoc const doc = analyze(src);

    std::uint32_t const gOff = static_cast<std::uint32_t>(src.find("dim g") + 4);
    Symbol const* moduleG = resolveAt(doc, gOff);
    CHECK_MSG(moduleG && moduleG->kind == SymbolKind::Dim && moduleG->name == "g",
              "a plain module dim is a Dim root at module level");
    CHECK_MSG(moduleG && !moduleG->shared, "plain Dim is not tagged shared");

    std::uint32_t const sOff = static_cast<std::uint32_t>(src.find("dim shared") + 11);
    Symbol const* sharedS = resolveAt(doc, sOff);
    CHECK_MSG(sharedS && sharedS->kind == SymbolKind::Dim && sharedS->name == "s",
              "dim shared records a module-level Dim");
    CHECK_MSG(sharedS && sharedS->shared, "Dim Shared is tagged shared");

    // At module level both forms resolve.
    std::uint32_t const moduleUse = static_cast<std::uint32_t>(src.find("g = 1"));
    CHECK_MSG(resolveAt(doc, moduleUse) == nullptr,
              "a gated plain module dim must not resolve from inside a block");

    std::uint32_t const sUse = static_cast<std::uint32_t>(src.find("s = 2"));
    CHECK_MSG(resolveAt(doc, sUse) == sharedS, "dim shared is visible inside a block");

    // A usage inside a procedure must not see the plain module dim at all.
    std::uint32_t const staticShared = static_cast<std::uint32_t>(src.find("shared st"));
    Symbol const* staticSt = resolveAt(doc, staticShared + 8);
    CHECK_MSG(staticSt && staticSt->kind == SymbolKind::Dim && staticSt->name == "st",
              "static shared parses as a Dim declaration");
    CHECK_MSG(resolveAt(doc, static_cast<std::uint32_t>(src.find("st = 3"))) == staticSt,
              "a static shared var is visible where it lives");

    // The occurrence sweep honors the gate too: the gated usage is not an
    // occurrence of the plain module dim, while the shared uses are.
    CHECK_MSG(occurrencesOf(doc, *moduleG).empty(),
              "a gated usage must not be projected as an occurrence");
    CHECK_MSG(occurrencesOf(doc, *sharedS).size() == 1,
              "the visible shared usage stays a real occurrence");

    // visibleSymbols respects the gate for completion: inside the block the
    // shared name and the static are listed, the plain module dim is not.
    std::vector<std::string> keys;
    for (Symbol const* sym : visibleSymbols(doc, sUse))
    {
        keys.push_back(sym->key);
    }
    auto has = [&keys](std::string const& k) {
        return std::find(keys.begin(), keys.end(), k) != keys.end();
    };
    CHECK_MSG(has("s"), "visibleSymbols must keep the shared module dim inside a block");
    CHECK_MSG(has("st"), "visibleSymbols must keep the static shared var");
    CHECK_MSG(!has("g"), "visibleSymbols must drop a plain module dim inside a block");
}

int main()
{
    TestScopingResolvesCorrectly();
    TestUnknownAndNonIdentifiersResolveNull();
    TestOccurrences();
    TestAnalyze();
    TestAnalyzeIncludes();
    TestAnalyzePragmaOnce();
    TestStorageGate();
    std::printf("resolve_checks: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}