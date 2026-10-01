/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// M15 type go-to + type hierarchy.
//
// `textDocument/typeDefinition`, `textDocument/implementation`, and the
// `textDocument/typeHierarchy` trio, over a fixture whose shapes were checked
// against `fbc 1.10.2` — every layout here compiles, so a green run is evidence
// about FreeBASIC and not about text fbc would reject anyway.
//
// Two facts the fixture had to be built around, both fbc-probed:
// - A UDT holding nothing but `declare sub go()` is error 256 ("cannot be
//   empty"), so every type below carries a field.
// - Only one direction of the member-implementation edge can cross a file
//   boundary, and which one depends on where the `#include` points: a request
//   stands on one half and reaches the other through *its own* include closure.
//   The ordinary layout (a header declares, the includer `.bas` implements) is
//   therefore answered from the `.bas`, and the forward direction needs the
//   other layout — the type declared first, the implementing header included
//   after it — which is what `decl.bas` is.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (the fixture, its documents, and the frames only this
// file sends).

#include "session_support.h"

using namespace fbtest;

namespace {
// The hierarchy. `base_t` declares a member procedure, `derived_t` extends it,
// `lonely_t` extends nothing and is extended by nothing — the shape that must
// still produce a navigable item, with empty lists rather than absent ones.
std::string const kShapesBi = "type base_t\n"
                              "    label as string\n"
                              "    declare sub go()\n"
                              "end type\n"
                              "\n"
                              "type derived_t extends base_t\n"
                              "    extra as integer\n"
                              "end type\n"
                              "\n"
                              "type lonely_t\n"
                              "    only as integer\n"
                              "end type\n";

// The includer: a module-level `sub base_t.go()` implementing the declared
// member, and the variables the type queries ask about.
std::string const kShapesBas = "#include \"shapes.bi\"\n"
                               "\n"
                               "sub base_t.go()\n"
                               "end sub\n"
                               "\n"
                               "sub main()\n"
                               "    dim v as base_t\n"
                               "    dim w as derived_t\n"
                               "    dim i as integer\n"
                               "end sub\n";

// A derived type in a file the requests never open, two levels below base_t:
// the subtype answer has to reach it through the reverse-`extends` projection,
// which is workspace-wide, rather than through the request's closure.
std::string const kFarBas = "#include \"shapes.bi\"\n"
                            "\n"
                            "type far_t extends derived_t\n"
                            "    deep as integer\n"
                            "end type\n";

// The other member-edge layout: the type first, then the implementing header
// included after it, so a request standing on the `declare` reaches the
// implementation through its own closure.
std::string const kDeclBas = "#include \"methods.bi\"\n"
                             "type wide_t\n"
                             "    label as string\n"
                             "    declare sub go()\n"
                             "end type\n"
                             "\n"
                             "sub main2()\n"
                             "    dim v as wide_t\n"
                             "end sub\n";

std::string const kMethodsBi = "sub wide_t.go()\n"
                               "end sub\n";

struct TypeFixture {
  std::filesystem::path sandbox;
  std::filesystem::path wsDir;
  std::string rootUri;
  std::string shapesBiUri;
  std::string shapesBasUri;
  std::string farBasUri;
  std::string declBasUri;
  std::string methodsBiUri;

  TypeFixture() {
    static std::atomic<long> counter{0};
    sandbox = std::filesystem::temp_directory_path() /
              ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
               std::to_string(counter.fetch_add(1)));
    wsDir = sandbox / "ws";
    std::filesystem::create_directories(wsDir);
    auto write = [&](char const *name, std::string const &text) {
      std::ofstream out(wsDir / name);
      out << text;
    };
    write("shapes.bi", kShapesBi);
    write("shapes.bas", kShapesBas);
    write("far.bas", kFarBas);
    write("decl.bas", kDeclBas);
    write("methods.bi", kMethodsBi);
    rootUri = FileUri(wsDir);
    shapesBiUri = FileUri(wsDir / "shapes.bi");
    shapesBasUri = FileUri(wsDir / "shapes.bas");
    farBasUri = FileUri(wsDir / "far.bas");
    declBasUri = FileUri(wsDir / "decl.bas");
    methodsBiUri = FileUri(wsDir / "methods.bi");
  }

