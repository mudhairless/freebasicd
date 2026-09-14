// Identifier resolution checks: FreeBASIC scoping over a parsed document.
// Byte-offset and LSP-agnostic.

#include <cstdio>
#include <string>

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

int main()
{
    TestScopingResolvesCorrectly();
    TestUnknownAndNonIdentifiersResolveNull();
    TestOccurrences();
    std::printf("resolve_checks: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}