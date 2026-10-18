/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// M17 formatting through the session: what `textDocument/formatting` and
// `textDocument/rangeFormatting` answer for an open buffer.
//
// The engine's rules are pinned byte for byte by format_engine_checks; this
// file pins the session half — the advertised capability, that the reply is a
// whole-document TextEdit in UTF-16 positions, that an already-formatted
// document answers with no edits at all, that a `.fbformat` found by walking
// up from the document changes the answer, that block comments survive the
// trip through the wire, and that a document the server has never seen is
// *answered* empty rather than left hanging. The last one matters: from the
// caller's side a give-up and an empty reply look identical to a poll, so the
// test has to see the answer arrive.
//
// `textDocument/rangeFormatting` currently defers to the full-document edit
// (formatRangeLsp's documented stub); its test pins that deferral so the day
// it becomes a true range edit the test is deleted deliberately, not by
// accident.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

#include <fstream>

using namespace fbtest;

namespace {

// The formatting request frames. Only this file sends them, so they live
// here. `options` is what a client would actually send; the server formats
// with the config file it discovered for the document and ignores the
// options, so both tests pass the same one.
std::string FormattingFrame(std::string const &id, std::string const &uri) {
  return R"({"jsonrpc":"2.0","id":")" + id +
         R"(","method":"textDocument/formatting","params":{"textDocument":{"uri":")" +
         uri + R"("},"options":{"tabSize":4,"insertSpaces":false}}})";
}

// The range formatting request: a slice of the middle of the document. It is
// answered with the whole-document edit until formatRangeLsp honors the
// range.
std::string RangeFormattingFrame(std::string const &id,
                                 std::string const &uri) {
  return R"({"jsonrpc":"2.0","id":")" + id +
         R"(","method":"textDocument/rangeFormatting","params":{"textDocument":{"uri":")" +
         uri +
         R"("},"range":{"start":{"line":1,"character":0},"end":{"line":2,"character":0}},"options":{"tabSize":4,"insertSpaces":false}}})";
}

// The `"newText":"..."` pair as it appears on the wire, for a needle that
// matches the formatted document exactly wherever it sits in the reply.
std::string NewTextNeedle(std::string const &text) {
  return "\"newText\":\"" + ToJsonString(text) + "\"";
}

// The FBdocs-preset answer for one small document: canonical keyword case,
// tab indentation (UseTab=ForIndentation with equal tab/indent widths), one
// space around the assignment. Every formatting test that needs a "the
// engine ran" reply uses one of these two strings, so a preset change shows
// up here first.
char const kUnformatted[] = "sub f()\nx=1\nif a then\ny=2\nend if\nend sub\n";
char const kFormatted[] = "Sub f()\n\tx = 1\n\tIf a Then\n\t\ty = 2\n"
                          "\tEnd If\nEnd Sub\n";

// A full-document formatting request over an unformatted buffer answers one
// TextEdit whose newText is the formatted document and whose range covers
// the whole buffer — start at the first line, end one past the last.
void TestFormattingRewritesTheOpenBuffer() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(session, server, output, fix,
                                   {{fix.progUri, kUnformatted}});

  std::string const init = WaitForOutputContaining(output, "\"id\":\"init\"");
  Expect(init.find("\"documentFormattingProvider\"") != std::string::npos,
         "initialize must advertise document formatting");
  Expect(init.find("\"documentRangeFormattingProvider\"") != std::string::npos,
         "initialize must advertise range formatting");

  std::string const reply = PollRequest(
      input, output, "fmt", NewTextNeedle(kFormatted),
      [&](std::string const &id) { return FormattingFrame(id, fix.progUri); });

  // The content is six lines plus the trailing newline, so the whole-document
  // range ends at line 6, character 0. These positions are UTF-16 code
  // units, which for this ASCII document are the byte columns.
  Expect(
      reply.find(
          R"("range":{"start":{"line":0,"character":0},"end":{"line":6,"character":0}})") !=
          std::string::npos,
      "the edit must span the whole document, in UTF-16 positions");
  Expect(CountOf(reply, "\"newText\":") == 1,
         "a full-document format is one edit, not one per changed line");

  session.stop();
}

// A buffer already in the discovered style produces no edits at all — the
// same fixed-point property format_engine_checks pins at the engine level,
// seen through the LSP layer. Returning at all is the assertion, which is why
// the poll's needle is the empty result.
void TestAlreadyFormattedAnswersNoEdits() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(session, server, output, fix,
                                   {{fix.progUri, kFormatted}});

  std::string const reply = PollRequest(
      input, output, "fmtnone", R"("result":[])",
      [&](std::string const &id) { return FormattingFrame(id, fix.progUri); });
  Expect(reply.find("\"newText\":") == std::string::npos,
         "an already-formatted document must produce no edit");

  session.stop();
}

