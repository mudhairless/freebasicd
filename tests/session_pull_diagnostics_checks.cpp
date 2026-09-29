/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// M14 pull diagnostics.
//
// The 3.17 alternative to push: the negotiation that decides which path runs, a
// full report and the `unchanged` answer for a matching previousResultId, the
// relatedDocuments closure, workspace/diagnostic, and the refresh hint that
// stands where a push would have been.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {
// Same pull capability, but without refresh support: the server advertises the
// provider yet never sends workspace/diagnostic/refresh (the client re-pulls
// on its own schedule).
char const kInitializePullNoRefreshFrame[] =
    R"FB({"jsonrpc":"2.0","id":"init","method":"initialize","params":{)FB"
    R"FB("capabilities":{"textDocument":{"diagnostic":{"relatedDocumentSupport":false}}}}})FB";

// Negotiation is the whole contract: a client that advertises
// textDocument.diagnostic is offered the pull provider and served no
// publishDiagnostics (the spec wants pull preferred once negotiated, and a
// client that supports both must never see the same diagnostics twice), while
// a legacy client sees no diagnosticProvider and keeps the push path — one
// build serves both generations.
void TestPullDiagnosticsNegotiation() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kInitializePullFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"init\"");
  Expect(response.find("\"diagnosticProvider\"") != std::string::npos,
         "a pull-capable client must be offered diagnosticProvider");
  Expect(response.find("\"interFileDependencies\":true") != std::string::npos,
         "diagnosticProvider must advertise interFileDependencies (the M6 "
         "include closure is what relatedDocuments answers from)");
  Expect(response.find("\"workspaceDiagnostics\":true") != std::string::npos,
         "diagnosticProvider must advertise workspaceDiagnostics");

  // didOpen must not push. The client can take the refresh hint
  // (refreshSupport), which the reparse sends *instead* of the publish, so the
  // refresh's arrival is also the ordering that proves the publish was skipped.
  input->append(MakeLspFrame(kDidOpenFrame));
  std::string const afterOpen = WaitForRefreshes(output, 1);
  Expect(afterOpen.find("\"method\":\"textDocument/publishDiagnostics\"") ==
             std::string::npos,
         "a pull client must never receive publishDiagnostics");

  session.stop();

  // The legacy half: no diagnostic capability, no provider, push unchanged.
  lsp::LanguageSession session2(log);
  auto input2 = std::make_shared<FeedableIStream>();
  auto output2 = std::make_shared<StringOStream>();
  FreeBasicServer server2(session2);
  server2.registerHandlers();
  session2.start(input2, output2);
  input2->append(MakeLspFrame(kInitializeFrame));
  std::string const legacy =
      WaitForOutputContaining(output2, "\"id\":\"init\"");
  Expect(legacy.find("\"diagnosticProvider\"") == std::string::npos,
         "a client without textDocument.diagnostic must not be offered pull");
  input2->append(MakeLspFrame(kDidOpenFrame));
  Expect(WaitForPublishedUri(output2, 1).find("\"diagnostics\":[") !=
             std::string::npos,
         "a legacy client must keep receiving publishDiagnostics");
  session2.stop();
}

