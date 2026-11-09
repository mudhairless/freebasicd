/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Use-before-declaration over the wire.

#include "session_support.h"

using namespace fbtest;

namespace {

struct OrderFixture {
  std::filesystem::path sandbox;
  std::string mainUri;
  OrderFixture() {
    static std::atomic<long> counter{0};
    sandbox = std::filesystem::temp_directory_path() /
              ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
               std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(sandbox);
    mainUri = FileUri(sandbox / "main.bas");
  }
  ~OrderFixture() {
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
  }
};

std::string OpenFrame(std::string const &uri, std::string const &text) {
  return R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
         R"({"uri":")" +
         uri + R"(","languageId":"basic","version":1,"text":")" +
         ToJsonString(text) + "\"}}}";
}

std::string CodeActionFrame(std::string const &id, std::string const &uri,
                            int line, int endChar = 1000) {
  return "{\"jsonrpc\":\"2.0\",\"id\":\"" + id +
         "\",\"method\":\"textDocument/"
         "codeAction\",\"params\":{\"textDocument\":"
         "{\"uri\":\"" +
         uri + "\"},\"range\":{\"start\":{\"line\":" + std::to_string(line) +
         ",\"character\":0},\"end\":{\"line\":" + std::to_string(line) +
         ",\"character\":" + std::to_string(endChar) +
         "}},\"context\":{\"diagnostics\":[]}}}";
}

void TestDeclareAfterUseStillDiagnoses() {
  OrderFixture fix;
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);
  std::string const text =
      "helper()\nDeclare Sub helper()\nSub helper()\nEnd Sub\n";
  input->append(MakeLspFrame(OpenFrame(fix.mainUri, text).c_str()));
  std::string const published = WaitForPublishedUri(output, 1);
  Expect(published.find("\"code\":\"use-before-declaration, fbc error: 42\"") !=
             std::string::npos,
         "a Declare after the use must still diagnose the earlier use");
  Expect(published.find("\"start\":{\"line\":0") != std::string::npos,
         "the diagnostic anchors on the use");
  session.stop();
}

void TestDeclareFixKeepsDefault() {
  OrderFixture fix;
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);
  std::string const text = "helper()\nSub helper(a As Integer = 5)\nEnd Sub\n";
  input->append(MakeLspFrame(OpenFrame(fix.mainUri, text).c_str()));
  Expect(WaitForPublishedUri(output, 1).find("use-before-declaration") !=
             std::string::npos,
         "the use must publish before it is fixable");
  std::string const reply =
      PollRequest(input, output, "caord", "Insert", [&](std::string const &id) {
        return CodeActionFrame(id, fix.mainUri, 0);
      });
  Expect(reply.find("\"title\":\"Insert 'Declare' for 'helper'\"") !=
             std::string::npos,
         "the fix must be titled with the Declare it inserts");
  Expect(
      reply.find("\"newText\":\"Declare Sub helper(a As Integer = 5)\\n\"") !=
          std::string::npos,
      "the generated Declare must keep the default");
  session.stop();
}

void TestEarlyDimOffersReorderNeverAlias() {
  OrderFixture fix;
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);
  std::string const text = "Dim x As T\nType T\n  n As Integer\nEnd Type\n";
  input->append(MakeLspFrame(OpenFrame(fix.mainUri, text).c_str()));
  std::string const published = WaitForPublishedUri(output, 1);
  Expect(published.find("\"code\":\"use-before-declaration, fbc error: 14\"") !=
             std::string::npos,
         "an early Dim As T must publish error 14");
  std::string const reply =
      PollRequest(input, output, "caord2", "Move", [&](std::string const &id) {
        return CodeActionFrame(id, fix.mainUri, 0);
      });
  Expect(reply.find("Move 'T' above its first use") != std::string::npos,
         "an early Dim As T must offer the reorder fix");
  Expect(reply.find("Type T As") == std::string::npos,
         "the type fix must never offer an alias");
  Expect(reply.find("Type T_") == std::string::npos,
         "the type fix must never offer an alias name");
  session.stop();
}

void TestCompletionGatedAboveDeclaration() {
  OrderFixture fix;
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);
  std::string const text = "p\nSub helper()\nEnd Sub\n";
  input->append(MakeLspFrame(OpenFrame(fix.mainUri, text).c_str()));
  WaitForPublishedUri(output, 1);
  auto frame = [&](std::string const &id, int line) {
    return "{\"jsonrpc\":\"2.0\",\"id\":\"" + id +
           "\",\"method\":\"textDocument/"
           "completion\",\"params\":{\"textDocument\":"
           "{\"uri\":\"" +
           fix.mainUri + "\"},\"position\":{\"line\":" + std::to_string(line) +
           ",\"character\":0}}}";
  };
  std::string const above =
      PollRequest(input, output, "compa", "\"result\"",
                  [&](std::string const &id) { return frame(id, 0); });
  Expect(above.find("\"label\":\"helper\"") == std::string::npos,
         "completion above the declaration must not offer the name");
  std::string const below =
      PollRequest(input, output, "compb", "\"label\":\"helper\"",
                  [&](std::string const &id) { return frame(id, 2); });
  Expect(below.find("\"label\":\"helper\"") != std::string::npos,
         "completion after the declaration must offer the name");
  session.stop();
}

void TestWorkspaceHeaderIncludeOffer() {
  std::filesystem::path sandbox;
  {
    static std::atomic<long> counter{0};
    sandbox = std::filesystem::temp_directory_path() /
              ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
               std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(sandbox / "inc");
    std::ofstream out(sandbox / "inc" / "types.bi");
    out << "Type T\n  n As Integer\nEnd Type\n";
  }
  std::string const mainUri = FileUri(sandbox / "main.bas");
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);
  std::string const rootUri = FileUri(sandbox);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  WaitForOutputContaining(output, "\"id\":\"init\"");
  input->append(MakeLspFrame(OpenFrame(mainUri, "Dim x As T\n").c_str()));
  WaitForPublishedUri(output, 1);
  std::string const reply = PollRequest(
      input, output, "cainord", "Include",
      [&](std::string const &id) { return CodeActionFrame(id, mainUri, 0); });
  Expect(reply.find("inc/types.bi") != std::string::npos,
         "the include fix must name the workspace header it adds");
  Expect(reply.find("#include") != std::string::npos,
         "the include fix must write the resolvable literal");
  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

void TestSystemHeaderIncludeOffer() {
  OrderFixture fix;
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);
  std::string const rootUri = FileUri(fix.sandbox);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  WaitForOutputContaining(output, "\"id\":\"init\"");
  input->append(MakeLspFrame(
      OpenFrame(fix.mainUri, "var x = qlz_decompress(23)\n").c_str()));
  WaitForPublishedUri(output, 1);
  std::string const reply = PollRequest(
      input, output, "casys", "Include", [&](std::string const &id) {
        return CodeActionFrame(id, fix.mainUri, 0);
      });
  Expect(reply.find("quicklz.bi") != std::string::npos,
         "the system fallback must offer the installed quicklz.bi header");
  session.stop();
}

} // namespace

namespace fbtest {

void RunOrderTests() {
  RUN_TEST(TestDeclareAfterUseStillDiagnoses);
  RUN_TEST(TestDeclareFixKeepsDefault);
  RUN_TEST(TestEarlyDimOffersReorderNeverAlias);
  RUN_TEST(TestCompletionGatedAboveDeclaration);
  RUN_TEST(TestWorkspaceHeaderIncludeOffer);
  RUN_TEST(TestSystemHeaderIncludeOffer);
}

} // namespace fbtest
