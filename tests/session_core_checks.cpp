/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// session lifecycle, pushed diagnostics, and the requests that need no index.
//
// Initialize/shutdown, the didOpen-didChange-didClose notification triangle as
// it publishes diagnostics, documentSymbol, folding, and the watched-files
// capability handshake.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {
// BUGS.md reuse example: a block-local `dim x` shadows the module `dim x`
// (valid FreeBASIC), so must not be reported as a duplicate definition.
char const kDidOpenScopeDupFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hello.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"dim x as integer = 1\nscope\n    dim x as string = \"Hello\"\nend scope\nx = x + 1\n"}}})FB";

char const *kDocumentSymbolFrame =
    R"FB({"jsonrpc":"2.0","id":"dsym","method":"textDocument/documentSymbol","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hello.bas"}}})FB";

char const *kFoldingRangeFrame =
    R"FB({"jsonrpc":"2.0","id":"fold","method":"textDocument/foldingRange","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hello.bas"}}})FB";

char const kDidCloseFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hello.bas"}})FB";

// A client that opts into `workspace.didChangeWatchedFiles` dynamic
// registration; must be registered for it on the `initialized` notification.
char const kInitializeDynamicFrame[] =
    R"FB({"jsonrpc":"2.0","id":"init","method":"initialize","params":{)FB"
    R"FB("capabilities":{"workspace":{"didChangeWatchedFiles":{"dynamicRegistration":true}}}}})FB";

char const *kInitializedFrame =
    R"FB({"jsonrpc":"2.0","method":"initialized","params":{}})FB";

// BUGS.md: a block-local `dim` that shadows an outer name must not be
// reported as a duplicate definition — declaration scopes reuse parent names.
void TestDiagnosticsRespectDeclarationScopes() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenScopeDupFrame));
  std::string const output_all = WaitForPublishedUri(output, 1);

  Expect(output_all.find("\"code\":\"duplicate-definition\"") ==
             std::string::npos,
         "a block-local Dim shadowing an outer Dim is not a duplicate");
  Expect(output_all.find("\"diagnostics\":[]") != std::string::npos,
         "a shadowing block must publish a healthy diagnostic list");

  session.stop();
}

void TestInitializeReportsSyncCapabilities() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kInitializeFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"init\"");

  Expect(response.find("\"id\":\"init\"") != std::string::npos,
         "initialize must receive a response");
  Expect(response.find("\"textDocumentSync\"") != std::string::npos,
         "initialize response must advertise textDocumentSync");
  Expect(response.find("\"openClose\":true") != std::string::npos,
         "textDocumentSync must advertise openClose");
  Expect(response.find("\"change\":2") != std::string::npos,
         "textDocumentSync must advertise incremental changes");
  Expect(response.find("\"documentSymbolProvider\":true") != std::string::npos,
         "initialize response must advertise documentSymbol support");
  Expect(response.find("\"hoverProvider\":true") != std::string::npos,
         "initialize response must advertise hover support");
  Expect(response.find("\"foldingRangeProvider\":true") != std::string::npos,
         "initialize response must advertise foldingRange support");
  Expect(response.find("\"definitionProvider\":true") != std::string::npos,
         "initialize response must advertise definition support");
  Expect(response.find("\"referencesProvider\":true") != std::string::npos,
         "initialize response must advertise references support");
  Expect(response.find("\"documentHighlightProvider\":true") !=
             std::string::npos,
         "initialize response must advertise documentHighlight support");
  Expect(response.find("\"completionProvider\"") != std::string::npos,
         "initialize response must advertise completion support");
  Expect(response.find("\"signatureHelpProvider\"") != std::string::npos,
         "initialize response must advertise signatureHelp support");
  Expect(response.find("\"renameProvider\"") != std::string::npos,
         "initialize response must advertise rename support");
  Expect(response.find("\"prepareProvider\":true") != std::string::npos,
         "initialize response must advertise prepareRename support");
  Expect(response.find("\"codeActionProvider\"") != std::string::npos,
         "initialize response must advertise codeAction support");
  Expect(response.find("\"quickfix\"") != std::string::npos,
         "codeActionProvider must advertise the quickfix kind it serves");

  session.stop();
}

