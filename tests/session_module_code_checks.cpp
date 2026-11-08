/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Module-level executable code outside the main module.
//
// A `.bas` file's module level runs as the implicit `main` — unless the file
// is not the main module, in which case it runs before `main` as load-time
// initialization, silently and in an order fbc does not specify. Which module
// is main comes from the root's `freebasicd.toml` (`[build]`): the server
// must never guess. An exe without a configured main is never judged; a
// library (dll/staticlib) has no entry point, so every module is judged; a
// `.bi` header never is — its statements execute in the including module.
//
// Both deliveries must agree ("one payload, two deliveries"): pull and push
// are each pinned below for the configured cases, and the config-change test
// drives one session across a `main` flip and a `kind` flip.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {

// The text of the main module: executable body, declarations, and both a
// procedure definition and a module constructor (all of which a fix or the
// lint itself must leave alone — the main module is the entry point's home).
char const kMainText[] =
    "dim total as integer\nprint \"main\"\nsub work()\n  print 1\nend sub\n";
char const kSecondText[] = "print \"second\"\nprint \"again\"\n";
char const kHeaderText[] =
    "print \"header\"\ndim shared sharedVal as integer\n";

struct ModuleFixture {
  std::filesystem::path sandbox;
  std::filesystem::path ws;
  std::string rootUri;
  std::string mainUri;
  std::string secondUri;
  std::string headerUri;
  std::string declUri;

  ModuleFixture() {
    static std::atomic<long> counter{0};
    sandbox = std::filesystem::temp_directory_path() /
              ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
               std::to_string(counter.fetch_add(1)));
    ws = sandbox / "ws";
    std::filesystem::create_directories(ws);
    rootUri = FileUri(ws);
    mainUri = FileUri(ws / "main.bas");
    secondUri = FileUri(ws / "second.bas");
    headerUri = FileUri(ws / "head.bi");
    declUri = FileUri(ws / "decl.bas");
  }

  ~ModuleFixture() {
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
  }
};

void WriteFile(std::filesystem::path const &path, char const *text) {
  std::ofstream out(path);
  out << text;
}

void WriteToml(std::filesystem::path const &ws, char const *text) {
  WriteFile(ws / "freebasicd.toml", text);
}

std::string OpenFrame(std::string const &uri, std::string const &text) {
  return std::string(
             R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
             R"({"uri":")") +
         uri + R"(","languageId":"basic","version":1,"text":")" +
         ToJsonString(text) + "\"}}}";
}

