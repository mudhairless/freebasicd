/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// definition, references, documentHighlight, and selectionRange.
//
// The four navigation requests, over a document the client has open. Selection
// ranges are here because they are the request whose reply owns an arena (see
// session_support.h).
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {
// M13 expand selection: the same nested document the byte-offset checks in
// selection_checks use, so every coordinate in the reply can be read straight
// off the source below.
//
//   0: sub outer()
//   1:   dim total = 0
//   2:   if total > 0 then
//   3:     total = total + 1
//   4:   end if
//   5: end sub
char const kDidOpenSelectionFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hello.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"sub outer()\n  dim total = 0\n  if total > 0 then\n    total = total + 1\n  end if\nend sub\n"}}})FB";

// Two positions in one request, on the `total` token and on the `=` after it.
// The result is positional, so two chains come back and result[i] belongs to
// positions[i].
char const *kSelectionRangeFrame =
    R"FB({"jsonrpc":"2.0","id":"sel","method":"textDocument/selectionRange","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hello.bas"},"positions":[)FB"
    R"FB({"line":3,"character":4},{"line":3,"character":10}]}})FB";

// The whole reply for the first position: the token, then the statement, then
// the IF block, then the SUB block. The file is not a level — it would add the
// newline after `end sub` and nothing else.
//
// Asserted as one literal rather than by counting `parent` keys: this is the
// check that the ancestors survive to the wire at all. LspCpp links them as
// non-owning pointers into a per-thread arena, and a reply that lost them would
// carry a single range and every editor's expand-selection would stop dead.
char const kSelectionChain[] =
    R"({"range":{"start":{"line":3,"character":4},"end":{"line":3,"character":9}},)"
    R"("parent":{"range":{"start":{"line":3,"character":4},"end":{"line":3,"character":21}},)"
    R"("parent":{"range":{"start":{"line":2,"character":2},"end":{"line":4,"character":8}},)"
    R"("parent":{"range":{"start":{"line":0,"character":0},"end":{"line":5,"character":7}}}}}})";

// The innermost level of the second chain: the `=` token, so the reply did not
// answer both positions from the first one.
char const kSelectionSecondToken[] =
    R"("range":{"start":{"line":3,"character":10},"end":{"line":3,"character":11}})";

char const *kDefinitionFrame =
    R"FB({"jsonrpc":"2.0","id":"def","method":"textDocument/definition","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/resolve.bas"},"position":{"line":1,"character":0}}})FB";

char const *kReferencesFrame =
    R"FB({"jsonrpc":"2.0","id":"ref","method":"textDocument/references","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/resolve.bas"},"position":{"line":1,"character":0},)FB"
    R"FB("context":{"includeDeclaration":true}}})FB";

char const *kHighlightFrame =
    R"FB({"jsonrpc":"2.0","id":"hl","method":"textDocument/documentHighlight","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/resolve.bas"},"position":{"line":1,"character":0}}})FB";

void TestSelectionRangeNestsParentChain() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  // The capability is asserted before the request: a chain is only worth
  // building if a client is told the feature exists, and a reply that arrives
  // for a capability the server never advertised proves nothing a client can
  // rely on.
  input->append(MakeLspFrame(kInitializeFrame));
  std::string const init = WaitForOutputContaining(output, "\"id\":\"init\"");
  Expect(init.find("\"selectionRangeProvider\":true") != std::string::npos,
         "initialize must advertise selectionRange");

  input->append(MakeLspFrame(kDidOpenSelectionFrame));
  // Counted, not just waited for: the chain is built from the open buffer, so a
  // didOpen that never published is a missing buffer and an empty reply, and
  // `WaitForPublishedUri` hands back the same snapshot when it gives up.
  WaitForPublishedUri(output, 1);
  Expect(CountPublished(output->snapshot()) == 1,
         "didOpen must publish diagnostics for the document the chain reads");

  input->append(MakeLspFrame(kSelectionRangeFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"sel\"");

  Expect(response.find("\"id\":\"sel\"") != std::string::npos,
         "selectionRange request must receive a response");
  Expect(response.find(kSelectionChain) != std::string::npos,
         "the reply must carry the whole nested chain of the first position");
  Expect(response.find(kSelectionSecondToken) != std::string::npos,
         "the reply must answer the second position with a chain of its own");

  session.stop();
}

void TestDefinitionResolvesToDeclaration() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenResolveFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kDefinitionFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"def\"");

  Expect(response.find("\"id\":\"def\"") != std::string::npos,
         "definition request must receive a response");
  Expect(response.find("\"start\":{\"line\":0,\"character\":4}") !=
             std::string::npos,
         "definition must jump to the counter declaration name");
  Expect(response.find("\"end\":{\"line\":0,\"character\":11}") !=
             std::string::npos,
         "definition range must cover the full counter name");

  session.stop();
}

void TestReferencesListAllSites() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenResolveFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kReferencesFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"ref\"");

  Expect(response.find("\"id\":\"ref\"") != std::string::npos,
         "references request must receive a response");
  Expect(response.find("\"start\":{\"line\":0,\"character\":4}") !=
             std::string::npos,
         "references must include the declaration site");
  Expect(response.find("\"start\":{\"line\":1,\"character\":0}") !=
             std::string::npos,
         "references must include the first usage");
  Expect(response.find("\"start\":{\"line\":1,\"character\":10}") !=
             std::string::npos,
         "references must include the second usage of counter");

  session.stop();
}

void TestHighlightCoversAllSites() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenResolveFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kHighlightFrame));
  std::string const response = WaitForOutputContaining(output, "\"id\":\"hl\"");

  Expect(response.find("\"id\":\"hl\"") != std::string::npos,
         "documentHighlight request must receive a response");
  std::size_t highlightCount = 0;
  std::size_t pos = 0;
  while ((pos = response.find("\"start\":", pos)) != std::string::npos) {
    ++highlightCount;
    pos += 8;
  }
  Expect(highlightCount == 3,
         "highlight must cover the declaration and both usages");

  session.stop();
}
} // namespace

namespace fbtest {

void RunNavigationTests() {
  RUN_TEST(TestSelectionRangeNestsParentChain);
  RUN_TEST(TestDefinitionResolvesToDeclaration);
  RUN_TEST(TestReferencesListAllSites);
  RUN_TEST(TestHighlightCoversAllSites);
}

} // namespace fbtest