// The version reaches users through the protocol, not only through the
// startup stderr line: `serverInfo` is where the spec puts it, and the client
// that asked for nothing in particular still gets it (the field is
// unconditional — gating it on a capability would only hide it). The asserted
// version is the compile definition itself, so a stale hand-written number
// here fails rather than agreeing with itself.
void TestInitializeReportsServerInfo() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kInitializeFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"init\"");

  Expect(response.find("\"serverInfo\"") != std::string::npos,
         "initialize response must carry serverInfo");
  Expect(response.find("\"name\":\"" + std::string(kServerName) + "\"") !=
             std::string::npos,
         "serverInfo must name the server");
  Expect(response.find("\"version\":\"" + std::string(FBLANG_VERSION) + "\"") !=
             std::string::npos,
         "serverInfo must carry the build's version");

  session.stop();
}

void TestDidOpenPublishesDiagnostics() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenFrame));
  std::string const output_all = WaitForPublishedUri(output, 1);

  Expect(output_all.find(Expand(kUri)) != std::string::npos,
         "publishDiagnostics must carry the opened uri");
  Expect(output_all.find("\"diagnostics\":[]") != std::string::npos,
         "a healthy document must publish no diagnostics");

  session.stop();
}

void TestDiagnosticsReflectParseErrors() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenDupFrame));
  std::string const output_all = WaitForPublishedUri(output, 1);

  Expect(output_all.find("\"code\":\"duplicate-definition\"") !=
             std::string::npos,
         "duplicate dims must be reported with their diagnostic code");
  Expect(output_all.find("duplicate definition: 'x'") != std::string::npos,
         "duplicate dims must be reported with the offending name");
  Expect(output_all.find("\"severity\":2") != std::string::npos,
         "duplicate dims must be reported as warnings");
  Expect(output_all.find("\"source\":\"freebasicd\"") != std::string::npos,
         "diagnostics must carry the server source name");
  Expect(output_all.find("\"start\":{\"line\":1,\"character\":4}") !=
             std::string::npos,
         "the duplicate range must cover the second 'x' name token (byte -> "
         "utf-16)");
  Expect(output_all.find("\"end\":{\"line\":1,\"character\":5}") !=
             std::string::npos,
         "the duplicate range must end after the second 'x' name token");

  session.stop();
}

void TestDocumentSymbolsReturnHierarchy() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenHierFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kDocumentSymbolFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"dsym\"");

  Expect(response.find("\"id\":\"dsym\"") != std::string::npos,
         "documentSymbol request must receive a response");
  Expect(response.find("\"name\":\"greet\"") != std::string::npos,
         "documentSymbol must include the SUB symbol");
  Expect(response.find("\"name\":\"clamp\"") != std::string::npos,
         "documentSymbol must include the FUNCTION symbol");
  Expect(response.find("\"kind\":6") != std::string::npos,
         "SUB must map to the Method symbol kind");
  Expect(response.find("\"kind\":12") != std::string::npos,
         "FUNCTION must map to the Function symbol kind");
  Expect(response.find("\"kind\":253") != std::string::npos,
         "parameters must map to the Parameter symbol kind");
  Expect(response.find("\"children\"") != std::string::npos,
         "nested parameter symbols must be reported as children");
  Expect(response.find("\"selectionRange\"") != std::string::npos,
         "document symbols must carry a selection range for the name token");

  session.stop();
}

// A nameless `enum`, the scope block holding a local, and the selection of the
// nameless one: three outline shapes a client cannot recover on its own (an
// empty name makes Kate graft every later entry under it, a dropped scope
// loses its locals, and a selection outside the range is a click on the wrong
// line). Frames only this test sends.
char const kDidOpenOutlineFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/outline.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"enum\n  red = 1\nend enum\nsub f()\n  dim flag as long\n)FB"
    R"FB(  if flag then\n    dim inner as long\n  end if\nend sub\n"}}})FB";

char const kOutlineSymbolFrame[] =
    R"FB({"jsonrpc":"2.0","id":"dsym2","method":"textDocument/documentSymbol","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/outline.bas"}}})FB";