std::string PullFrame(std::string const &id, std::string const &uri) {
  return std::string(
             R"({"jsonrpc":"2.0","id":")" + id +
             R"(","method":"textDocument/diagnostic","params":{"textDocument":{"uri":")") +
         uri + "\"}}}";
}

std::string PullFrameWithPrevious(std::string const &id, std::string const &uri,
                                  std::string const &previous) {
  return std::string(
             R"({"jsonrpc":"2.0","id":")" + id +
             R"(","method":"textDocument/diagnostic","params":{"textDocument":{"uri":")") +
         uri + "\",\"previousResultId\":\"" + previous + "\"}}}";
}

std::string RootPullInitialize(std::string const &rootUri) {
  return R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"capabilities":{"textDocument":{"diagnostic":{"relatedDocumentSupport":true}},"workspace":{"diagnostics":{"refreshSupport":true}}},"rootUri":")" +
         rootUri + "\"}}";
}

std::string RootPushInitialize(std::string const &rootUri) {
  return R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
         rootUri + "\"}}";
}

// An exe with a named main: the second module fires (one run, anchored on its
// first statement, naming the configured main), and the main module itself,
// the declarations-only module, and the header are all silent.
void TestExeWithMainFiresOutsideMain() {
  ModuleFixture fix;
  WriteToml(fix.ws, "[build]\nkind = \"exe\"\nmain = \"main.bas\"\n");
  WriteFile(fix.ws / "main.bas", kMainText);
  WriteFile(fix.ws / "second.bas", kSecondText);
  WriteFile(fix.ws / "decl.bas", "dim a as integer\nconst k = 2\n");
  WriteFile(fix.ws / "head.bi", kHeaderText);

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(RootPullInitialize(fix.rootUri).c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"diagnosticProvider\"") != std::string::npos,
         "initialize must advertise the pull provider");
  input->append(MakeLspFrame(OpenFrame(fix.mainUri, kMainText).c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.secondUri, kSecondText).c_str()));
  input->append(MakeLspFrame(
      OpenFrame(fix.declUri, "dim a as integer\nconst k = 2\n").c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.headerUri, kHeaderText).c_str()));

  std::string const second = PollRequest(
      input, output, "mc1", "\"kind\":\"full\"",
      [&](std::string const &id) { return PullFrame(id, fix.secondUri); });
  Expect(CountOf(second, "\"code\":\"module-code-outside-main\"") == 1,
         "the non-main module must pull exactly one run diagnostic");
  Expect(second.find("\"severity\":2") != std::string::npos,
         "the diagnostic is a Warning (severity 2)");
  Expect(
      second.find("\"message\":\"module-level code runs before the "
                  "program's entry point as load-time initialization, in "
                  "an order fbc does not specify (main module: main.bas)\"") !=
          std::string::npos,
      "the message names the configured main module");
  Expect(second.find("\"start\":{\"line\":0,\"character\":0},\"end\":"
                     "{\"line\":0,\"character\":14}") != std::string::npos,
         "the diagnostic anchors on the run's first statement");

  std::string const mainReport = PollRequest(
      input, output, "mc2", "\"kind\":\"full\"",
      [&](std::string const &id) { return PullFrame(id, fix.mainUri); });
  Expect(mainReport.find("module-code-outside-main") == std::string::npos,
         "the main module's body IS the entry point: silent");

  std::string const declReport = PollRequest(
      input, output, "mc3", "\"kind\":\"full\"",
      [&](std::string const &id) { return PullFrame(id, fix.declUri); });
  Expect(declReport.find("module-code-outside-main") == std::string::npos,
         "a declarations-only module runs no code: silent");

  std::string const headerReport = PollRequest(
      input, output, "mc4", "\"kind\":\"full\"",
      [&](std::string const &id) { return PullFrame(id, fix.headerUri); });
  Expect(headerReport.find("module-code-outside-main") == std::string::npos,
         "a header executes in the including module's context: silent");

  session.stop();
}

// An exe with no configured main — and a project with no `freebasicd.toml`
// at all — reports exactly what it reported before the lint existed: nothing.
// The main module is unknowable without config, so the server stays silent.
void TestExeWithoutMainNeverFires() {
  ModuleFixture fix;
  // No toml written: the defaults (`kind = "exe"`, no main).
  WriteFile(fix.ws / "main.bas", kMainText);
  WriteFile(fix.ws / "second.bas", kSecondText);

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(RootPushInitialize(fix.rootUri).c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"diagnosticProvider\"") == std::string::npos,
         "a legacy client keeps the push path");
  input->append(MakeLspFrame(OpenFrame(fix.mainUri, kMainText).c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.secondUri, kSecondText).c_str()));
  std::string const both = WaitForPublishedUri(output, 2);
  Expect(CountPublished(both) == 2, "both opens must publish diagnostics");
  Expect(both.find("module-code-outside-main") == std::string::npos,
         "with no configured main, no file anywhere may be judged");

  session.stop();
}

// A library has no entry point, so every module's module-level code is
// load-time initialization: any file with a run fires, worded for a target
// with no main — and the push delivery carries it too.
void TestLibraryFiresEverywhere() {
  ModuleFixture fix;
  WriteToml(fix.ws, "[build]\nkind = \"dll\"\n");
  WriteFile(fix.ws / "main.bas", kMainText);
  WriteFile(fix.ws / "second.bas", kSecondText);
  WriteFile(fix.ws / "head.bi", kHeaderText);

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(RootPushInitialize(fix.rootUri).c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"diagnosticProvider\"") == std::string::npos,
         "a legacy client keeps the push path");
  input->append(MakeLspFrame(OpenFrame(fix.mainUri, kMainText).c_str()));
  std::string const mainPublish = WaitForPublishedUri(output, 1);
  Expect(
      LastPublish(mainPublish).find("\"code\":\"module-code-outside-main\"") !=
          std::string::npos,
      "in a library even the first module's body is load-time init");
  Expect(LastPublish(mainPublish)
                 .find("\"message\":\"module-level code runs as "
                       "load-time initialization for a target with no entry "
                       "point, in an order fbc does not specify\"") !=
             std::string::npos,
         "the push publish carries the library wording");

  input->append(MakeLspFrame(OpenFrame(fix.secondUri, kSecondText).c_str()));
  std::string const secondPublish = WaitForPublishedUri(output, 2);
  Expect(LastPublish(secondPublish)
                 .find("\"code\":\"module-code-outside-main\"") !=
             std::string::npos,
         "every module of a library fires");

  input->append(MakeLspFrame(OpenFrame(fix.headerUri, kHeaderText).c_str()));
  std::string const headPublish = WaitForPublishedUri(output, 3);
  Expect(LastPublish(headPublish).find("module-code-outside-main") ==
             std::string::npos,
         "a library header is still a header: silent");

  session.stop();
}