  ~TypeFixture() {
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
  }
};

// The harness in session_support.h starts a session against TwoFileFixture,
// whose six documents none of these tests use; this is the same ten lines
// against this file's own workspace. `opens` is handed to didOpen in order.
std::shared_ptr<FeedableIStream> StartTypeSession(
    lsp::LanguageSession &session, FreeBasicServer &server,
    std::shared_ptr<StringOStream> const &output, TypeFixture const &fix,
    std::vector<std::pair<std::string, std::string>> const &opens) {
  auto input = std::make_shared<FeedableIStream>();
  server.registerHandlers();
  session.start(input, output);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      fix.rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");
  for (auto const &open : opens) {
    std::string const frame =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
        R"({"uri":")" +
        open.first + R"(","languageId":"basic","version":1,"text":")" +
        ToJsonString(open.second) + "\"}}}";
    input->append(MakeLspFrame(frame.c_str()));
  }
  return input;
}

// One position request, and its reply's tail. `needle` locates the token the
// cursor sits on; `at` is how far into it the position goes, which matters
// where a needle is a prefix of another (`type base_t` vs `base_t.go`).
std::string PositionReply(std::shared_ptr<FeedableIStream> const &input,
                          std::shared_ptr<StringOStream> const &output,
                          std::string const &id, std::string const &method,
                          std::string const &uri, std::string const &position) {
  input->append(
      MakeLspFrame((R"({"jsonrpc":"2.0","id":")" + id + R"(","method":")" +
                    method + R"(","params":{"textDocument":{"uri":")" + uri +
                    R"("},"position":)" + position + "}}")
                       .c_str()));
  return TailAfter(WaitForOutputContaining(output, "\"" + id + "\""),
                   "\"" + id + "\"");
}

// One typeHierarchy follow-up request carrying an item the client echoes back,
// and its reply's tail. `kind` is 23 (Struct), what a `type` is sent as; the
// follow-ups identify the item by `uri` + `selectionRange` and read neither
// the name nor the kind, but the field is not optional on the wire.
std::string FollowUpReply(std::shared_ptr<FeedableIStream> const &input,
                          std::shared_ptr<StringOStream> const &output,
                          std::string const &id, std::string const &method,
                          std::string const &item) {
  input->append(
      MakeLspFrame((R"({"jsonrpc":"2.0","id":")" + id + R"(","method":")" +
                    method + R"(","params":{"item":)" + item + "}}")
                       .c_str()));
  return TailAfter(WaitForOutputContaining(output, "\"" + id + "\""),
                   "\"" + id + "\"");
}

// An item as a client carries it between the three requests: the type's name
// token for `selectionRange`, and the whole declaration for `range`.
std::string HierarchyItemJson(std::string const &uri, std::string const &src,
                              std::string const &name,
                              std::string const &from) {
  std::size_t const at = src.find(from);
  return R"({"name":")" + name + R"(","kind":23,"uri":")" + uri +
         R"(","range":)" + WireRangeBetween(src, from, "end type") +
         R"(,"selectionRange":)" + WireRangeOf(src, name, at) + "}";
}
} // namespace