void TestDocumentSymbolsNameAnonymousAndScopes() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenOutlineFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kOutlineSymbolFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"dsym2\"");

  Expect(response.find("\"id\":\"dsym2\"") != std::string::npos,
         "documentSymbol request must receive a response");
  Expect(response.find("\"name\":\"<anonymous enum>\"") != std::string::npos,
         "a nameless declaration must be named for the outline");
  Expect(response.find("\"name\":\"\"") == std::string::npos,
         "no outline entry may carry an empty name");
  Expect(
      response.find("\"selectionRange\":{\"start\":{\"line\":0,"
                    "\"character\":0},\"end\":{\"line\":0,\"character\":4}}") !=
          std::string::npos,
      "the anonymous declaration selects its own `enum` keyword");
  Expect(response.find("\"name\":\"red\"") != std::string::npos,
         "the anonymous enum's members stay nested under it");
  Expect(response.find("\"name\":\"if..then <scope>\",\"kind\":3") !=
             std::string::npos,
         "the scope directly holding the local is emitted, carrying the `if` "
         "it belongs to, marked `<scope>` and mapped to the Namespace kind");
  Expect(response.find("\"name\":\"if\"") == std::string::npos,
         "the block level declaring nothing directly is spliced out: only the "
         "immediate parent of a declaration earns a level of its own");
  Expect(response.find("\"name\":\"inner\"") != std::string::npos,
         "the local declared inside the scope block reaches the outline");
  Expect(response.find("\"detail\":\"then\"") == std::string::npos,
         "a scope marker is not repeated as its own detail");

  session.stop();
}

// A member implementation is a file-level entry whose identity is the type it
// qualifies: `sub T.proc()` is spelled that way because fbc rejects a
// definition inside the type body (FreeBASIC.md §7), and the outline has to
// carry the qualifier — at file level nothing else says whose proc it is.
// Frames only this test sends.
char const kDidOpenMemberFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/member.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"type T\n  declare sub proc()\nend type\nsub T.proc()\nend sub\n"}}})FB";

char const kMemberSymbolFrame[] =
    R"FB({"jsonrpc":"2.0","id":"dsym3","method":"textDocument/documentSymbol","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/member.bas"}}})FB";

void TestDocumentSymbolsQualifyMemberImplementations() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenMemberFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kMemberSymbolFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"dsym3\"");

  Expect(response.find("\"name\":\"T.proc\"") != std::string::npos,
         "the implementation must be listed as the source spells its "
         "identity, qualifier included: `sub T.proc()` writes `T.proc`");
  Expect(CountOf(response, "\"name\":\"proc\"") == 1,
         "the `declare` inside the type is the only bare `proc`: the "
         "implementation must not also answer as an unqualified one");
  Expect(response.find("\"name\":\"T\"") != std::string::npos,
         "the type itself is still listed at file level");

  session.stop();
}

void TestFoldingRangesReturned() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenHierFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kFoldingRangeFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"fold\"");

  Expect(response.find("\"id\":\"fold\"") != std::string::npos,
         "foldingRange request must receive a response");
  Expect(response.find("\"startLine\":0,\"endLine\":1") != std::string::npos,
         "the SUB block must fold from line 0 up to the END SUB line");
  Expect(
      response.find("\"startLine\":4,\"endLine\":6") != std::string::npos,
      "the FUNCTION block must fold from line 4 up to the END FUNCTION line");

  session.stop();
}

void TestDidChangePushesDiagnostics() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must trigger a publish");

  input->append(MakeLspFrame(kDidChangeFrame));
  std::string const after = WaitForPublishedUri(output, 2);
  Expect(!after.empty() && after.size() > 1,
         "didChange must trigger a publish");

  session.stop();
}

void TestDidCloseEvictsAndPublishes() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must trigger a publish");

  input->append(MakeLspFrame(kDidCloseFrame));
  Expect(WaitForPublishedUri(output, 2).empty() == false,
         "didClose must trigger a clearing publish");

  session.stop();
}

void TestShutdownReturnsNullResult() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kInitializeFrame));
  WaitForOutputContaining(output, "\"id\":\"init\"");

  input->append(MakeLspFrame(kShutdownFrame));
  std::string const response = WaitForOutputContaining(output, "\"id\":2");

  Expect(response.find("\"id\":2") != std::string::npos,
         "shutdown response must preserve the id");
  Expect(response.find("\"result\":null") != std::string::npos,
         "shutdown response result must be null");

  session.stop();
}

void TestExitNotifiesSession() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();
  std::atomic<bool> exited{false};

  FreeBasicServer server(session);
  server.setExitHandler(
      [&exited]() { exited.store(true, std::memory_order_relaxed); });
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kExitFrame));
  for (int i = 0; i < 100 && !exited.load(std::memory_order_relaxed); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  Expect(exited.load(std::memory_order_relaxed),
         "exit notification must signal the exit handler");

  session.stop();
}