// The document pull: the first ask answers a full report (items + resultId),
// and a re-ask naming that resultId as previousResultId answers `unchanged`
// (kind + resultId only, no items) — the client keeps its cached copy.
void TestPullDiagnosticsFullAndUnchanged() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kInitializePullFrame));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"diagnosticProvider\"") != std::string::npos,
         "initialize must advertise the pull provider");

  input->append(MakeLspFrame(kDidOpenDupFrame));
  std::string const afterOpen = WaitForRefreshes(output, 1);
  Expect(afterOpen.find("\"method\":\"textDocument/publishDiagnostics\"") ==
             std::string::npos,
         "didOpen must not publish to a pull client");

  std::string const full = PollRequest(
      input, output, "pdoc", "\"kind\":\"full\"", [&](std::string const &id) {
        return std::string(
                   R"({"jsonrpc":"2.0","id":")" + id +
                   R"(","method":"textDocument/diagnostic","params":{"textDocument":{"uri":")") +
               Expand(kUri) + "\"}}}";
      });
  Expect(full.find("\"kind\":\"full\"") != std::string::npos,
         "the first pull must answer the full arm");
  Expect(full.find("\"code\":\"duplicate-definition\"") != std::string::npos,
         "the full report must carry the document's diagnostics");
  Expect(full.find("\"start\":{\"line\":1,\"character\":4}") !=
             std::string::npos,
         "the pulled diagnostic must carry the same range as the push path");
  std::string const resultId = ResultIdOf(full);
  Expect(!resultId.empty(), "the full report must carry a resultId");

  std::string const unchanged = PollRequest(
      input, output, "pdoc2", "\"kind\":\"unchanged\"",
      [&](std::string const &id) {
        return std::string(
                   R"({"jsonrpc":"2.0","id":")" + id +
                   R"(","method":"textDocument/diagnostic","params":{"textDocument":{"uri":")") +
               Expand(kUri) + R"("},"previousResultId":")" + resultId + "\"}}";
      });
  Expect(unchanged.find("\"kind\":\"unchanged\"") != std::string::npos,
         "a matching previousResultId must answer the unchanged arm");
  Expect(unchanged.find("duplicate-definition") == std::string::npos,
         "the unchanged report must not resend the cached items");
  Expect(!ResultIdOf(unchanged).empty(),
         "the unchanged report must still name its resultId");

  session.stop();
}

