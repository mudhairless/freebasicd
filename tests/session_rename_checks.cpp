/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// M8 prepareRename and rename.
//
// The rename handshake end to end: what prepareRename promises, a cross-file
// rename rewriting both files, a local-only symbol staying put, and the two
// rejections (an invalid new name, a collision with an existing symbol).
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {

void TestPrepareRenameReturnsRangeAndPlaceholder() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(
      session, server, output, fix,
      {{fix.libUri, kLibContent}, {fix.mainUri, kMainContent}});

  // Cursor on the module-level globalCount usage in main.bas; the returned
  // rename range is that usage token's own range in the requesting document
  // (the resolved declaration lives in lib.bi, but prepareRename reports the
  // editor's selection) plus the current name as the placeholder.
  std::string const prep = PollRequest(
      input, output, "prep", "\"placeholder\":\"globalCount\"",
      [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/prepareRename","params":{"textDocument":{"uri":")" +
               fix.mainUri + R"("},"position":{"line":2,"character":7}}})";
      });
  Expect(prep.find("\"range\":{\"start\":{\"line\":2,\"character\":7},\"end\":{"
                   "\"line\":2,\"character\":18}}") != std::string::npos,
         "prepareRename must return the requesting-file token range");
  Expect(prep.find("\"placeholder\":\"globalCount\"") != std::string::npos,
         "prepareRename must offer the current name as placeholder");

  session.stop();
}

void TestPrepareRenameOnKeywordReturnsNull() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(
      session, server, output, fix,
      {{fix.libUri, kLibContent}, {fix.mainUri, kMainContent}});

  // Cursor on the `dim` keyword (line 1, column 0) — not renameable.
  std::string const prep = PollRequest(
      input, output, "prk", "\"result\":null", [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/prepareRename","params":{"textDocument":{"uri":")" +
               fix.mainUri + R"("},"position":{"line":1,"character":0}}})";
      });
  Expect(prep.find("\"result\":null") != std::string::npos,
         "prepareRename on a keyword must resolve to null");

  session.stop();
}

void TestRenameCrossFileRewritesBothFiles() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(
      session, server, output, fix,
      {{fix.libUri, kLibContent}, {fix.mainUri, kMainContent}});

  // Rename the shared module var from a client usage: both files' sites are
  // rewritten as document edits with no client-version constraint (disk is
  // master for closed files).
  std::string const ren = PollRequest(
      input, output, "ren", "\"documentChanges\"", [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/rename","params":{"textDocument":{"uri":")" +
               fix.mainUri +
               R"("},"position":{"line":2,"character":7},"newName":"renamedCount"}})";
      });
  Expect(ren.find("\"documentChanges\"") != std::string::npos,
         "rename must reply with a workspace edit over documentChanges");
  Expect(ren.find(fix.libUri) != std::string::npos,
         "the header must receive edits");
  Expect(ren.find(fix.mainUri) != std::string::npos,
         "the client file must receive edits");
  Expect(ren.find("\"version\":") == std::string::npos,
         "document edits must not tie the edit to a client buffer version: "
         "the version member is left unset (disk content is master), and "
         "LspCpp's JSON writer omits unset optionals rather than writing null");

  std::size_t editCount = 0;
  std::size_t pos = 0;
  while ((pos = ren.find("\"newText\":\"renamedCount\"", pos)) !=
         std::string::npos) {
    ++editCount;
    pos += 1;
  }
  Expect(editCount == 5, "rename must cover the declaration, the header usage, "
                         "and the three client usages");
  Expect(ren.find("\"start\":{\"line\":0,\"character\":11}") !=
                 std::string::npos &&
             ren.find("\"end\":{\"line\":0,\"character\":22}") !=
                 std::string::npos,
         "the lib.bi declaration name must be rewritten");
  Expect(ren.find("\"start\":{\"line\":3,\"character\":10}") !=
             std::string::npos,
         "the usage inside libProc must be rewritten");
  Expect(ren.find("\"start\":{\"line\":4,\"character\":4}") !=
                 std::string::npos &&
             ren.find("\"start\":{\"line\":4,\"character\":18}") !=
                 std::string::npos,
         "both in-block usages in main.bas must be rewritten");

  session.stop();
}