void TestInitializeServesStaticWatchersToNonDynamicClient() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kInitializeFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"init\"");

  Expect(response.find("\"didChangeWatchedFiles\"") != std::string::npos,
         "a non-dynamic client must get the static watcher capability in the "
         "initialize reply");
  Expect(response.find("\"globPattern\":\"**/*.{bas,bi}\"") !=
             std::string::npos,
         "the static watcher must watch the FreeBASIC source globs");
  Expect(response.find("\"kind\":7") != std::string::npos,
         "the static watcher must cover create/change/delete events");

  input->append(MakeLspFrame(kInitializedFrame));
  bool sawRegistration = false;
  for (int i = 0; i < 20; ++i) {
    if (output->snapshot().find("\"method\":\"client/registerCapability\"") !=
        std::string::npos) {
      sawRegistration = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(!sawRegistration, "a non-dynamic client must not receive a "
                           "registerCapability request after initialized");

  session.stop();
}

void TestInitializedRegistersWatchedFilesDynamically() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kInitializeDynamicFrame));
  std::string const init = WaitForOutputContaining(output, "\"id\":\"init\"");
  Expect(init.find("\"didChangeWatchedFiles\"") == std::string::npos,
         "a dynamic client must not be served the static watcher capability");

  input->append(MakeLspFrame(kInitializedFrame));
  std::string const registered =
      WaitForOutputContaining(output, "\"client/registerCapability\"");

  Expect(
      registered.find("\"method\":\"workspace/didChangeWatchedFiles\"") !=
          std::string::npos,
      "the registerCapability request must register the watched-files method");
  Expect(registered.find("\"method\":\"client/registerCapability\"") !=
             std::string::npos,
         "the server must send the registerCapability request to the client");
  Expect(registered.find("\"globPattern\":\"**/*.{bas,bi}\"") !=
             std::string::npos,
         "the registration must watch the FreeBASIC source globs");
  Expect(registered.find("\"kind\":7") != std::string::npos,
         "the registration must cover create/change/delete events");

  std::size_t registrationCount = 0;
  std::size_t pos = 0;
  while ((pos = registered.find("\"method\":\"client/registerCapability\"",
                                pos)) != std::string::npos) {
    ++registrationCount;
    pos += 1;
  }
  Expect(
      registrationCount == 1,
      "a dynamic client must receive exactly one registerCapability request");

  session.stop();
}

void TestEndToEndLifecycle() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();
  std::atomic<bool> exited{false};

  FreeBasicServer server(session);
  server.setExitHandler(
      [&exited]() { exited.store(true, std::memory_order_relaxed); });
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kInitializeFrame));
  std::string const init = WaitForOutputContaining(output, "\"id\":\"init\"");
  Expect(init.find("\"change\":2") != std::string::npos,
         "initialize must advertise incremental sync");

  input->append(MakeLspFrame(kDidOpenFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kShutdownFrame));
  std::string const shutdown = WaitForOutputContaining(output, "\"id\":2");
  Expect(shutdown.find("\"result\":null") != std::string::npos,
         "shutdown response result must be null");

  input->append(MakeLspFrame(kExitFrame));
  for (int i = 0; i < 100 && !exited.load(std::memory_order_relaxed); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(exited.load(std::memory_order_relaxed),
         "exit must signal the exit handler after shutdown");

  session.stop();
}
} // namespace

namespace fbtest {

void RunCoreTests() {
  RUN_TEST(TestInitializeReportsSyncCapabilities);
  RUN_TEST(TestInitializeReportsServerInfo);
  RUN_TEST(TestDidOpenPublishesDiagnostics);
  RUN_TEST(TestDiagnosticsReflectParseErrors);
  RUN_TEST(TestDiagnosticsRespectDeclarationScopes);
  RUN_TEST(TestDocumentSymbolsReturnHierarchy);
  RUN_TEST(TestDocumentSymbolsNameAnonymousAndScopes);
  RUN_TEST(TestDocumentSymbolsQualifyMemberImplementations);
  RUN_TEST(TestFoldingRangesReturned);
  RUN_TEST(TestDidChangePushesDiagnostics);
  RUN_TEST(TestDidCloseEvictsAndPublishes);
  RUN_TEST(TestShutdownReturnsNullResult);
  RUN_TEST(TestExitNotifiesSession);
  RUN_TEST(TestInitializeServesStaticWatchersToNonDynamicClient);
  RUN_TEST(TestInitializedRegistersWatchedFilesDynamically);
  RUN_TEST(TestEndToEndLifecycle);
}

} // namespace fbtest