// Runs: a `Dim` between two statements splits them into two diagnostics; a
// module-level `If` block and its neighbours are one run.
void TestRunInterruptedByDimAndIfSpan() {
  ModuleFixture fix;
  WriteToml(fix.ws, "[build]\nkind = \"exe\"\nmain = \"main.bas\"\n");
  WriteFile(fix.ws / "main.bas", kMainText);
  WriteFile(fix.ws / "second.bas", kSecondText);
  std::string const splitText = "print 1\ndim x as integer\nprint 2\n";
  std::string const spanText = "print 1\nif 1 then\n  print 2\nend if\n";
  std::string const splitUri = FileUri(fix.ws / "split.bas");
  std::string const spanUri = FileUri(fix.ws / "span.bas");
  WriteFile(fix.ws / "split.bas", splitText.c_str());
  WriteFile(fix.ws / "span.bas", spanText.c_str());

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(RootPullInitialize(fix.rootUri).c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"diagnosticProvider\"") != std::string::npos,
         "initialize must advertise the pull provider");
  input->append(MakeLspFrame(OpenFrame(splitUri, splitText).c_str()));
  input->append(MakeLspFrame(OpenFrame(spanUri, spanText).c_str()));

  std::string const split = PollRequest(
      input, output, "mc5", "\"kind\":\"full\"",
      [&](std::string const &id) { return PullFrame(id, splitUri); });
  Expect(CountOf(split, "\"code\":\"module-code-outside-main\"") == 2,
         "a `Dim` between executable statements produces two diagnostics");

  std::string const span = PollRequest(
      input, output, "mc6", "\"kind\":\"full\"",
      [&](std::string const &id) { return PullFrame(id, spanUri); });
  Expect(CountOf(span, "\"code\":\"module-code-outside-main\"") == 1,
         "a run spanning a module-level `If` block is one diagnostic");

  session.stop();
}