// A `.fbformat` in a directory above the document changes the answer, so the
// discovery walk is wired to the handler and not merely to the config unit
// tests. The sibling document, whose walk does not pass through styled/,
// still gets the default preset — one session, two answers, from the file
// system alone.
void TestConfigFileDiscoveryChangesBehavior() {
  TwoFileFixture const fix;
  std::filesystem::create_directories(fix.wsDir / "styled");
  {
    std::ofstream cfg(fix.wsDir / "styled" / ".fbformat");
    cfg << "[format]\nKeywordCase = \"UPPER\"\n";
  }
  std::string const styledUri = FileUri(fix.wsDir / "styled" / "a.bas");

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(
      session, server, output, fix,
      {{styledUri, "sub f()\nend sub\n"}, {fix.progUri, "sub g()\nend sub\n"}});

  std::string const styledReply = PollRequest(
      input, output, "fmtcfg", NewTextNeedle("SUB f()\nEND SUB\n"),
      [&](std::string const &id) { return FormattingFrame(id, styledUri); });
  Expect(styledReply.find("SUB f()") != std::string::npos,
         "a .fbformat above the document must steer its formatting");

  std::string const defaultReply = PollRequest(
      input, output, "fmtdef", NewTextNeedle("Sub g()\nEnd Sub\n"),
      [&](std::string const &id) { return FormattingFrame(id, fix.progUri); });
  Expect(defaultReply.find("Sub g()") != std::string::npos,
         "a sibling outside the config's directory keeps the default preset");

  session.stop();
}

// A block comment survives the trip through the wire: its lines are copied
// byte for byte (only its first line takes the block's indent), while the
// code around it is still formatted.
void TestBlockCommentPreservedThroughTheSession() {
  TwoFileFixture const fix;
  char const *const content = "sub f()\n/'block\ncomment'/\nx=1\nend sub\n";
  char const *const want = "Sub f()\n\t/'block\ncomment'/\n\tx = 1\nEnd Sub\n";

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(session, server, output, fix,
                                   {{fix.progUri, content}});

  std::string const reply = PollRequest(
      input, output, "fmtblk", NewTextNeedle(want),
      [&](std::string const &id) { return FormattingFrame(id, fix.progUri); });
  Expect(reply.find("comment'/") != std::string::npos,
         "the block comment's interior must reach the client unchanged");

  session.stop();
}

// The range request is answered with the whole-document edit while
// formatRangeLsp defers. Deleting this test when range formatting becomes a
// true range edit is the deliberate act; letting it pass forever on a
// deferral nobody remembers is not.
void TestRangeFormattingDefersToTheWholeDocument() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(session, server, output, fix,
                                   {{fix.progUri, kUnformatted}});

  std::string const reply =
      PollRequest(input, output, "fmtrange", NewTextNeedle(kFormatted),
                  [&](std::string const &id) {
                    return RangeFormattingFrame(id, fix.progUri);
                  });
  Expect(reply.find(R"("start":{"line":0,"character":0})") != std::string::npos,
         "the deferring range edit still spans the whole document");

  session.stop();
}

// A document the server has never seen — never opened, not on disk — is
// answered with an empty result. Silence and an empty reply are the same
// timeout from the caller's side, so the assertion is that the answer
// arrives at all.
void TestFormattingUnknownDocumentAnswersEmpty() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(session, server, output, fix,
                                   {{fix.progUri, "x=1\n"}});
  std::string const ghostUri = FileUri(fix.wsDir / "never-opened.bas");

  std::string const reply = PollRequest(
      input, output, "fmtghost", R"("result":[])",
      [&](std::string const &id) { return FormattingFrame(id, ghostUri); });
  Expect(reply.find("\"newText\":") == std::string::npos,
         "an unknown document must produce no edit, not a guess");

  session.stop();
}

} // namespace

namespace fbtest {

void RunFormattingTests() {
  RUN_TEST(TestFormattingRewritesTheOpenBuffer);
  RUN_TEST(TestAlreadyFormattedAnswersNoEdits);
  RUN_TEST(TestConfigFileDiscoveryChangesBehavior);
  RUN_TEST(TestBlockCommentPreservedThroughTheSession);
  RUN_TEST(TestRangeFormattingDefersToTheWholeDocument);
  RUN_TEST(TestFormattingUnknownDocumentAnswersEmpty);
}

} // namespace fbtest
