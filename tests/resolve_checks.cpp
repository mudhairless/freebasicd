// Identifier resolution checks: FreeBASIC scoping over a parsed document.
// Byte-offset and LSP-agnostic.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#include "index.h"
#include "lexer.h"
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

static std::string const kDoc = "dim total as integer\n"
                                "total = total + 1\n"
                                "sub bump(n as integer)\n"
                                "    dim total as integer\n"
                                "    total = n\n"
                                "    n = 2\n"
                                "end sub\n";

static void TestScopingResolvesCorrectly() {
  AnalyzedDoc const doc = analyze(kDoc);
  CHECK(doc.parse.roots.size() == 2); // dim total, sub bump

  std::size_t const dimTotal = kDoc.find("dim total");
  std::size_t const usage = kDoc.find("total = total");
  std::size_t const localDim = kDoc.find("dim total", kDoc.find("sub bump"));
  std::size_t const localUsage = kDoc.find("total = n");
  std::size_t const nUsage = kDoc.find("n = 2");

  Symbol const *moduleTotal =
      resolveAt(doc, static_cast<std::uint32_t>(dimTotal + 4));
  CHECK(moduleTotal && moduleTotal->kind == SymbolKind::Dim &&
        moduleTotal->name == "total");

  Symbol const *usageTotal = resolveAt(doc, static_cast<std::uint32_t>(usage));
  CHECK_MSG(usageTotal == moduleTotal,
            "module-level usage must resolve to the module dim");

  Symbol const *local =
      resolveAt(doc, static_cast<std::uint32_t>(localDim + 8));
  CHECK(local && local->kind == SymbolKind::Dim && local->name == "total");

  Symbol const *localUse =
      resolveAt(doc, static_cast<std::uint32_t>(localUsage + 4));
  CHECK_MSG(
      localUse == local,
      "a usage inside the sub must resolve to the local dim, shadowing module");

  Symbol const *param = resolveAt(doc, static_cast<std::uint32_t>(nUsage));
  CHECK(param && param->kind == SymbolKind::Parameter && param->name == "n");
}

static void TestUnknownAndNonIdentifiersResolveNull() {
  AnalyzedDoc const doc = analyze(kDoc);

  std::size_t const unknown =
      kDoc.find("1\nsub"); // the literal `1` is a number
  CHECK_MSG(resolveAt(doc, static_cast<std::uint32_t>(unknown)) == nullptr,
            "numbers must be unresolvable");

  std::size_t const keyword = kDoc.find("dim total");
  CHECK_MSG(resolveAt(doc, static_cast<std::uint32_t>(keyword + 2)) == nullptr,
            "keywords must be unresolvable");
}

static void TestOccurrences() {
  AnalyzedDoc const doc = analyze(kDoc);

  std::size_t const dimTotal = kDoc.find("dim total");
  Symbol const *moduleTotal =
      resolveAt(doc, static_cast<std::uint32_t>(dimTotal + 4));
  std::vector<Occurrence> const refs = occurrencesOf(doc, *moduleTotal);
  CHECK_MSG(refs.size() == 2, "module total must be referenced exactly twice");
  for (auto const &r : refs) {
    CHECK_MSG(r.range.beg >= kDoc.find("total = total") &&
                  r.range.beg < kDoc.find("sub bump"),
              "both references sit in the module-level statement");
  }

  std::size_t const nUsage = kDoc.find("n = 2");
  Symbol const *param = resolveAt(doc, static_cast<std::uint32_t>(nUsage));
  std::vector<Occurrence> const paramRefs = occurrencesOf(doc, *param);
  CHECK_MSG(paramRefs.size() == 2,
            "param n must be referenced twice (total=n and n=2)");
  CHECK_MSG(paramRefs.size() >= 1 &&
                paramRefs[0].range.beg < paramRefs[1].range.beg,
            "references must be sorted");
}