namespace {
// typeDefinition answers a `Location`, so the two go-to requests share one
// shape: the name token of the type, in the file that declares it.
void TestTypeDefinitionFollowsTheAsClause() {
  TypeFixture fix;
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  // Both files are opened, so every range below is measured against the buffer
  // the client sent — and the header's ranges are measured against *its*
  // buffer, which is not the .bas's bytes.
  auto input = StartTypeSession(
      session, server, output, fix,
      {{fix.shapesBasUri, kShapesBas}, {fix.shapesBiUri, kShapesBi}});

  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"typeDefinitionProvider\":true") != std::string::npos,
         "initialize must advertise typeDefinition");

  std::string const baseName = WireRangeOf(kShapesBi, "base_t");

  // `dim v as base_t`: the type is declared in a header, one include away.
  std::string const atV =
      WirePositionOf(kShapesBas, "base_t", kShapesBas.find("dim v"));
  std::string const v =
      PositionReply(input, output, "tdv", "textDocument/typeDefinition",
                    fix.shapesBasUri, atV);
  Expect(v.find("\"uri\":\"" + fix.shapesBiUri + "\"") != std::string::npos,
         "typeDefinition of `dim v as base_t` must land in the declaring "
         "header, not the document the request came from");
  Expect(v.find(baseName) != std::string::npos,
         "typeDefinition must select the type's name token, not its whole "
         "declaration");
  Expect(v.find(kShapesBi) == std::string::npos,
         "the reply carries positions, not source text");

  // `dim w as derived_t` picks the derived type, not the one it extends.
  std::string const atW =
      WirePositionOf(kShapesBas, "derived_t", kShapesBas.find("dim w"));
  std::string const w =
      PositionReply(input, output, "tdw", "textDocument/typeDefinition",
                    fix.shapesBasUri, atW);
  Expect(w.find(WireRangeOf(kShapesBi, "derived_t")) != std::string::npos,
         "`dim w as derived_t` is of type derived_t, and that is where the "
         "request must land");

  // The module-level `sub base_t.go()` is of the type it qualifies.
  std::string const atImpl =
      WirePositionOf(kShapesBas, "base_t.go()", kShapesBas.find("sub base_t"));
  std::string const impl =
      PositionReply(input, output, "tdm", "textDocument/typeDefinition",
                    fix.shapesBasUri, atImpl);
  Expect(impl.find(baseName) != std::string::npos,
         "a member implementation is of the type it qualifies");

  // Nothing to go to: a builtin type, and a plain sub with no `as` at all.
  // Both answer null, which is what stops the client offering the affordance.
  std::string const atI = WirePositionOf(kShapesBas, "integer");
  std::string const i =
      PositionReply(input, output, "tdi", "textDocument/typeDefinition",
                    fix.shapesBasUri, atI);
  Expect(i.find("\"result\":null") != std::string::npos,
         "a builtin type has no declaration to go to, so the answer is null");

  std::string const atMain = WirePositionOf(kShapesBas, "sub main", 4);
  std::string const main =
      PositionReply(input, output, "tdn", "textDocument/typeDefinition",
                    fix.shapesBasUri, atMain);
  Expect(main.find("\"result\":null") != std::string::npos,
         "a procedure carrying no type must answer null rather than a guess");

  session.stop();
}

// The other two shapes a cursor can ask about: a member of a type body, which
// is of the type that declares it, and a type name, which is its own type.
void TestTypeDefinitionInsideATypeBody() {
  TypeFixture fix;
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartTypeSession(
      session, server, output, fix,
      {{fix.shapesBasUri, kShapesBas}, {fix.shapesBiUri, kShapesBi}});

  // The field `label`, inside base_t's body: of base_t, even though the field
  // declares `as string`. The enclosing type is the question a cursor in a
  // type body can answer; a builtin is not a navigable answer.
  std::string const atLabel = WirePositionOf(kShapesBi, "label");
  std::string const field =
      PositionReply(input, output, "tdf", "textDocument/typeDefinition",
                    fix.shapesBiUri, atLabel);
  Expect(field.find(WireRangeOf(kShapesBi, "base_t")) != std::string::npos,
         "a field inside a type body is of the type that declares it");

  // The cursor already on a type name — the shape a client sends prepare for.
  std::string const atSelf =
      WirePositionOf(kShapesBi, "derived_t", kShapesBi.find("type derived_t"));
  std::string const self =
      PositionReply(input, output, "tds", "textDocument/typeDefinition",
                    fix.shapesBiUri, atSelf);
  Expect(self.find(WireRangeOf(kShapesBi, "derived_t")) != std::string::npos,
         "a type is its own type: the cursor on `type derived_t` lands there");

  session.stop();
}

