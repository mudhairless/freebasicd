/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// M7 analysis reuse.
//
// Repeated requests served from one parse, a versionless didChange forcing a
// fresh one, and a cross-file closure reusing the analysis the index already
// holds.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {

// M10: repeat requests against an unchanged open buffer must be served from
// the content-addressed analysis cache — the cache's miss counter must not
// move, and its hit counter must. This is the "no reparse observable" gate.
void TestRepeatRequestsShareAnalysis() {
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
  fblang::AnalysisCache::Stats const afterOpen = server.analysisStats();

  auto completionFrame = [](std::string const &id) {
    return R"({"jsonrpc":"2.0","id":")" + id +
           R"(","method":"textDocument/completion","params":{"textDocument":{"uri":"file://{{tmp}}/resolve.bas"},"position":{"line":1,"character":8}}})";
  };

  input->append(MakeLspFrame(completionFrame("c1").c_str()));
  std::string const first = WaitForOutputContaining(output, "\"id\":\"c1\"");
  Expect(first.find("\"label\":\"counter\"") != std::string::npos,
         "the first completion must offer the in-scope symbol");
  fblang::AnalysisCache::Stats const afterFirst = server.analysisStats();

  input->append(MakeLspFrame(completionFrame("c2").c_str()));
  std::string const second = WaitForOutputContaining(output, "\"id\":\"c2\"");
  Expect(second.find("\"label\":\"counter\"") != std::string::npos,
         "the repeat completion must reply identically");
  fblang::AnalysisCache::Stats const afterSecond = server.analysisStats();

  Expect(afterFirst.misses == afterOpen.misses,
         "the first request on an open buffer must hit the didOpen analysis");
  Expect(afterSecond.misses == afterFirst.misses,
         "a repeat request must not re-analyze the unchanged buffer");
  Expect(afterSecond.hits > afterFirst.hits,
         "the repeat request must be served from the cache");

  session.stop();
}

// M10: a didChange with no `version` field still leaves the WorkingFile version
// unchanged; a version-keyed cache would serve the pre-change parse. The
// content-addressed key must re-analyze the new bytes and serve fresh symbols.
void TestVersionlessChangeServesFreshAnalysis() {
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
  fblang::AnalysisCache::Stats const afterOpen = server.analysisStats();

  // Full-document replacement, no "version" key on the textDocument.
  input->append(MakeLspFrame(
      R"FB({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file://{{tmp}}/resolve.bas"},"contentChanges":[{"range":{"start":{"line":0,"character":0},"end":{"line":1,"character":21}},"text":"dim total as integer\ntotal = total + 1\n"}]}})FB"));
  Expect(WaitForPublishedUri(output, 2).empty() == false,
         "the versionless change must publish diagnostics");

  input->append(MakeLspFrame(
      R"({"jsonrpc":"2.0","id":"ds2","method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file://{{tmp}}/resolve.bas"}}})"));
  std::string const syms = WaitForOutputContaining(output, "\"id\":\"ds2\"");
  Expect(syms.find("\"name\":\"total\"") != std::string::npos,
         "the versionless change's new declaration must be visible");
  Expect(syms.find("\"name\":\"counter\"") == std::string::npos,
         "the stale pre-change parse must never be served");

  fblang::AnalysisCache::Stats const afterChange = server.analysisStats();
  Expect(afterChange.misses == afterOpen.misses + 1,
         "changed bytes must force exactly one re-analysis, version or not");

  session.stop();
}

// M10: the cross-file closure tax is gone — a references pass over the
// requesting buffer plus a closed closure file warms the cache on first use
// (one disk-read miss), and a repeat pass re-analyzes nothing.
void TestReferencesClosureReusesAnalysis() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  // Only main.bas is open; lib.bi stays closed and is read from disk by the
  // closure provider.
  auto input = StartIndexedSession(session, server, output, fix,
                                   {{fix.mainUri, kMainContent}});

  auto refsFrame = [&](std::string const &id) {
    return R"({"jsonrpc":"2.0","id":")" + id +
           R"(","method":"textDocument/references","params":{"textDocument":{"uri":")" +
           fix.mainUri +
           R"("},"position":{"line":2,"character":7},"context":{"includeDeclaration":true}}})";
  };

  std::string const first = PollRequest(
      input, output, "rf1", "\"start\":{\"line\":0,\"character\":11}",
      [&](std::string const &id) { return refsFrame(id); });
  Expect(first.find("\"start\":{\"line\":0,\"character\":11}") !=
             std::string::npos,
         "the references pass must reach the header declaration");
  fblang::AnalysisCache::Stats const afterFirst = server.analysisStats();

  std::string const second = PollRequest(
      input, output, "rf2", "\"start\":{\"line\":0,\"character\":11}",
      [&](std::string const &id) { return refsFrame(id); });
  Expect(second.find("\"start\":{\"line\":0,\"character\":11}") !=
             std::string::npos,
         "the repeat references pass must reply identically");
  fblang::AnalysisCache::Stats const afterSecond = server.analysisStats();

  Expect(afterSecond.misses == afterFirst.misses,
         "a repeat references pass must reuse the closure analyses");
  Expect(afterSecond.hits > afterFirst.hits,
         "the repeat references pass must count cache hits");

  session.stop();
}
} // namespace

namespace fbtest {

void RunAnalysisTests() {
  RUN_TEST(TestRepeatRequestsShareAnalysis);
  RUN_TEST(TestVersionlessChangeServesFreshAnalysis);
  RUN_TEST(TestReferencesClosureReusesAnalysis);
}

} // namespace fbtest