static void TestAnalyzeIsDeterministic() {
  // Two analyses of identical content must produce identical projections:
  // token stream, parse tree, and usage sites. Content-addressed caching only
  // serves one analysis per (path, content) when that is true — if a second
  // analysis could ever differ, a cache hit would be a lie.
  AnalyzedDoc const a = analyze(kDoc);
  AnalyzedDoc const b = analyze(kDoc);
  CHECK_MSG(a.tokens.size() == b.tokens.size(),
            "the token stream length is deterministic");
  for (std::size_t i = 0; i < a.tokens.size(); ++i) {
    bool const sameKind = a.tokens[i].kind == b.tokens[i].kind;
    bool const sameRange = a.tokens[i].beg == b.tokens[i].beg &&
                           a.tokens[i].end == b.tokens[i].end;
    CHECK_MSG(sameKind && sameRange,
              "every token's kind and range is deterministic");
  }
  CHECK_MSG(a.parse.roots.size() == b.parse.roots.size(),
            "the symbol tree shape is deterministic");
  for (std::size_t i = 0; i < a.parse.roots.size(); ++i) {
    bool const sameKey = a.parse.roots[i].key == b.parse.roots[i].key;
    bool const sameOcc = a.parse.roots[i].occurrences.size() ==
                         b.parse.roots[i].occurrences.size();
    bool const sameSel =
        a.parse.roots[i].selection.beg == b.parse.roots[i].selection.beg &&
        a.parse.roots[i].selection.end == b.parse.roots[i].selection.end;
    CHECK_MSG(sameKey && sameOcc && sameSel,
              "roots agree on key, selection, and usage-count");
  }
}

static void TestAnalyze() {
  AnalyzedDoc const doc = analyze(kDoc);

  Symbol const *moduleTotal =
      resolveAt(doc, static_cast<std::uint32_t>(kDoc.find("dim total") + 4));
  CHECK(moduleTotal && moduleTotal->name == "total");
  CHECK_MSG(moduleTotal->moduleScope,
            "the module-level dim is a file-root decl");

  std::vector<Occurrence> const refs = occurrencesOf(doc, *moduleTotal);
  CHECK_MSG(refs.size() == 2, "module total must be referenced exactly twice");
  for (auto const &r : refs) {
    CHECK_MSG(r.range.beg >= kDoc.find("total = total") &&
                  r.range.beg < kDoc.find("sub bump"),
              "both references sit in the module-level statement");
    CHECK_MSG(r.moduleScope,
              "module-level usages carry the module-scope site flag");
  }

  std::size_t const localDimStart =
      kDoc.find("dim total", kDoc.find("sub bump"));
  Symbol const *local =
      resolveAt(doc, static_cast<std::uint32_t>(localDimStart + 8));
  CHECK(local && local->kind == SymbolKind::Dim && local->name == "total");
  CHECK_MSG(!local->moduleScope, "a dim inside the sub is not file-scoped");

  std::vector<Occurrence> const localRefs = occurrencesOf(doc, *local);
  CHECK_MSG(localRefs.size() == 1, "the sub-local total has exactly one usage");
  CHECK_MSG(!localRefs[0].moduleScope, "that usage sits inside the sub block");

  Symbol const *param =
      resolveAt(doc, static_cast<std::uint32_t>(kDoc.find("n = 2")));
  CHECK(param && param->kind == SymbolKind::Parameter && param->name == "n");
  CHECK_MSG(occurrencesOf(doc, *param).size() == 2,
            "param n keeps both of its usages");
}