// The two edges have a display consequence, and this is the surface that
// showed it: `extends` used to be captured as a field of the type, and
// `sub base_t.go()` used to be a root keyed `base_t`, so the outline listed
// `base_t` twice and drew a lens for a symbol that does not exist.
void TestDocumentSymbolShowsTheEdgeNotItsParts() {
  TypeFixture fix;
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartTypeSession(
      session, server, output, fix,
      {{fix.shapesBasUri, kShapesBas}, {fix.shapesBiUri, kShapesBi}});

  input->append(MakeLspFrame(
      (R"({"jsonrpc":"2.0","id":"dsym","method":"textDocument/documentSymbol","params":{"textDocument":{"uri":")" +
       fix.shapesBasUri + "\"}}}")
          .c_str()));
  std::string const bas = TailAfter(
      WaitForOutputContaining(output, "\"id\":\"dsym\""), "\"id\":\"dsym\"");
  Expect(bas.find("\"name\":\"base_t\"") == std::string::npos,
         "the qualifier of a member implementation is not a symbol of its "
         "own: `sub base_t.go()` used to be listed as a second `base_t`, and "
         "that is the duplicate outline entry this milestone removes");
  Expect(CountOf(bas, "\"name\":\"go\"") == 1 &&
             bas.find("\"detail\":\"sub base_t.go()\"") != std::string::npos,
         "the implementation must be listed once, under its member name and "
         "with its qualifier in the detail — which is what makes it "
         "addressable");

  input->append(MakeLspFrame(
      (R"({"jsonrpc":"2.0","id":"dsym2","method":"textDocument/documentSymbol","params":{"textDocument":{"uri":")" +
       fix.shapesBiUri + "\"}}}")
          .c_str()));
  std::string const bi = TailAfter(
      WaitForOutputContaining(output, "\"id\":\"dsym2\""), "\"id\":\"dsym2\"");
  Expect(bi.find("\"name\":\"extends\"") == std::string::npos,
         "`extends` is a clause of the opener line, never a field of the type");
  Expect(CountOf(bi, "\"name\":\"base_t\"") == 1,
         "the type body lists `declare sub go()`, and the type appears once "
         "however many members it declares");
  Expect(bi.find("\"name\":\"derived_t\"") != std::string::npos &&
             bi.find("\"name\":\"lonely_t\"") != std::string::npos,
         "every type in the header must still be listed");

  session.stop();
}

// The member-implementation edge, once per direction and once per layout.
void TestImplementationCrossesBothWays() {
  TypeFixture fix;
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartTypeSession(session, server, output, fix,
                                {{fix.shapesBasUri, kShapesBas},
                                 {fix.shapesBiUri, kShapesBi},
                                 {fix.declBasUri, kDeclBas},
                                 {fix.methodsBiUri, kMethodsBi}});

  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"implementationProvider\":true") != std::string::npos,
         "initialize must advertise implementation");

  // Forward, in the ordinary layout: the `.bas` implements what the header
  // declares, so the cursor on `sub base_t.go()` reaches the `declare`.
  std::string const atGo =
      WirePositionOf(kShapesBas, "go()", kShapesBas.find("sub base_t"));
  std::string const forward =
      PositionReply(input, output, "impf", "textDocument/implementation",
                    fix.shapesBasUri, atGo);
  Expect(forward.find("\"uri\":\"" + fix.shapesBiUri + "\"") !=
             std::string::npos,
         "implementation must reach the declare in the header that declares "
         "it, from the .bas that implements it");
  Expect(forward.find(WireRangeOf(kShapesBi, "go")) != std::string::npos,
         "the answer must be the declare's name token, measured against the "
         "header's bytes and not the .bas's");

  // Backward, in the other layout: the type is declared first and the
  // implementing header included after it, so the `declare`'s own closure holds
  // the implementation.
  std::string const atDecl =
      WirePositionOf(kDeclBas, "go()", kDeclBas.find("declare sub"));
  std::string const backward =
      PositionReply(input, output, "impb", "textDocument/implementation",
                    fix.declBasUri, atDecl);
  Expect(backward.find("\"uri\":\"" + fix.methodsBiUri + "\"") !=
             std::string::npos,
         "in the layout that includes the implementing header, the declare "
         "must reach the implementation across a file");

  // The far end is a function, never a fan-out, because fbc rejects a
  // re-implementation of an inherited member (error 158). One location.
  Expect(CountOf(forward, "\"uri\":") == 1,
         "one declared member has exactly one implementation, so the reply "
         "carries one location");
  Expect(CountOf(backward, "\"uri\":") == 1,
         "one implementation answers exactly one declare");

  // Not on the edge: a field has no implementation, and a plain sub belongs to
  // no type. Both are empty answers rather than the nearest declaration.
  std::string const atField = WirePositionOf(kShapesBi, "label");
  std::string const field =
      PositionReply(input, output, "imp0", "textDocument/implementation",
                    fix.shapesBiUri, atField);
  Expect(field.find("\"result\":null") != std::string::npos,
         "a field inside a type body has no implementation to go to");

  std::string const atSub = WirePositionOf(kShapesBas, "sub main", 4);
  std::string const sub =
      PositionReply(input, output, "imp1", "textDocument/implementation",
                    fix.shapesBasUri, atSub);
  Expect(sub.find("\"result\":null") != std::string::npos,
         "a module-level sub with no dot implements nothing");

  session.stop();
}

