/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// cross-file resolution and the storage gate.
//
// Definition/references/highlight across the include boundary, completion over
// the same closure, and the lenient by-key fallback that resolves a type
// outside the closure.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {
// Tier-3 leniency: a name the closure does not declare at all still resolves
// to any workspace root `byKey` knows about — a not-yet-included header.
// Tracked as a divergence (FreeBASIC.md §12), accepted by PLAN M7.
void TestCrossFileLenientByKeyFallback() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  // Only the client is opened; extra.bi is never touched by the client, so
  // only the workspace scan can index it.
  auto input = StartIndexedSession(session, server, output, fix,
                                   {{fix.mainUri, kMainContent}});

  std::string const def = PollRequest(
      input, output, "cby", fix.extraUri, [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/definition","params":{"textDocument":{"uri":")" +
               fix.mainUri + R"("},"position":{"line":2,"character":33}}})";
      });
  Expect(def.find(fix.extraUri) != std::string::npos,
         "an out-of-closure byKey hit must still resolve its declaration");
  Expect(def.find("\"start\":{\"line\":0,\"character\":11}") !=
                 std::string::npos &&
             def.find("\"end\":{\"line\":0,\"character\":20}") !=
                 std::string::npos,
         "the lenient fallback must land on the extra.bi declaration name");

  session.stop();
}

void TestCrossFileDefinitionReferencesHighlight() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(
      session, server, output, fix,
      {{fix.libUri, kLibContent}, {fix.mainUri, kMainContent}});

  // Definition: the module-level usage of globalCount jumps into lib.bi.
  // Poll until the background scan has indexed the closure.
  std::string const def = PollRequest(
      input, output, "cdef", fix.libUri, [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/definition","params":{"textDocument":{"uri":")" +
               fix.mainUri + R"("},"position":{"line":2,"character":7}}})";
      });
  Expect(def.find("\"start\":{\"line\":0,\"character\":11}") !=
                 std::string::npos &&
             def.find("\"end\":{\"line\":0,\"character\":22}") !=
                 std::string::npos,
         "definition must land on the globalCount declaration name in lib.bi");
  Expect(def.find(fix.mainUri + "\"") == std::string::npos ||
             def.find(fix.libUri + "\"") != std::string::npos,
         "definition must point at the header, not the client file");

  // References: the declaration plus every closure usage, project files
  // sorted lexically (lib.bi before main.bas), sites by byte offset.
  std::string const refs = PollRequest(
      input, output, "cref", "\"start\":{\"line\":3,\"character\":10}",
      [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/references","params":{"textDocument":{"uri":")" +
               fix.mainUri +
               R"("},"position":{"line":2,"character":7},"context":{"includeDeclaration":true}}})";
      });
  Expect(refs.find("\"start\":{\"line\":0,\"character\":11}") !=
                 std::string::npos &&
             refs.find("\"end\":{\"line\":0,\"character\":22}") !=
                 std::string::npos,
         "references must include the declaration site in lib.bi");
  Expect(refs.find("\"start\":{\"line\":3,\"character\":10}") !=
             std::string::npos,
         "references must include the print usage inside libProc");
  Expect(refs.find("\"start\":{\"line\":2,\"character\":7}") !=
             std::string::npos,
         "references must include the main.bas module usage");
  Expect(
      refs.find("\"start\":{\"line\":4,\"character\":4}") !=
              std::string::npos &&
          refs.find("\"start\":{\"line\":4,\"character\":18}") !=
              std::string::npos,
      "references must include both in-sub usages of globalCount in main.bas");

  // Highlight is per-document: grouped usages in main.bas only (the remote
  // declaration contributes no foreign range) — module and both in-sub sites.
  std::string const hl = PollRequest(
      input, output, "chl", "\"line\":4", [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/documentHighlight","params":{"textDocument":{"uri":")" +
               fix.mainUri + R"("},"position":{"line":2,"character":7}}})";
      });
  std::size_t hlCount = 0;
  std::size_t pos = 0;
  while ((pos = hl.find("\"start\":", pos)) != std::string::npos) {
    ++hlCount;
    pos += 8;
  }
  Expect(hlCount == 3,
         "highlight must cover the three in-document globalCount usages");
  Expect(hl.find("\"start\":{\"line\":2,\"character\":7}") !=
                 std::string::npos &&
             hl.find("\"start\":{\"line\":4,\"character\":4}") !=
                 std::string::npos &&
             hl.find("\"start\":{\"line\":4,\"character\":18}") !=
                 std::string::npos,
         "each in-document usage must be a highlight site");

  session.stop();
}