static void TestAnalyzeIncludes() {
  std::string const src = "#define X 1\n"
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

static void TestAnalyzePragmaOnce() {
  AnalyzedDoc const on = analyze("#pragma once\ndim guard as integer\n");
  CHECK_MSG(on.pragmaOnce, "a #pragma once line must set the pragmaOnce flag");

  AnalyzedDoc const ws = analyze("  #pragma once\n");
  CHECK_MSG(ws.pragmaOnce,
            "whitespace before #pragma once must still be detected");

  AnalyzedDoc const withOnce = analyze("#include once \"a.bi\"\n");
  CHECK_MSG(!withOnce.pragmaOnce,
            "#include once is an edge flag, not a #pragma once");

  AnalyzedDoc const commented = analyze("' #pragma once\n");
  CHECK_MSG(!commented.pragmaOnce,
            "a comment quoting #pragma once must not set the flag");

  AnalyzedDoc const other = analyze("#pragma push\n#cmdline \"-d foo\"\n");
  CHECK_MSG(!other.pragmaOnce,
            "a #pragma with a different directive must not set the flag");
}

// The §12.2 storage gate (FreeBASIC.md §8): from inside a procedure body
// (its own control blocks included), a plain module-level Dim is not
// visible — only `Dim Shared` module declarations are. At module level every
// root is visible, and module-level control blocks (scope/for/if/...) inherit
// module scope, so a plain module Dim stays visible inside a SCOPE block.
// This is fbc-probe-verified (error 42 / sc.bas).
static void TestStorageGate() {
  std::string const src = "dim g as integer\n"
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
  Symbol const *moduleG = resolveAt(doc, gOff);
  CHECK_MSG(moduleG && moduleG->kind == SymbolKind::Dim && moduleG->name == "g",
            "a plain module dim is a Dim root at module level");
  CHECK_MSG(moduleG && !moduleG->shared, "plain Dim is not tagged shared");

  std::uint32_t const sOff =
      static_cast<std::uint32_t>(src.find("dim shared") + 11);
  Symbol const *sharedS = resolveAt(doc, sOff);
  CHECK_MSG(sharedS && sharedS->kind == SymbolKind::Dim && sharedS->name == "s",
            "dim shared records a module-level Dim");
  CHECK_MSG(sharedS && sharedS->shared, "Dim Shared is tagged shared");

  // At module level both forms resolve.
  std::uint32_t const moduleUse = static_cast<std::uint32_t>(src.find("g = 1"));
  CHECK_MSG(resolveAt(doc, moduleUse) == nullptr,
            "a gated plain module dim must not resolve from inside a block");

  std::uint32_t const sUse = static_cast<std::uint32_t>(src.find("s = 2"));
  CHECK_MSG(resolveAt(doc, sUse) == sharedS,
            "dim shared is visible inside a block");

  // A usage inside a procedure must not see the plain module dim at all.
  std::uint32_t const staticShared =
      static_cast<std::uint32_t>(src.find("shared st"));
  Symbol const *staticSt = resolveAt(doc, staticShared + 8);
  CHECK_MSG(staticSt && staticSt->kind == SymbolKind::Dim &&
                staticSt->name == "st",
            "static shared parses as a Dim declaration");
  CHECK_MSG(resolveAt(doc, static_cast<std::uint32_t>(src.find("st = 3"))) ==
                staticSt,
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
  for (Symbol const *sym : visibleSymbols(doc, sUse)) {
    keys.push_back(sym->key);
  }
  auto has = [&keys](std::string const &k) {
    return std::find(keys.begin(), keys.end(), k) != keys.end();
  };
  CHECK_MSG(has("s"),
            "visibleSymbols must keep the shared module dim inside a block");
  CHECK_MSG(has("st"), "visibleSymbols must keep the static shared var");
  CHECK_MSG(!has("g"),
            "visibleSymbols must drop a plain module dim inside a block");
}

// Declaration-scope blocks shadow: a `Dim` inside SCOPE/IF/FOR/SELECT (etc.)
// is its own declaration, visible inside its block and gone after it, and it
// may reuse an enclosing name without being a duplicate (BUGS.md, fbc-probed).
static void TestBlockScopesShadowAndDie() {
  std::string const src = "dim x as integer\n"
                          "scope\n"
                          "    dim x as string\n"
                          "    x = \"hi\"\n"
                          "end scope\n"
                          "x = 1\n"
                          "if true then\n"
                          "    dim y as integer\n"
                          "    y = 2\n"
                          "end if\n"
                          "y = 3\n"
                          "sub run()\n"
                          "    dim z as integer\n"
                          "    for i = 1 to 3\n"
                          "        dim z as string\n"
                          "        z = \"loop\"\n"
                          "    next i\n"
                          "    z = 4\n"
                          "end sub\n";
  AnalyzedDoc const doc = analyze(src);

  // Module `x` root, and a module-level SCOPE Scope root holding the block x.
  Symbol const *moduleX =
      resolveAt(doc, static_cast<std::uint32_t>(src.find("dim x") + 4));
  CHECK_MSG(moduleX && moduleX->kind == SymbolKind::Dim,
            "the module x is a Dim root");
  CHECK_MSG(moduleX && moduleX->moduleScope, "the module x is file-scoped");
  Symbol const *blockX = resolveAt(
      doc,
      static_cast<std::uint32_t>(src.find("dim x", src.find("scope")) + 4));
  CHECK_MSG(blockX && blockX->kind == SymbolKind::Dim,
            "the SCOPE-block x is its own Dim");
  CHECK_MSG(blockX && !blockX->moduleScope,
            "a block-local Dim is never file-scoped");

  // The block use resolves to the local x; the after-block use to the module x.
  std::uint32_t const blockUse =
      static_cast<std::uint32_t>(src.find("x = \"hi\""));
  CHECK_MSG(resolveAt(doc, blockUse) == blockX,
            "an in-block use must resolve to the block-local Dim");
  std::uint32_t const afterBlock =
      static_cast<std::uint32_t>(src.find("x = 1\nif"));
  CHECK_MSG(resolveAt(doc, afterBlock) == moduleX,
            "after the SCOPE block the module Dim is visible again");

  // y is declared only inside the IF block: no module `y` exists.
  std::uint32_t const yUse = static_cast<std::uint32_t>(src.find("y = 3"));
  CHECK_MSG(resolveAt(doc, yUse) == nullptr,
            "a block-local name must not resolve after its block");

  // A FOR block nested in a procedure shadows the procedure's Dim; back in the
  // procedure body the outer Dim is visible again.
  std::uint32_t const procZ =
      static_cast<std::uint32_t>(src.find("dim z as integer") + 4);
  std::uint32_t const loopZ =
      static_cast<std::uint32_t>(src.find("dim z as string") + 4);
  std::uint32_t const loopUse = static_cast<std::uint32_t>(src.find("z = \""));
  Symbol const *outerZ = resolveAt(doc, procZ);
  Symbol const *loopLocal = resolveAt(doc, loopZ);
  CHECK_MSG(outerZ && outerZ->kind == SymbolKind::Dim,
            "the procedure z is a Dim");
  CHECK_MSG(loopLocal && loopLocal->kind == SymbolKind::Dim,
            "the FOR-loop z is its own Dim");
  CHECK_MSG(resolveAt(doc, loopUse) == loopLocal,
            "the loop use must resolve to the loop-local Dim");
  std::uint32_t const procUse = static_cast<std::uint32_t>(src.find("z = 4"));
  CHECK_MSG(resolveAt(doc, procUse) == outerZ,
            "after the loop the procedure Dim is visible again");

  // Occurrences split: each Dim collects only its own use.
  CHECK_MSG(occurrencesOf(doc, *moduleX).size() == 1,
            "the module x keeps its after-block usage only");
  CHECK_MSG(occurrencesOf(doc, *blockX).size() == 1,
            "the block x keeps its in-block usage only");
}

// A `for <name> as <type>` counter is a real loop-local Dim (fbc ground
// truth, FreeBASIC.md §8): uses inside the loop resolve to it, nothing is
// visible after `next`, and a header without `as` reuses the enclosing
// declaration instead of declaring (undeclared would be error 42 in fbc).
static void TestForCounterIsLoopLocal() {
  std::string const src = "sub run()\n"
                          "    for i as integer = 0 to 3\n"
                          "        print i\n"
                          "        dim j as integer\n"
                          "        j = i + 1\n"
                          "    next i\n"
                          "    i = 9\n"
                          "    dim k as integer\n"
                          "    for k = 0 to 2\n"
                          "        k = 1\n"
                          "    next k\n"
                          "    k = 4\n"
                          "end sub\n";
  AnalyzedDoc const doc = analyze(src);

  Symbol const *counter =
      resolveAt(doc, static_cast<std::uint32_t>(src.find("for i as") + 4));
  CHECK_MSG(counter && counter->kind == SymbolKind::Dim && counter->loopVar,
            "the `as` counter registers as a loop-local Dim marked loopVar");
  CHECK_MSG(counter && counter->signature.find("for i as integer = 0 to 3") !=
                           std::string::npos,
            "the counter's signature carries the header with its type");
  std::uint32_t const printUse =
      static_cast<std::uint32_t>(src.find("print i") + 6);
  std::uint32_t const bodyUse =
      static_cast<std::uint32_t>(src.find("j = i + 1") + 4);
  CHECK_MSG(resolveAt(doc, printUse) == counter,
            "an in-loop use of the counter resolves to the loop-local Dim");
  CHECK_MSG(resolveAt(doc, bodyUse) == counter,
            "a use inside an initializer resolves to the loop-local Dim");
  std::uint32_t const after = static_cast<std::uint32_t>(src.find("i = 9"));
  CHECK_MSG(resolveAt(doc, after) == nullptr,
            "the `as` counter is invisible after NEXT (fbc ground truth)");

  // A predeclared counter reused without `as` is the very same declaration.
  Symbol const *kDecl =
      resolveAt(doc, static_cast<std::uint32_t>(src.find("dim k") + 4));
  std::uint32_t const reuse =
      static_cast<std::uint32_t>(src.find("for k =") + 4);
  CHECK_MSG(kDecl && kDecl->kind == SymbolKind::Dim, "dim k registers");
  CHECK_MSG(!kDecl->loopVar, "a reused counter is not a new loopVar Dim");
  CHECK_MSG(resolveAt(doc, reuse) == kDecl,
            "a header without `as` reuses the enclosing declaration");
  CHECK_MSG(resolveAt(doc, static_cast<std::uint32_t>(src.find("k = 4"))) ==
                kDecl,
            "the reused counter stays declared after the loop");
}

// A module-level plain Dim stays visible inside a module-level declaration
// scope (FreeBASIC.md §8 probe sc.bas), and the storage gate still applies in
// a procedure-local control block (module plain Dims stay invisible there).
static void TestStorageGateInControlBlocks() {
  std::string const src = "dim g as integer\n"
                          "scope\n"
                          "    g = 1\n"
                          "end scope\n"
                          "sub run()\n"
                          "    scope\n"
                          "        g = 2\n"
                          "    end scope\n"
                          "end sub\n"
                          "if true then\n"
                          "    g = 3\n"
                          "end if\n";
  AnalyzedDoc const doc = analyze(src);
  Symbol const *moduleG =
      resolveAt(doc, static_cast<std::uint32_t>(src.find("dim g") + 4));
  CHECK_MSG(moduleG && moduleG->kind == SymbolKind::Dim,
            "the plain module Dim resolves at its declaration");
  CHECK_MSG(resolveAt(doc, static_cast<std::uint32_t>(src.find("g = 1"))) ==
                moduleG,
            "a SCOPE block at module level inherits the plain module Dim");
  CHECK_MSG(resolveAt(doc, static_cast<std::uint32_t>(src.find("g = 3"))) ==
                moduleG,
            "an IF block at module level inherits the plain module Dim");
  CHECK_MSG(resolveAt(doc, static_cast<std::uint32_t>(src.find("g = 2"))) ==
                nullptr,
            "inside a procedure-local block the plain module Dim is still "
            "storage-gated");
}

// --- M8: occurrencesAcross ---

static std::filesystem::path MakeTmpDir() {
  static std::atomic<long> counter{0};
  std::filesystem::path const root =
      std::filesystem::temp_directory_path() /
      ("fblsp-resolve-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(root);
  return root;
}

static void WriteFile(std::filesystem::path const &path,
                      std::string const &content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// The content seam serves content and its analysis as one pinned unit. The
// analysis borrows the unit's own bytes, so the unit must own them before
// analyze runs.
static std::shared_ptr<DocumentContent const>
MakeContentUnit(std::string content) {
  auto unit = std::make_shared<DocumentContent>();
  unit->content = std::move(content);
  unit->analysis = analyze(unit->content);
  return unit;
}

// Single-file mode (nullptr index): sites of the module dim span its
// declaration and both usages, never the shadowing sub-local; sites of the
// sub-local span only its own declaration and usage, never the module dim.
static void TestOccurrencesAcrossSingleFile() {
  std::string const src = "dim total as integer\n"
                          "total = total + 1\n"
                          "sub bump(n as integer)\n"
                          "    dim total as integer\n"
                          "    total = n\n"
                          "end sub\n";
  std::string const path = "/virtual/single.bas";
  std::shared_ptr<DocumentContent const> const unit = MakeContentUnit(src);
  ContentProvider const content = [&unit, &path](std::string const &p) {
    return p == path ? unit : std::shared_ptr<DocumentContent const>();
  };

  AnalyzedDoc const doc = analyze(src);

  // From the module-level usage: only the module dim's own sites.
  std::uint32_t const moduleUse =
      static_cast<std::uint32_t>(src.find("total = total"));
  std::uint32_t const moduleUse2 =
      static_cast<std::uint32_t>(src.find("total", moduleUse + 1));
  std::uint32_t const moduleDecl =
      static_cast<std::uint32_t>(src.find("dim total") + 4);
  std::vector<OccurrenceSite> const sites =
      occurrencesAcross(doc, path, moduleUse, nullptr, content);
  CHECK_MSG(sites.size() == 3, "the module dim spans decl + two usages");
  if (sites.size() == 3) {
    CHECK(sites[0].file == path && sites[0].range.beg == moduleDecl);
    CHECK(sites[1].file == path && sites[1].range.beg == moduleUse);
    CHECK(sites[2].file == path && sites[2].range.beg == moduleUse2);
    for (auto const &s : sites) {
      CHECK_MSG(s.range.end == s.range.beg + 5, "total is five bytes long");
    }
  }

  // From the sub-local: only its own declaration and usage survive; the
  // module-level dim is a different declaration.
  std::uint32_t const subBegin =
      static_cast<std::uint32_t>(src.find("sub bump"));
  std::uint32_t const localDecl =
      static_cast<std::uint32_t>(src.find("dim total", subBegin) + 4);
  std::uint32_t const localUse =
      static_cast<std::uint32_t>(src.find("total = n"));
  std::vector<OccurrenceSite> const localSites =
      occurrencesAcross(doc, path, localDecl, nullptr, content);
  CHECK_MSG(localSites.size() == 2, "the sub-local spans decl + one usage");
  if (localSites.size() == 2) {
    CHECK(localSites[0].range.beg == localDecl);
    CHECK(localSites[1].range.beg == localUse);
  }

  // A keyword offset resolves nothing.
  std::uint32_t const dimKw =
      static_cast<std::uint32_t>(src.find("dim total") + 1);
  CHECK_MSG(occurrencesAcross(doc, path, dimKw, nullptr, content).empty(),
            "a keyword must yield no rename sites");
}

// Cross-file workspace rename: a shared module dim is renamed from a client
// file; candidates are the closure plus reverse reachability, and every site
// is re-resolved so a shadowing or gated same-named local is untouched.
static void TestOccurrencesAcrossCrossFile() {
  std::string const libContent = "dim shared globalCount as integer\n"
                                 "dim localOnly as integer\n"
                                 "sub libProc()\n"
                                 "    print globalCount\n"
                                 "end sub\n";
  std::string const mainContent = "#include \"lib.bi\"\n"
                                  "dim head as integer\n"
                                  "head = globalCount + localOnly + earlyBird\n"
                                  "sub mainProc()\n"
                                  "    globalCount = globalCount + 1\n"
                                  "    localOnly = 5\n"
                                  "end sub\n";

  std::filesystem::path const sandbox = MakeTmpDir();
  std::filesystem::path const ws = sandbox / "ws";
  std::filesystem::create_directories(ws);
  WriteFile(ws / "lib.bi", libContent);
  WriteFile(ws / "main.bas", mainContent);

  std::string const libNorm = normalizePath(ws / "lib.bi");
  std::string const mainNorm = normalizePath(ws / "main.bas");
  ContentProvider const content = [](std::string const &p) {
    std::ifstream in(std::filesystem::path(p), std::ios::binary);
    if (!in) {
      return std::shared_ptr<DocumentContent const>();
    }
    return MakeContentUnit(std::string(std::istreambuf_iterator<char>(in),
                                       std::istreambuf_iterator<char>()));
  };

  WorkspaceIndex index(ws);
  index.open();
  index.scan(false);
  try {
    AnalyzedDoc const doc = analyze(mainContent);

    // Shared globalCount: decl + libProc use in lib.bi, module + two
    // in-sub usages in main.bas — five sites across two files.
    std::uint32_t const mainModuleUse =
        static_cast<std::uint32_t>(mainContent.find("globalCount"));
    std::vector<OccurrenceSite> const sites =
        occurrencesAcross(doc, mainNorm, mainModuleUse, &index, content);
    CHECK_MSG(sites.size() == 5,
              "shared globalCount covers decl + every usage");
    if (sites.size() == 5) {
      std::uint32_t const libDecl =
          static_cast<std::uint32_t>(libContent.find("globalCount"));
      std::uint32_t const libUse = static_cast<std::uint32_t>(
          libContent.find("globalCount", libDecl + 1));
      std::uint32_t const mainSubLine =
          static_cast<std::uint32_t>(mainContent.find("sub mainProc"));
      std::uint32_t const mainFirst = static_cast<std::uint32_t>(
          mainContent.find("globalCount", mainSubLine));
      std::uint32_t const mainSecond = static_cast<std::uint32_t>(
          mainContent.find("globalCount", mainFirst + 1));
      CHECK(sites[0].file == libNorm && sites[1].file == libNorm);
      CHECK(sites[2].file == mainNorm && sites[3].file == mainNorm &&
            sites[4].file == mainNorm);
      CHECK(sites[0].range.beg == libDecl && sites[1].range.beg == libUse);
      CHECK(sites[2].range.beg == mainModuleUse &&
            sites[3].range.beg == mainFirst &&
            sites[4].range.beg == mainSecond);
    }

    // A plain module dim is visible from module level but storage-gated
    // inside blocks: the in-sub `localOnly = 5` (fbc error 42) is not a
    // rename site.
    std::uint32_t const loModuleUse =
        static_cast<std::uint32_t>(mainContent.find("localOnly"));
    std::vector<OccurrenceSite> const loSites =
        occurrencesAcross(doc, mainNorm, loModuleUse, &index, content);
    CHECK_MSG(loSites.size() == 2,
              "a plain dim keeps its decl + the module usage only");
    if (loSites.size() == 2) {
      std::uint32_t const loDecl =
          static_cast<std::uint32_t>(libContent.find("localOnly"));
      CHECK(loSites[0].file == libNorm && loSites[0].range.beg == loDecl);
      CHECK(loSites[1].file == mainNorm && loSites[1].range.beg == loModuleUse);
    }

    // Shadowing: a same-named local in an included file's block is
    // untouched even though the shared declaration is visible there —
    // tier-1 in-file resolution wins for the local.
    std::string const shLib = "dim shared ticker as integer\n"
                              "sub poke()\n"
                              "    print ticker\n"
                              "end sub\n";
    std::string const shMain = "#include \"sh.lib.bi\"\n"
                               "sub localOnly()\n"
                               "    dim ticker as integer\n"
                               "    ticker = 7\n"
                               "end sub\n"
                               "ticker = ticker + 1\n";
    WriteFile(ws / "sh.lib.bi", shLib);
    WriteFile(ws / "sh.main.bas", shMain);
    index.scan(false);

    std::string const shLibNorm = normalizePath(ws / "sh.lib.bi");
    std::string const shMainNorm = normalizePath(ws / "sh.main.bas");
    AnalyzedDoc const shDoc = analyze(shMain);
    std::uint32_t const shModuleUse =
        static_cast<std::uint32_t>(shMain.find("ticker = ticker + 1"));
    std::vector<OccurrenceSite> const shSites =
        occurrencesAcross(shDoc, shMainNorm, shModuleUse, &index, content);
    CHECK_MSG(shSites.size() == 4,
              "shared ticker: lib decl + lib use + the two module usages");
    if (shSites.size() == 4) {
      std::uint32_t const shLibDecl =
          static_cast<std::uint32_t>(shLib.find("ticker"));
      std::uint32_t const shLibUse =
          static_cast<std::uint32_t>(shLib.find("ticker", shLibDecl + 1));
      CHECK(shSites[0].file == shLibNorm && shSites[0].range.beg == shLibDecl);
      CHECK(shSites[1].file == shLibNorm && shSites[1].range.beg == shLibUse);
      CHECK(shSites[2].file == shMainNorm && shSites[3].file == shMainNorm);
      std::uint32_t const shModuleUse2 =
          static_cast<std::uint32_t>(shMain.find("ticker", shModuleUse + 1));
      CHECK(shSites[2].range.beg == shModuleUse &&
            shSites[3].range.beg == shModuleUse2);
      // No site inside the shadowing local's block (in main.bas).
      std::uint32_t const shadowBeg =
          static_cast<std::uint32_t>(shMain.find("dim ticker"));
      std::uint32_t const shadowEnd =
          static_cast<std::uint32_t>(shMain.find("ticker = 7")) + 6;
      for (auto const &s : shSites) {
        if (s.file != shMainNorm) {
          continue;
        }
        CHECK_MSG(s.range.beg < shadowBeg || s.range.beg > shadowEnd,
                  "shadowing local sites must not be rename targets");
      }
    }
  } catch (...) {
    CHECK_MSG(false, "cross-file occurrencesAcross must not throw");
  }
  index.close();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

int main() {
  TestScopingResolvesCorrectly();
  TestUnknownAndNonIdentifiersResolveNull();
  TestOccurrences();
  TestAnalyze();
  TestAnalyzeIsDeterministic();
  TestAnalyzeIncludes();
  TestAnalyzePragmaOnce();
  TestStorageGate();
  TestBlockScopesShadowAndDie();
  TestForCounterIsLoopLocal();
  TestStorageGateInControlBlocks();
  TestOccurrencesAcrossSingleFile();
  TestOccurrencesAcrossCrossFile();
  std::printf("resolve_checks: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