// prepare returns one item, with both halves already filled — the capability
// carries no resolveProvider, so a client is never left polling for them.
void TestTypeHierarchyPrepareFillsBothDirections() {
  TypeFixture fix;
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartTypeSession(
      session, server, output, fix,
      {{fix.shapesBasUri, kShapesBas}, {fix.shapesBiUri, kShapesBi}});

  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"typeHierarchyProvider\":true") != std::string::npos,
         "initialize must advertise typeHierarchy");
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"typeHierarchyProvider\":{") == std::string::npos,
         "the advertised capability must be the bare bool, which carries no "
         "resolveProvider: this build implements no typeHierarchy/resolve, "
         "and a client that is told to resolve must never be sent one");

  // The cursor is on `dim w as derived_t`, in the .bas, and the item names the
  // type in the header — one item, both lists filled.
  std::string const atW =
      WirePositionOf(kShapesBas, "derived_t", kShapesBas.find("dim w"));
  std::string const prep =
      PositionReply(input, output, "thp", "textDocument/typeHierarchy",
                    fix.shapesBasUri, atW);
  Expect(CountOf(prep, "\"name\":\"") == 3,
         "prepare returns one item, and derived_t has one base and one "
         "subtype — three names in all");
  Expect(prep.find("\"name\":\"derived_t\"") != std::string::npos,
         "prepare must name the type under the cursor");
  Expect(prep.find("\"parents\":[{\"name\":\"base_t\"") != std::string::npos,
         "the item's parents must be filled in prepare, not left unresolved");
  Expect(prep.find("\"children\":[{\"name\":\"far_t\"") != std::string::npos,
         "the item's children must be filled in prepare, not left unresolved");
  Expect(prep.find("\"uri\":\"" + fix.shapesBiUri + "\"") != std::string::npos,
         "the prepared item must name the file that declares the type");
  Expect(prep.find("\"detail\":\"type derived_t extends base_t\"") !=
             std::string::npos,
         "the item's detail is the opener line, which is where the client "
         "shows what the type is");
  Expect(prep.find("\"data\":") == std::string::npos,
         "the prepared item must carry no data field: uri and selectionRange "
         "already identify it, and nothing here would read a blob back");

  // A type that extends nothing and is extended by nothing still gets an
  // item — with the lists present and empty, so a client renders a leaf
  // instead of a node it must ask twice for.
  std::string const atLonely = WirePositionOf(kShapesBi, "lonely_t");
  std::string const lonely =
      PositionReply(input, output, "thl", "textDocument/typeHierarchy",
                    fix.shapesBiUri, atLonely);
  Expect(lonely.find("\"name\":\"lonely_t\"") != std::string::npos,
         "a base-less type is still navigable");
  Expect(lonely.find("\"parents\":[]") != std::string::npos &&
             lonely.find("\"children\":[]") != std::string::npos,
         "an empty hierarchy is two empty lists, not two absent fields");

  // Nothing to anchor on: a module-level procedure has no type, so there is
  // no item and the client offers no affordance. The reply carries no
  // `result` key at all, because LspCpp's serializer omits an absent optional
  // member — the same frame M13's prepareCallHierarchy gets, and a client
  // reading `result` sees `undefined`, which this feature's own types admit.
  std::string const atMain = WirePositionOf(kShapesBas, "sub main", 4);
  std::string const atModule =
      PositionReply(input, output, "thm", "textDocument/typeHierarchy",
                    fix.shapesBasUri, atMain);
  Expect(atModule.find("\"name\":") == std::string::npos &&
             atModule.find("\"result\":[]") == std::string::npos,
         "prepare with no type under the cursor must answer no item at all");

  session.stop();
}