void TestCrossFileStorageGate() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(
      session, server, output, fix,
      {{fix.libUri, kLibContent}, {fix.mainUri, kMainContent}});

  // localOnly is a file-root plain dim in lib.bi: visible from module level.
  std::string const moduleLevel =
      PollRequest(input, output, "cg1", fix.libUri, [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/definition","params":{"textDocument":{"uri":")" +
               fix.mainUri + R"("},"position":{"line":2,"character":22}}})";
      });
  // The reply travels in the message: a poll that returns the wrong answer and
  // a poll that waited its whole budget are the same failed assertion to the
  // reader, and only the first one is obvious from the test.
  std::string const why =
      "module-level use of a plain header dim must resolve into the header; "
      "reply was " +
      moduleLevel.substr(0, 200);
  Expect(moduleLevel.find("\"start\":{\"line\":1,\"character\":4}") !=
                 std::string::npos &&
             moduleLevel.find("\"end\":{\"line\":1,\"character\":13}") !=
                 std::string::npos,
         why.c_str());

  // The same name inside a procedure must not resolve at all (fbc error 42).
  std::string const inside = PollRequest(
      input, output, "cg2", "\"result\":null", [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/definition","params":{"textDocument":{"uri":")" +
               fix.mainUri + R"("},"position":{"line":5,"character":4}}})";
      });
  Expect(inside.find(fix.libUri) == std::string::npos &&
             inside.find(fix.mainUri) == std::string::npos,
         "a gated plain module dim must not resolve from inside a block");

  session.stop();
}

void TestCrossFileCompletionHonorsGate() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(
      session, server, output, fix,
      {{fix.libUri, kLibContent}, {fix.progUri, kProgContent}});

  // Module level, empty prefix (start of the `dim counter` line, so the whole
  // list is produced): the closure's plain dim and the in-file counter are
  // both visible. Wait on the closure symbol so the asynchronous index scan
  // has settled before the assertions (a pre-index reply has only in-file
  // names).
  std::string const moduleLevel = PollRequest(
      input, output, "ccm", "\"label\":\"localOnly\"",
      [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/completion","params":{"textDocument":{"uri":")" +
               fix.progUri + R"("},"position":{"line":1,"character":0}}})";
      });
  Expect(moduleLevel.find("\"label\":\"localOnly\"") != std::string::npos,
         "module-level completion must offer the closure's plain dim");
  Expect(moduleLevel.find("\"label\":\"counter\"") != std::string::npos,
         "module-level completion must offer the in-file counter");

  // Inside the sub: the local counter completes, the closure's plain dim is
  // gated out.
  std::string const inside = PollRequest(
      input, output, "cci", "\"label\":\"counter\"",
      [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/completion","params":{"textDocument":{"uri":")" +
               fix.progUri + R"("},"position":{"line":3,"character":0}}})";
      });
  Expect(inside.find("\"label\":\"counter\"") != std::string::npos,
         "in-block completion must offer the local counter");
  Expect(inside.find("\"label\":\"localOnly\"") == std::string::npos,
         "a plain module dim of an included header must not complete inside a "
         "block");

  session.stop();
}
} // namespace

namespace fbtest {

void RunCrossfileTests() {
  RUN_TEST(TestCrossFileDefinitionReferencesHighlight);
  RUN_TEST(TestCrossFileStorageGate);
  RUN_TEST(TestCrossFileCompletionHonorsGate);
  RUN_TEST(TestCrossFileLenientByKeyFallback);
}

} // namespace fbtest