// Changing `build.main` (or `kind`) changes the file's delivered diagnostics
// and its `diagnosticsResultId`-relevant payload: a client holding the old
// report must not be told `unchanged`.
void TestConfigChangeMovesReport() {
  ModuleFixture fix;
  WriteToml(fix.ws, "[build]\nkind = \"exe\"\nmain = \"main.bas\"\n");
  WriteFile(fix.ws / "main.bas", kMainText);
  WriteFile(fix.ws / "second.bas", kSecondText);

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(RootPullInitialize(fix.rootUri).c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"diagnosticProvider\"") != std::string::npos,
         "initialize must advertise the pull provider");
  input->append(MakeLspFrame(OpenFrame(fix.secondUri, kSecondText).c_str()));

  std::string const before = PollRequest(
      input, output, "mc7", "\"kind\":\"full\"",
      [&](std::string const &id) { return PullFrame(id, fix.secondUri); });
  Expect(before.find("\"code\":\"module-code-outside-main\"") !=
             std::string::npos,
         "under `main = main.bas` the second module must fire");
  std::string const beforeId = ResultIdOf(before);
  Expect(!beforeId.empty(), "the full report must carry a resultId");

  // This file becomes the main module: the diagnostic must go, and the id
  // must move so the client is not told `unchanged`.
  WriteToml(fix.ws, "[build]\nkind = \"exe\"\nmain = \"second.bas\"\n");
  input->append(MakeLspFrame(kDidChangeConfigurationFrame));
  std::string const afterMain = PollRequest(
      input, output, "mc8", "\"kind\":", [&](std::string const &id) {
        return PullFrameWithPrevious(id, fix.secondUri, beforeId);
      });
  Expect(afterMain.find("\"kind\":\"full\"") != std::string::npos,
         "a `main` flip must recompute, not `unchanged`");
  Expect(afterMain.find("module-code-outside-main") == std::string::npos,
         "once this file is main, the diagnostic must go");
  std::string const afterMainId = ResultIdOf(afterMain);
  Expect(!afterMainId.empty() && afterMainId != beforeId,
         "changing `build.main` must move the resultId");

  // The same file under a library kind fires again — with the library
  // wording and a moved id.
  WriteToml(fix.ws, "[build]\nkind = \"dll\"\n");
  input->append(MakeLspFrame(kDidChangeConfigurationFrame));
  std::string const afterKind = PollRequest(
      input, output, "mc9", "\"kind\":", [&](std::string const &id) {
        return PullFrameWithPrevious(id, fix.secondUri, afterMainId);
      });
  Expect(afterKind.find("\"kind\":\"full\"") != std::string::npos,
         "a `kind` flip must recompute, not `unchanged`");
  Expect(afterKind.find("module-code-outside-main") != std::string::npos,
         "under `kind = dll` the same run must fire again");
  std::string const afterKindId = ResultIdOf(afterKind);
  Expect(!afterKindId.empty() && afterKindId != afterMainId &&
             afterKindId != beforeId,
         "changing `build.kind` must move the resultId");

  session.stop();
}

// The quick fix over the wire: the wrap offers the derived constructor name
// and an edit that keeps the run's text verbatim.
void TestWrapFixOverTheWire() {
  ModuleFixture fix;
  WriteToml(fix.ws, "[build]\nkind = \"exe\"\nmain = \"main.bas\"\n");
  WriteFile(fix.ws / "main.bas", kMainText);
  WriteFile(fix.ws / "second.bas", kSecondText);

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(RootPushInitialize(fix.rootUri).c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"diagnosticProvider\"") == std::string::npos,
         "a legacy client keeps the push path");
  input->append(MakeLspFrame(OpenFrame(fix.secondUri, kSecondText).c_str()));
  std::string const published = WaitForPublishedUri(output, 1);
  Expect(LastPublish(published).find("\"code\":\"module-code-outside-main\"") !=
             std::string::npos,
         "the publish must carry the diagnostic before it can be fixed");

  std::string const reply = PollRequest(
      input, output, "mcf", "SecondInit", [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/codeAction","params":{"textDocument":{"uri":")" +
               fix.secondUri +
               R"("},"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":0}},"context":{"diagnostics":[]}}})";
      });
  Expect(reply.find("\"title\":\"Wrap in constructor 'SecondInit'\"") !=
             std::string::npos,
         "the fix is titled with the derived constructor name");
  Expect(reply.find("Sub SecondInit() Constructor") != std::string::npos,
         "the edit opens the constructor");
  Expect(reply.find("End Sub") != std::string::npos,
         "the edit closes the constructor");
  // The two insertions sit exactly at the run's edges, so the run's own bytes
  // stay put verbatim; no edit rewrites them.
  Expect(reply.find("\"start\":{\"line\":0,\"character\":0},\"end\":"
                    "{\"line\":0,\"character\":0}") != std::string::npos,
         "the opener inserts at the first statement");
  Expect(reply.find("\"start\":{\"line\":1,\"character\":13},\"end\":"
                    "{\"line\":1,\"character\":13}") != std::string::npos,
         "the closer inserts at the last statement's end");

  session.stop();
}

} // namespace

namespace fbtest {

void RunModuleCodeTests() {
  RUN_TEST(TestExeWithMainFiresOutsideMain);
  RUN_TEST(TestExeWithoutMainNeverFires);
  RUN_TEST(TestLibraryFiresEverywhere);
  RUN_TEST(TestRunInterruptedByDimAndIfSpan);
  RUN_TEST(TestConfigChangeMovesReport);
  RUN_TEST(TestWrapFixOverTheWire);
}

} // namespace fbtest