// The two follow-ups, answered from the item the client echoes back rather
// than from a document and a position.
void TestTypeHierarchyFollowUpsFromTheEchoedItem() {
  TypeFixture fix;
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  // far.bas is written to disk and never opened: the subtype answer has to
  // reach it through the workspace-wide reverse-`extends` projection.
  auto input = StartTypeSession(
      session, server, output, fix,
      {{fix.shapesBiUri, kShapesBi}, {fix.shapesBasUri, kShapesBas}});

  std::string const baseItem =
      HierarchyItemJson(fix.shapesBiUri, kShapesBi, "base_t", "type base_t");
  std::string const derivedItem = HierarchyItemJson(
      fix.shapesBiUri, kShapesBi, "derived_t", "type derived_t");
  std::string const lonelyItem = HierarchyItemJson(fix.shapesBiUri, kShapesBi,
                                                   "lonely_t", "type lonely_t");

  // supertypes: the whole chain above, nearest first, each name measured
  // against the file that declares it.
  std::string const sup = FollowUpReply(
      input, output, "supd", "typeHierarchy/supertypes", derivedItem);
  Expect(CountOf(sup, "\"name\":\"") == 1 &&
             sup.find("\"name\":\"base_t\"") != std::string::npos,
         "derived_t extends base_t and nothing else, so supertypes is exactly "
         "base_t");

  // subtypes: the whole subtree, including a type two levels down in a file
  // this request never opened.
  std::string const sub = PollRequest(
      input, output, "subb", "\"uri\":\"" + fix.farBasUri + "\"",
      [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"typeHierarchy/subtypes","params":{"item":)" +
               baseItem + "}}";
      });
  Expect(sub.find("\"name\":\"derived_t\"") != std::string::npos,
         "base_t is extended directly by derived_t");
  Expect(sub.find("\"name\":\"far_t\"") != std::string::npos,
         "subtypes is the whole subtree, so far_t two levels down is included "
         "even though its file was never opened");
  Expect(sub.find(WireRangeOf(kFarBas, "far_t")) != std::string::npos,
         "an item for a file the client never opened must still carry ranges "
         "measured against that file's own bytes");
  Expect(sub.find("\"parents\":") == std::string::npos &&
             sub.find("\"children\":") == std::string::npos,
         "the follow-ups answer a flat list; only prepare nests, so a client "
         "that recurses on a follow-up's items walks nothing");

  // A leaf answers with empty lists, not null: the item was found, and it
  // simply has no subtypes.
  std::string const leaf = FollowUpReply(input, output, "subl",
                                         "typeHierarchy/subtypes", lonelyItem);
  Expect(leaf.find("\"result\":[]") != std::string::npos,
         "a type nothing extends has no subtypes, which is an empty list");
  std::string const root = FollowUpReply(input, output, "supl",
                                         "typeHierarchy/supertypes", baseItem);
  Expect(root.find("\"result\":[]") != std::string::npos,
         "a type that extends nothing has no supertypes");

  // An item the server cannot identify answers null rather than a type picked
  // by name: a (uri, name-token range) pair is the identity, and a client that
  // trimmed the range should learn that instead of being sent elsewhere.
  std::string const trimmed =
      R"({"name":"derived_t","kind":23,"uri":")" + fix.shapesBiUri +
      R"(","range":)" +
      WireRangeBetween(kShapesBi, "type derived_t", "end type") +
      R"(,"selectionRange":{"start":{"line":0,)"
      R"("character":0},"end":{"line":0,"character":4}}})";
  std::string const none =
      FollowUpReply(input, output, "supx", "typeHierarchy/supertypes", trimmed);
  Expect(none.find("\"name\":") == std::string::npos,
         "an item whose selectionRange is not a type's name token must answer "
         "no item at all, not the nearest type");

  session.stop();
}
} // namespace

namespace fbtest {

void RunTypeHierarchyTests() {
  RUN_TEST(TestTypeDefinitionFollowsTheAsClause);
  RUN_TEST(TestTypeDefinitionInsideATypeBody);
  RUN_TEST(TestDocumentSymbolShowsTheEdgeNotItsParts);
  RUN_TEST(TestImplementationCrossesBothWays);
  RUN_TEST(TestTypeHierarchyPrepareFillsBothDirections);
  RUN_TEST(TestTypeHierarchyFollowUpsFromTheEchoedItem);
}

} // namespace fbtest