// relatedDocuments: the include closure a pull on one document surfaces
// alongside its own problems — the interFileDependencies promise. The included
// header's own diagnostics ride on the request document's full report.
void TestPullDiagnosticsRelatedDocuments() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(sandbox);
  {
    // err.bi carries a parse error of its own; src.bas pulls it in.
    std::ofstream out(sandbox / "err.bi");
    out << "dim x as integer\ndim x as string\n";
    std::ofstream out2(sandbox / "src.bas");
    out2 << "#include \"err.bi\"\nprint \"hi\"\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const rootUri = FileUri(sandbox);
  std::string const srcUri = FileUri(sandbox / "src.bas");
  std::string const errUri = FileUri(sandbox / "err.bi");
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri +
      R"(","capabilities":{"textDocument":{"diagnostic":{"relatedDocumentSupport":true}}}}})";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"diagnosticProvider\"") != std::string::npos,
         "initialize must advertise the pull provider");

  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      srcUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("#include \"err.bi\"\nprint \"hi\"\n") + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));
  // The open upserts src.bas's index entry with its include edge resolved;
  // that entry is what the pull's closure walk reads. Its arrival (the
  // refresh) is the ordering that guarantees the walk sees it.
  Expect(WaitForRefreshes(output, 1).empty() == false,
         "didOpen must send the refresh hint");

  std::string const reply = PollRequest(
      input, output, "prel", "relatedDocuments", [&](std::string const &id) {
        return std::string(
                   R"({"jsonrpc":"2.0","id":")" + id +
                   R"(","method":"textDocument/diagnostic","params":{"textDocument":{"uri":")") +
               srcUri + "\"}}}";
      });
  Expect(reply.find("\"relatedDocuments\"") != std::string::npos,
         "a pull on an including document must surface relatedDocuments");
  Expect(reply.find(errUri) != std::string::npos,
         "the related report must be keyed by the included file's uri");
  Expect(reply.find("\"code\":\"duplicate-definition\"") != std::string::npos,
         "the related report must carry the header's own diagnostics");
  Expect(reply.find("\"kind\":\"full\"") != std::string::npos,
         "the request document's report must be full");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// The workspace pull: every document the client holds reports for — or that
// newly carries diagnostics — is answered; clean unlisted files are skipped,
// an open buffer's report carries its wire version, and a gate flip changes
// the resultId (so a client caching by id is told the report changed instead
// of being answered "unchanged").
void TestWorkspaceDiagnostic() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(sandbox);
  {
    // A config marker makes the client root a real workspace root, so the
    // workspace scan covers bad.bas and clean.bas eagerly instead of deferring
    // to the first didOpen.
    std::ofstream marker(sandbox / "freebasicd.toml");
    marker << "[server]\n";
    std::ofstream out(sandbox / "bad.bas");
    out << "else\n"; // a stray-closer Error
    std::ofstream out2(sandbox / "clean.bas");
    out2 << "print \"ok\"\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const rootUri = FileUri(sandbox);
  std::string const badUri = FileUri(sandbox / "bad.bas");
  std::string const cleanUri = FileUri(sandbox / "clean.bas");
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri +
      R"(","capabilities":{"textDocument":{"diagnostic":{"relatedDocumentSupport":true}},)"
      R"("workspace":{"diagnostics":{"refreshSupport":true}}}}})";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"diagnosticProvider\"") != std::string::npos,
         "initialize must advertise the pull provider");

  // First ask, nothing listed: the file with diagnostics answers full, the
  // clean file (never reported, nothing to say) is skipped.
  std::string const first = PollRequest(
      input, output, "pws", "\"kind\":\"full\"", [&](std::string const &id) {
        return std::string(R"({"jsonrpc":"2.0","id":")" + id +
                           R"(","method":"workspace/diagnostic","params":{}})");
      });
  Expect(first.find(badUri) != std::string::npos,
         "the workspace pull must report the file that carries diagnostics");
  Expect(first.find("\"code\":\"stray-closer\"") != std::string::npos,
         "the full report must carry the file's diagnostics");
  Expect(first.find(cleanUri) == std::string::npos,
         "a clean file never reported must be skipped");
  std::string const resultId1 = ResultIdOf(first);
  Expect(!resultId1.empty(), "the workspace full report must carry a resultId");

  // Second ask, the file listed with the id it holds: unchanged, no items.
  std::string const second = PollRequest(
      input, output, "pws2", "\"kind\":\"unchanged\"",
      [&](std::string const &id) {
        return std::string(
                   R"({"jsonrpc":"2.0","id":")" + id +
                   R"(","method":"workspace/diagnostic","params":{"previousResultIds":[{"uri":")") +
               badUri + R"(","value":")" + resultId1 + "\"}]}}";
      });
  Expect(second.find("\"kind\":\"unchanged\"") != std::string::npos,
         "a listed file whose resultId matches must answer unchanged");
  Expect(second.find("stray-closer") == std::string::npos,
         "the unchanged workspace answer must not resend the items");

  // Open bad.bas with edited content under a real wire version: the stale id
  // forces a full report and the open-buffer report carries the version.
  std::string const openBad =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      badUri + R"(","languageId":"basic","version":7,"text":")" +
      ToJsonString("else\ncase 1\n") + "\"}}}";
  input->append(MakeLspFrame(openBad.c_str()));
  Expect(WaitForRefreshes(output, 1).find(
             "\"method\":\"textDocument/publishDiagnostics\"") ==
             std::string::npos,
         "didOpen must not publish to a pull client");
  std::string const third = PollRequest(
      input, output, "pws3", "\"version\":7", [&](std::string const &id) {
        return std::string(
                   R"({"jsonrpc":"2.0","id":")" + id +
                   R"(","method":"workspace/diagnostic","params":{"previousResultIds":[{"uri":")") +
               badUri + R"(","value":")" + resultId1 + "\"}]}}";
      });
  Expect(third.find("\"version\":7") != std::string::npos,
         "an open buffer's report must carry its wire version");
  Expect(third.find("\"kind\":\"full\"") != std::string::npos,
         "a stale resultId (edited content) must answer full");
  std::string const resultId2 = ResultIdOf(third);
  Expect(!resultId2.empty() && resultId2 != resultId1,
         "edited content must change the report's resultId");

  // Gate flip: diagnostics off changes the id too (so a cache keyed on it is
  // invalidated) and the report answers full with an empty item list.
  {
    std::ofstream out(sandbox / "freebasicd.toml");
    out << "diagnosticsOn = false\n";
  }
  input->append(MakeLspFrame(kDidChangeConfigurationFrame));
  Expect(WaitForRefreshes(output, 2).find(
             "\"method\":\"textDocument/publishDiagnostics\"") ==
             std::string::npos,
         "a config change must not push to a pull client");
  std::string const gated = PollRequest(
      input, output, "pws4", "\"items\":[]", [&](std::string const &id) {
        return std::string(
                   R"({"jsonrpc":"2.0","id":")" + id +
                   R"(","method":"workspace/diagnostic","params":{"previousResultIds":[{"uri":")") +
               badUri + R"(","value":")" + resultId2 + "\"}]}}";
      });
  Expect(gated.find("\"items\":[]") != std::string::npos,
         "a gated-off workspace report must answer an empty item list");
  Expect(gated.find("stray-closer") == std::string::npos,
         "a gated-off report must carry no diagnostics");
  std::string const resultId3 = ResultIdOf(gated);
  Expect(!resultId3.empty() && resultId3 != resultId2,
         "the diagnostics gate must be part of the report's identity");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// The refresh hint: every diagnostic-relevant change asks a pull client to
// re-pull exactly once, and a client without refresh support is never sent the
// request (it re-pulls on its own schedule).
void TestPullDiagnosticsRefresh() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kInitializePullFrame));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"diagnosticProvider\"") != std::string::npos,
         "initialize must advertise the pull provider");

  input->append(MakeLspFrame(kDidOpenFrame));
  Expect(CountRefreshes(WaitForRefreshes(output, 1)) == 1,
         "didOpen must send exactly one refresh to a refresh-capable client");

  input->append(MakeLspFrame(kDidChangeFrame));
  Expect(CountRefreshes(WaitForRefreshes(output, 2)) == 2,
         "didChange must send exactly one further refresh");

  session.stop();

  // Without refreshSupport, the same open+change must send no refresh; the
  // pull still answers (the client just re-pulls on its own schedule).
  lsp::LanguageSession session2(log);
  auto input2 = std::make_shared<FeedableIStream>();
  auto output2 = std::make_shared<StringOStream>();
  FreeBasicServer server2(session2);
  server2.registerHandlers();
  session2.start(input2, output2);
  input2->append(MakeLspFrame(kInitializePullNoRefreshFrame));
  Expect(WaitForOutputContaining(output2, "\"id\":\"init\"")
                 .find("\"diagnosticProvider\"") != std::string::npos,
         "initialize must advertise the pull provider without refresh support");
  input2->append(MakeLspFrame(kDidOpenFrame));
  input2->append(MakeLspFrame(kDidChangeFrame));
  // The pull round-trip after the open+change is the ordering that proves the
  // open and change were processed, and that the stream still holds no refresh.
  std::string const answered = PollRequest(
      input2, output2, "pnr", "\"kind\":\"full\"", [&](std::string const &id) {
        return std::string(
                   R"({"jsonrpc":"2.0","id":")" + id +
                   R"(","method":"textDocument/diagnostic","params":{"textDocument":{"uri":")") +
               Expand(kUri) + "\"}}}";
      });
  Expect(answered.find("\"kind\":\"full\"") != std::string::npos,
         "a pull must still be answered without refresh support");
  Expect(CountRefreshes(answered) == 0,
         "a client without refreshSupport must never receive the refresh "
         "request");
  session2.stop();
}
} // namespace

namespace fbtest {

void RunPullDiagnosticsTests() {
  RUN_TEST(TestPullDiagnosticsNegotiation);
  RUN_TEST(TestPullDiagnosticsFullAndUnchanged);
  RUN_TEST(TestPullDiagnosticsRelatedDocuments);
  RUN_TEST(TestWorkspaceDiagnostic);
  RUN_TEST(TestPullDiagnosticsRefresh);
}

} // namespace fbtest
