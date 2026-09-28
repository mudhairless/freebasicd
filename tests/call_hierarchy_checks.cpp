/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Call-hierarchy checks for the M13 scan module.
//
// Byte-offset and LSP-agnostic, like the module: no index, no session. The only
// workspace state the scans reach is the `CalleeResolver` seam, and these
// checks wire it to the real in-file resolvers (`resolveAt` for a bare name,
// `resolveMemberAccess` for a member) with a null index, so the shape rules and
// the identity matching are exercised against the same lookups the session
// uses, minus the workspace.
//
// Every expected site is a needle plus the snippet it sits after, so an
// expectation names the call it means and a fixture that moves stays honest.

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "call_hierarchy.h"
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

namespace {

char const kPath[] = "/tmp/w.bas";

// The call shapes and what surrounds them: member Sub/Function calls, a
// property read that is *not* a call, a bare statement call with and without
// arguments, a Function in an expression, recursion, a local that shadows a Sub
// of the same name, and a module-level call with no caller to be.
char const kFixture[] = "type counter\n"
                        "    sub bump()\n"
                        "    end sub\n"
                        "    sub twice()\n"
                        "        bump()\n"
                        "        bump()\n"
                        "    end sub\n"
                        "    function peek() as integer\n"
                        "        return 0\n"
                        "    end function\n"
                        "    property get count() as integer\n"
                        "        return 0\n"
                        "    end property\n"
                        "end type\n"
                        "\n"
                        "sub helper(n as integer)\n"
                        "end sub\n"
                        "\n"
                        "function double_it(v as integer) as integer\n"
                        "    return v * 2\n"
                        "end function\n"
                        "\n"
                        "sub recur(n as integer)\n"
                        "    if n > 0 then recur(n - 1)\n"
                        "end sub\n"
                        "\n"
                        "sub shadowed()\n"
                        "    dim helper as integer\n"
                        "    helper 1\n"
                        "end sub\n"
                        "\n"
                        "sub caller()\n"
                        "    dim c as counter\n"
                        "    dim x as integer\n"
                        "    c.bump()\n"
                        "    x = c.peek()\n"
                        "    x = c.count\n"
                        "    helper(1)\n"
                        "    helper 2\n"
                        "    recur(3)\n"
                        "    x = double_it(4)\n"
                        "end sub\n"
                        "\n"
                        "helper 9\n";

// Every way a member call is written: a reserved word as a member name
// (`sub open()` is legal and the parser records it), `->` on a pointer, a
// chained access through a field, and the `with`-implicit leading dot.
char const kMemberFixture[] = "type sink\n"
                              "    sub open()\n"
                              "    end sub\n"
                              "    sub close()\n"
                              "    end sub\n"
                              "    sub push(v as integer)\n"
                              "    end sub\n"
                              "end type\n"
                              "\n"
                              "type outer\n"
                              "    sub deep()\n"
                              "    end sub\n"
                              "    field as sink\n"
                              "end type\n"
                              "\n"
                              "sub user()\n"
                              "    dim s as sink\n"
                              "    dim p as sink pointer\n"
                              "    dim o as outer\n"
                              "    s.open()\n"
                              "    s.close()\n"
                              "    p->open()\n"
                              "    o.field.push(1)\n"
                              "    with s\n"
                              "        .open()\n"
                              "    end with\n"
                              "end sub\n";

AnalyzedDoc const &fixture() {
  static std::string const src = kFixture;
  static AnalyzedDoc const doc = analyze(src);
  return doc;
}

AnalyzedDoc const &memberFixture() {
  static std::string const src = kMemberFixture;
  static AnalyzedDoc const doc = analyze(src);
  return doc;
}

// The seam the session wires, minus the index: a bare name (both the `name(`
// and the bare-statement shape) resolves in-file, a member identifier goes
// through the member resolver.
CalleeResolver inFileResolver() {
  return [](AnalyzedDoc const &doc, CalleeRef const &ref) -> CrossDecl {
    if (ref.shape == CalleeRef::Shape::Member) {
      MemberAccess const member =
          resolveMemberAccess(doc, kPath, ref.off, nullptr);
      return CrossDecl{member.file, member.member};
    }
    if (Symbol const *local = resolveAt(doc, ref.off)) {
      return CrossDecl{nullptr, local};
    }
    return {};
  };
}

// Offset of `needle`, searching from `from`. A missing needle is a broken
// fixture, reported as such rather than as a wrong answer.
std::size_t find(std::string const &src, std::string const &needle,
                 std::size_t from = 0) {
  std::size_t const found = src.find(needle, from);
  if (found == std::string::npos) {
    std::printf("FAIL fixture is missing the needle \"%s\"\n", needle.c_str());
    ++failures;
  }
  return found;
}

// The token spelled `needle`, searching from byte `from` — a call site's range
// on the wire is exactly this token.
SourceRange site(std::string const &src, std::string const &needle,
                 std::size_t from = 0) {
  std::size_t const beg = find(src, needle, from);
  return SourceRange{static_cast<std::uint32_t>(beg),
                     static_cast<std::uint32_t>(beg + needle.size())};
}

// The same token, anchored on a snippet on the same line — what tells two calls
// of one name apart (`helper(1)` and `helper 2`).
SourceRange onLine(std::string const &src, std::string const &anchor,
                   std::string const &needle) {
  return site(src, needle, find(src, anchor));
}

std::string ranges(std::initializer_list<SourceRange> rs) {
  std::string out;
  for (SourceRange const &r : rs) {
    if (!out.empty()) {
      out += ",";
    }
    out += std::to_string(r.beg) + "-" + std::to_string(r.end);
  }
  return out;
}

std::string text(std::vector<SourceRange> const &rs) {
  std::string out;
  for (SourceRange const &r : rs) {
    if (!out.empty()) {
      out += ",";
    }
    out += std::to_string(r.beg) + "-" + std::to_string(r.end);
  }
  return out;
}

// Outgoing calls as one line: `name[sites] name[sites]`, in reported order.
std::string outgoingText(std::vector<OutgoingCall> const &calls) {
  std::string out;
  for (OutgoingCall const &call : calls) {
    if (!out.empty()) {
      out += " ";
    }
    out += call.to.name + "[" + text(call.sites) + "]";
  }
  return out;
}

// The same for the incoming half: `caller[sites]`.
std::string incomingText(std::vector<IncomingCall> const &calls) {
  std::string out;
  for (IncomingCall const &call : calls) {
    if (!out.empty()) {
      out += " ";
    }
    out += call.caller->name + "[" + text(call.sites) + "]";
  }
  return out;
}

void expect(char const *what, std::string const &got,
            std::string const &expected) {
  if (got != expected) {
    std::printf("FAIL %s: got \"%s\", expected \"%s\"\n", what, got.c_str(),
                expected.c_str());
    ++failures;
  }
}

void expectOutgoing(char const *what, std::vector<OutgoingCall> const &calls,
                    std::string const &expected) {
  expect(what, outgoingText(calls), expected);
}

void expectIncoming(char const *what, std::vector<IncomingCall> const &calls,
                    std::string const &expected) {
  expect(what, incomingText(calls), expected);
}

// The first symbol named `name` in the tree, so no test hardcodes an offset to
// name its subject.
Symbol const *symNamed(AnalyzedDoc const &doc, std::string const &name) {
  for (auto const &root : doc.parse.roots) {
    if (root.name == name) {
      return &root;
    }
    for (auto const &child : root.children) {
      if (child.name == name) {
        return &child;
      }
    }
  }
  return nullptr;
}

// The identity a follow-up request is answered against: the declaring file plus
// the name token. The wire's version of the same pair.
DeclIdentity identityOf(AnalyzedDoc const &doc, std::string const &name) {
  Symbol const *const sym = symNamed(doc, name);
  return DeclIdentity{kPath, sym->selection.beg, sym->selection.end};
}

// --- the enclosing-procedure lookup ------------------------------------------

void TestProcedureAt() {
  std::string const src = kFixture;
  AnalyzedDoc const &doc = fixture();

  // On a name token, on a Dim, and inside a nested block: the `if ... then
  // <call>` in `recur` belongs to the Sub, not to the `if`, and the `return` in
  // `double_it` belongs to the Function.
  CHECK(procedureAt(doc, static_cast<std::uint32_t>(find(src, "caller"))) ==
        symNamed(doc, "caller"));
  CHECK(procedureAt(doc, static_cast<std::uint32_t>(find(src, "dim c as"))) ==
        symNamed(doc, "caller"));
  CHECK(procedureAt(doc, static_cast<std::uint32_t>(find(
                             src, "recur(n - 1)"))) == symNamed(doc, "recur"));
  CHECK(
      procedureAt(doc, static_cast<std::uint32_t>(find(src, "return v * 2"))) ==
      symNamed(doc, "double_it"));

  // Innermost wins: a member procedure is not the Type around it, and a call
  // inside a member belongs to the member.
  CHECK(procedureAt(doc, static_cast<std::uint32_t>(find(src, "bump"))) ==
        symNamed(doc, "bump"));
  SourceRange const firstBump = onLine(src, "twice", "bump");
  CHECK(procedureAt(doc, firstBump.beg) == symNamed(doc, "twice"));

  // Module level has no procedure; neither has a type body with no member
  // procedure under the cursor, because the walk out finds no procedure.
  CHECK(procedureAt(doc, static_cast<std::uint32_t>(find(src, "helper 9"))) ==
        nullptr);
  CHECK(procedureAt(doc, static_cast<std::uint32_t>(find(src, "counter"))) ==
        nullptr);
  // Past the end of the document, and in a document with nothing in it.
  CHECK(procedureAt(doc, static_cast<std::uint32_t>(src.size())) == nullptr);
  AnalyzedDoc const empty = analyze("");
  CHECK(procedureAt(empty, 0) == nullptr);
}

// --- outgoing calls ----------------------------------------------------------

void TestOutgoingCallShapes() {
  std::string const src = kFixture;
  AnalyzedDoc const &doc = fixture();
  CalleeResolver const resolve = inFileResolver();

  // One entry per callee in source order, with `helper`'s two call forms merged
  // (`helper(1)` and the paren-less `helper 2`, which fbc 1.10.2 accepts in
  // `fb` mode). The property read `c.count` is not a call edge, and `caller`'s
  // own name token is not a call to itself.
  expectOutgoing("caller",
                 outgoingCalls(doc, kPath, symNamed(doc, "caller"), resolve),
                 "bump[" + ranges({onLine(src, "c.bump", "bump")}) + "] peek[" +
                     ranges({onLine(src, "c.peek", "peek")}) + "] helper[" +
                     ranges({onLine(src, "helper(1)", "helper"),
                             onLine(src, "helper 2", "helper")}) +
                     "] recur[" + ranges({onLine(src, "recur(3)", "recur")}) +
                     "] double_it[" +
                     ranges({onLine(src, "double_it(4)", "double_it")}) + "]");

  // A member procedure calling its own type's other member twice: the two
  // identical lines are told apart by searching past the first.
  SourceRange const first = onLine(src, "twice", "bump");
  expectOutgoing(
      "twice", outgoingCalls(doc, kPath, symNamed(doc, "twice"), resolve),
      "bump[" + ranges({first, site(src, "bump", first.beg + 1)}) + "]");

  // A body with no calls at all.
  expectOutgoing("helper",
                 outgoingCalls(doc, kPath, symNamed(doc, "helper"), resolve),
                 "");
}

void TestOutgoingMemberShapes() {
  std::string const src = kMemberFixture;
  AnalyzedDoc const &doc = memberFixture();
  CalleeResolver const resolve = inFileResolver();

  // `s.open()`, `p->open()` and the `with`-implicit `.open()` are one callee
  // reached three ways: merged because the merge key is the declaration's
  // identity, not the token.
  expectOutgoing(
      "user", outgoingCalls(doc, kPath, symNamed(doc, "user"), resolve),
      "open[" +
          ranges({onLine(src, "s.open", "open"), onLine(src, "p->open", "open"),
                  site(src, "open", find(src, "with s"))}) +
          "] close[" + ranges({onLine(src, "s.close", "close")}) + "] push[" +
          ranges({onLine(src, "o.field.push", "push")}) + "]");
}

void TestOutgoingExcludesNonCalls() {
  std::string const src = kFixture;
  AnalyzedDoc const &doc = fixture();
  CalleeResolver const resolve = inFileResolver();

  // A local `dim helper` shadows the Sub `helper`, so `helper 1` in that body
  // is a variable reference, not a call. Matching the name would have reported
  // one; resolving the site does not.
  expectOutgoing("shadowed",
                 outgoingCalls(doc, kPath, symNamed(doc, "shadowed"), resolve),
                 "");

  // A declaration's own signature line is not a call to itself: `sub helper(`
  // and `sub recur(` are both `name(` at a statement head, and only the one
  // inside the body is a call.
  expectOutgoing("recur",
                 outgoingCalls(doc, kPath, symNamed(doc, "recur"), resolve),
                 "recur[" + ranges({onLine(src, "then recur", "recur")}) + "]");

  // A null caller answers empty rather than walking the whole document, and a
  // null resolver answers empty rather than claiming every name is a call.
  CHECK(outgoingCalls(doc, kPath, nullptr, resolve).empty());
  CHECK(outgoingCalls(doc, kPath, symNamed(doc, "caller"), CalleeResolver{})
            .empty());
}

// --- incoming calls ----------------------------------------------------------

void TestIncomingGroupedByCaller() {
  std::string const src = kFixture;
  AnalyzedDoc const &doc = fixture();
  CalleeResolver const resolve = inFileResolver();

  // Two callers of the member Sub `bump`, the first calling it twice.
  SourceRange const first = onLine(src, "twice", "bump");
  expectIncoming(
      "bump", incomingCallsIn(doc, kPath, identityOf(doc, "bump"), resolve),
      "twice[" + ranges({first, site(src, "bump", first.beg + 1)}) +
          "] caller[" + ranges({onLine(src, "c.bump", "bump")}) + "]");

  // One caller, two call forms.
  expectIncoming(
      "helper", incomingCallsIn(doc, kPath, identityOf(doc, "helper"), resolve),
      "caller[" +
          ranges({onLine(src, "helper(1)", "helper"),
                  onLine(src, "helper 2", "helper")}) +
          "]");

  // Recursion: the Sub calls itself, so it is a caller of itself as well as
  // being called by `caller`.
  expectIncoming(
      "recur", incomingCallsIn(doc, kPath, identityOf(doc, "recur"), resolve),
      "recur[" + ranges({onLine(src, "then recur", "recur")}) + "] caller[" +
          ranges({onLine(src, "recur(3)", "recur")}) + "]");

  // A Function called from an expression is a call.
  expectIncoming(
      "double_it",
      incomingCallsIn(doc, kPath, identityOf(doc, "double_it"), resolve),
      "caller[" + ranges({onLine(src, "double_it(4)", "double_it")}) + "]");
  expectIncoming("peek",
                 incomingCallsIn(doc, kPath, identityOf(doc, "peek"), resolve),
                 "caller[" + ranges({onLine(src, "c.peek", "peek")}) + "]");
}

void TestIncomingExcludesNonCallables() {
  AnalyzedDoc const &doc = fixture();
  CalleeResolver const resolve = inFileResolver();

  // A variable's identity matches no call site: the scan resolves every
  // call-shaped token, and none of them names the local `x`.
  expectIncoming("local variable",
                 incomingCallsIn(doc, kPath, identityOf(doc, "x"), resolve),
                 "");
  // So does an identity that names no declaration at all, and one from another
  // file.
  CHECK(
      incomingCallsIn(doc, kPath, DeclIdentity{kPath, 3, 7}, resolve).empty());
  CHECK(incomingCallsIn(doc, kPath, DeclIdentity{"/other.bas", 0, 0}, resolve)
            .empty());
  // A null resolver answers empty rather than reporting every call-shaped
  // token.
  CHECK(incomingCallsIn(doc, kPath, identityOf(doc, "helper"), CalleeResolver{})
            .empty());
}

void TestIncomingDropsModuleLevelCall() {
  std::string const src = kFixture;
  AnalyzedDoc const &doc = fixture();
  CalleeResolver const resolve = inFileResolver();

  // The trailing `helper 9` is a call to `helper` at module level: no enclosing
  // procedure to be the `from` node, so it is not reported. Both sites inside
  // `caller` are still there, which is what proves the call was dropped rather
  // than the scan short-circuited.
  expectIncoming(
      "helper with a module-level call",
      incomingCallsIn(doc, kPath, identityOf(doc, "helper"), resolve),
      "caller[" +
          ranges({onLine(src, "helper(1)", "helper"),
                  onLine(src, "helper 2", "helper")}) +
          "]");
}

// --- the item copy -----------------------------------------------------------

void TestCallItemCarriesTheNode() {
  AnalyzedDoc const &doc = fixture();
  Symbol const *const caller = symNamed(doc, "caller");
  CallItem const item = callItemOf(*caller, kPath);
  CHECK(item.file == kPath);
  CHECK(item.name == "caller");
  CHECK(item.kind == SymbolKind::Sub);
  CHECK(item.range.beg == caller->range.beg &&
        item.range.end == caller->range.end);
  CHECK(item.selection.beg == caller->selection.beg &&
        item.selection.end == caller->selection.end);
  CHECK(item.detail == caller->signature);
  CHECK(!item.detail.empty());
}

// --- the invariants, exhaustively --------------------------------------------

// Two properties that must hold at every offset, not only at the ones a case
// above happened to pick: a procedure's range contains every site attributed to
// it, and one callee is never two entries (the merge is by identity).
void checkInvariants(std::string const &src) {
  AnalyzedDoc const doc = analyze(src);
  CalleeResolver const resolve = inFileResolver();

  for (std::uint32_t at = 0; at <= src.size(); ++at) {
    Symbol const *const proc = procedureAt(doc, at);
    if (proc == nullptr) {
      continue;
    }
    CHECK(proc->range.beg <= at && at <= proc->range.end);
  }

  std::vector<Symbol const *> all;
  for (auto const &root : doc.parse.roots) {
    all.push_back(&root);
    for (auto const &child : root.children) {
      all.push_back(&child);
    }
  }
  for (Symbol const *sym : all) {
    std::vector<DeclIdentity> seen;
    for (OutgoingCall const &call : outgoingCalls(doc, kPath, sym, resolve)) {
      CHECK(!call.sites.empty());
      for (SourceRange const &at : call.sites) {
        CHECK(at.beg >= sym->range.beg && at.end <= sym->range.end);
      }
      DeclIdentity const id{call.to.file, call.to.selection.beg,
                            call.to.selection.end};
      for (DeclIdentity const &other : seen) {
        CHECK(!(other == id));
      }
      seen.push_back(id);
    }
  }
}

} // namespace

int main() {
  TestProcedureAt();
  TestOutgoingCallShapes();
  TestOutgoingMemberShapes();
  TestOutgoingExcludesNonCalls();
  TestIncomingGroupedByCaller();
  TestIncomingExcludesNonCallables();
  TestIncomingDropsModuleLevelCall();
  TestCallItemCarriesTheNode();

  checkInvariants(kFixture);
  checkInvariants(kMemberFixture);
  checkInvariants("");
  checkInvariants("sub a()\nend sub\n");
  checkInvariants("sub unterminated()\n  a()\n");
  checkInvariants("type t\n  sub m()\n  end sub\nend type\nsub u()\n"
                  "  dim v as t\n  v.m()\n  w()\nend sub\n");

  if (failures == 0) {
    std::printf("call_hierarchy_checks: all passed\n");
    return 0;
  }
  std::printf("call_hierarchy_checks: %d failures\n", failures);
  return 1;
}
