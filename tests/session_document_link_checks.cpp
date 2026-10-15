/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// M16 document links.
//
// The link a client draws over an `#include` filename literal, and the target
// it opens: the resolved header on disk. What the module owns (which includes
// carry a link, the range) is pinned by document_link_checks; this file pins
// the session half — the advertised capability, the resolved target URI, and
// that an include resolving nowhere produces no link.
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

} // namespace

namespace fbtest {

void RunDocumentLinkTests() {
  RUN_TEST(TestDocumentLinkLinksResolvedIncludesOnly);
}

} // namespace fbtest
