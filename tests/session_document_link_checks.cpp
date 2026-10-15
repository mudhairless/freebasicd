/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// M16 include navigation.
//
// What the cursor on an `#include` filename literal does: the link a client
// draws over it, and the `textDocument/definition` it answers with the resolved
// header. The two are one question asked two ways, so a client that renders no
// documentLink — Kate's client does not — still navigates the include through
// definition. What the module owns (which includes carry a hit, the ranges) is
// pinned by document_link_checks; this file pins the session half — the
// advertised capability, the resolved target URI, and that an include resolving
// nowhere navigates nowhere rather than to a file that is not there.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {

// The documentLink request frame. Only this file sends it, so it lives here.
std::string DocumentLinkFrame(std::string const &id, std::string const &uri) {
  return R"({"jsonrpc":"2.0","id":")" + id +
         R"(","method":"textDocument/documentLink","params":{"textDocument":{"uri":")" +
         uri + R"("}}})";
}

// The include a document names, when it resolves, becomes one link whose target
// is the resolved header and whose range is the filename literal inside the
// quotes. The unresolved include — a filename that exists nowhere — produces no
// link rather than a pointer at a file that is not there, which is the same
// rule the `include-not-found` diagnostic reports.
void TestDocumentLinkLinksResolvedIncludesOnly() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  // main.bas includes lib.bi, which exists in the workspace; prog.bas is opened
  // with an include of a filename that exists nowhere.
  auto input =
      StartIndexedSession(session, server, output, fix,
                          {{fix.mainUri, kMainContent},
                           {fix.progUri, "#include \"missing.bi\"\n"}});

  std::string const init = WaitForOutputContaining(output, "\"id\":\"init\"");
  Expect(init.find("\"documentLinkProvider\":{\"resolveProvider\":false}") !=
             std::string::npos,
         "initialize must advertise documentLink without a resolve provider");

  std::string const headerTarget = FileUri(fix.wsDir / "lib.bi");
  std::string const literalRange = WireRangeOf(kMainContent, "lib.bi");
  std::string const links = PollRequest(
      input, output, "dl", headerTarget, [&](std::string const &id) {
        return DocumentLinkFrame(id, fix.mainUri);
      });

  Expect(links.find("\"target\":\"" + headerTarget + "\"") != std::string::npos,
         "the include link must point at the resolved header on disk");
  Expect(links.find(literalRange) != std::string::npos,
         "the link range must cover the include's filename literal");
  Expect(CountOf(links, "\"target\":") == 1,
         "main.bas names one include, so it carries one link");
  Expect(links.find("\"data\":") == std::string::npos,
         "resolveProvider is false, so a data field nothing would send back is "
         "noise on the wire");

  // The unresolved include: the document names one, it resolves nowhere, and
  // the reply is empty rather than a dead link. The poll's needle is the empty
  // result, so returning at all is the assertion that nothing was offered.
  std::string const unresolved = PollRequest(
      input, output, "dlp", "\"result\":[]", [&](std::string const &id) {
        return DocumentLinkFrame(id, fix.progUri);
      });
  Expect(unresolved.find("\"target\":") == std::string::npos,
         "an include that resolves to no file must produce no link");

  session.stop();
}

// The same include navigation through `textDocument/definition`, the request
// every client honors: a cursor on the filename literal opens the resolved
// header, and an include that resolves nowhere answers null. The positive
// poll's needle is the header URI, so a reply that comes back before the index
// has settled the include cannot satisfy it — only the resolved answer can. The
// negative poll's needle is the null result, which is the whole assertion: from
// the caller's side a give-up and a wrong location are the same failed test, so
// the unresolved case has to *answer*, not merely time out.
void TestDefinitionOnIncludeOpensTheHeader() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input =
      StartIndexedSession(session, server, output, fix,
                          {{fix.mainUri, kMainContent},
                           {fix.progUri, "#include \"missing.bi\"\n"}});

  std::string const headerTarget = FileUri(fix.wsDir / "lib.bi");
  // main.bas line 0 is `#include "lib.bi"`: the filename literal begins at
  // character 10, so 11 is inside it. Line 0 is the only line either document
  // has an include on.
  auto definitionReply = [&](std::string const &tag, std::string const &needle,
                             std::string const &uri, int character) {
    return PollRequest(input, output, tag, needle, [&](std::string const &id) {
      return R"({"jsonrpc":"2.0","id":")" + id +
             R"(","method":"textDocument/definition","params":{"textDocument":{"uri":")" +
             uri + R"("},"position":{"line":0,"character":)" +
             std::to_string(character) + R"(}}})";
    });
  };

  std::string const resolved =
      definitionReply("incdef", headerTarget, fix.mainUri, 11);
  Expect(resolved.find("\"uri\":\"" + headerTarget + "\"") != std::string::npos,
         "definition on an include must open the resolved header on disk");
  Expect(resolved.find("\"start\":{\"line\":0,\"character\":0}") !=
             std::string::npos,
         "a file target has no symbol to select, so the location is the "
         "header's first line");

  std::string const unresolved =
      definitionReply("incnone", "\"result\":null", fix.progUri, 11);
  Expect(unresolved.find("\"result\":null") != std::string::npos,
         "definition on an unresolved include must answer null");
  Expect(unresolved.find("\"uri\":") == std::string::npos,
         "an unresolved include must not name a file that is not there");

  session.stop();
}

} // namespace

namespace fbtest {

void RunDocumentLinkTests() {
  RUN_TEST(TestDocumentLinkLinksResolvedIncludesOnly);
  RUN_TEST(TestDefinitionOnIncludeOpensTheHeader);
}

} // namespace fbtest