void TestRenameLocalOnlyStaysInFile() {
  TwoFileFixture const fix;

  // `loc` is a reserved keyword (the LOC file-position function), so the
  // fixture must pick a name that lexes as an identifier.
  std::string const progText = "#include \"lib.bi\"\n"
                               "dim counter\n"
                               "sub prog()\n"
                               "    dim counter\n"
                               "    counter = 3\n"
                               "end sub\n";

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input =
      StartIndexedSession(session, server, output, fix,
                          {{fix.libUri, kLibContent}, {fix.progUri, progText}});

  // Rename the sub-local `counter` from its in-block usage: only the local
  // declaration and usage change; the module-level `dim counter` is untouched
  // and no other file receives an edit.
  std::string const ren = PollRequest(
      input, output, "rlo", "\"documentChanges\"", [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/rename","params":{"textDocument":{"uri":")" +
               fix.progUri +
               R"("},"position":{"line":4,"character":4},"newName":"localCounter"}})";
      });
  Expect(ren.find(fix.progUri) != std::string::npos,
         "local rename must edit the file it lives in");
  Expect(ren.find(fix.libUri) == std::string::npos,
         "a local rename must not touch other files");
  Expect(ren.find("\"start\":{\"line\":3,\"character\":8}") !=
                 std::string::npos &&
             ren.find("\"end\":{\"line\":3,\"character\":15}") !=
                 std::string::npos,
         "the local declaration name must be rewritten");
  Expect(ren.find("\"start\":{\"line\":4,\"character\":4}") !=
             std::string::npos,
         "the in-block usage must be rewritten");
  Expect(ren.find("\"line\":1") == std::string::npos,
         "the module-level dim counter must stay untouched");

  session.stop();
}

void TestRenameRejectsInvalidName() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(
      session, server, output, fix,
      {{fix.libUri, kLibContent}, {fix.mainUri, kMainContent}});

  // A digit-leading name cannot lex as one identifier token.
  std::string const bad = PollRequest(
      input, output, "rin", "\"error\"", [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/rename","params":{"textDocument":{"uri":")" +
               fix.mainUri +
               R"("},"position":{"line":2,"character":7},"newName":"123abc"}})";
      });
  Expect(bad.find("\"error\"") != std::string::npos &&
             bad.find("invalid new name") != std::string::npos,
         "a digit-leading new name must be rejected");

  // A reserved keyword cannot be a new name either.
  std::string const kw =
      PollRequest(input, output, "rk", "\"error\"", [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/rename","params":{"textDocument":{"uri":")" +
               fix.mainUri +
               R"("},"position":{"line":2,"character":7},"newName":"print"}})";
      });
  Expect(kw.find("\"error\"") != std::string::npos &&
             kw.find("invalid new name") != std::string::npos,
         "a reserved-keyword new name must be rejected");

  session.stop();
}

void TestRenameRejectsCollision() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(
      session, server, output, fix,
      {{fix.libUri, kLibContent}, {fix.mainUri, kMainContent}});

  // main.bas already declares a module-scope `head`; folding globalCount into
  // that key would merge two declarations in one textual module.
  std::string const ren =
      PollRequest(input, output, "rc", "\"error\"", [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/rename","params":{"textDocument":{"uri":")" +
               fix.mainUri +
               R"("},"position":{"line":2,"character":7},"newName":"head"}})";
      });
  Expect(ren.find("\"error\"") != std::string::npos &&
             ren.find("collides with an existing declaration") !=
                 std::string::npos,
         "renaming onto a closure module-scope key must be rejected");

  session.stop();
}
} // namespace

namespace fbtest {

void RunRenameTests() {
  RUN_TEST(TestPrepareRenameReturnsRangeAndPlaceholder);
  RUN_TEST(TestPrepareRenameOnKeywordReturnsNull);
  RUN_TEST(TestRenameCrossFileRewritesBothFiles);
  RUN_TEST(TestRenameLocalOnlyStaysInFile);
  RUN_TEST(TestRenameRejectsInvalidName);
  RUN_TEST(TestRenameRejectsCollision);
}

} // namespace fbtest
