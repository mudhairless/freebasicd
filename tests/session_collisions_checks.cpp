/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Duplicate `Public` procedure collisions across one root's
// include closure (probed with fbc 1.10.3, FreeBASIC.md §9).
//
// A `Public` procedure that shares its uppercased name (plus, for
// `Overload` procedures, its parameter signature) with the same key in
// another file of the same root is a link error — each file compiles
// alone, so fbc has no message for it. The server reports it while
// editing as an Error (`duplicate-public-procedure`, no fbc identity),
// anchored on the definition's name token and naming the other file(s).
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {

struct CollisionFixture {
  std::filesystem::path sandbox;
  std::filesystem::path ws;
  std::string rootUri;
  std::string firstUri;
  std::string secondUri;
  std::string thirdUri;

  CollisionFixture() {
    static std::atomic<long> counter{0};
    sandbox = std::filesystem::temp_directory_path() /
              ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
               std::to_string(counter.fetch_add(1)));
    ws = sandbox / "ws";
    std::filesystem::create_directories(ws);
    rootUri = FileUri(ws);
    firstUri = FileUri(ws / "first.bas");
    secondUri = FileUri(ws / "second.bas");
    thirdUri = FileUri(ws / "third.bas");
  }

  ~CollisionFixture() {
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
  }
};

void WriteFile(std::filesystem::path const &path, char const *text) {
  std::ofstream out(path);
  out << text;
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

std::string ReplaceFrame(std::string const &uri, int sl, int sc, int el, int ec,
                         std::string const &text) {
  return R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":)"
         R"({"textDocument":{"uri":")" +
         uri +
         R"(","version":2},"contentChanges":[{"range":{"start":{"line":)" +
         std::to_string(sl) + R"(,"character":)" + std::to_string(sc) +
         R"(},"end":{"line":)" + std::to_string(el) + R"(,"character":)" +
         std::to_string(ec) + R"(}},"text":")" + ToJsonString(text) + "\"}]}}";
}

std::string PullRequestFor(std::shared_ptr<FeedableIStream> const &input,
                           std::shared_ptr<StringOStream> const &output,
                           std::string const &prefix, std::string const &uri) {
  return PollRequest(input, output, prefix, "\"kind\":\"full\"",
                     [&](std::string const &id) { return PullFrame(id, uri); });
}

std::string
PullRequestForPrevious(std::shared_ptr<FeedableIStream> const &input,
                       std::shared_ptr<StringOStream> const &output,
                       std::string const &prefix, std::string const &uri,
                       std::string const &prev) {
  return PollRequest(input, output, prefix,
                     "\"kind\":", [&](std::string const &id) {
                       return PullFrameWithPrevious(id, uri, prev);
                     });
}

std::size_t CountCode(std::string const &report, std::string const &code) {
  std::string const needle = std::string("\"code\":\"") + code + "\"";
  return CountOf(report, needle);
}

// Two `Public` constructors whose names differ only by case are one link
// symbol: both files report, each naming the other file.
void TestDuplicateConstructorPair() {
  CollisionFixture fix;
  std::string const firstText =
      "Sub Init() Constructor\n  Print \"a\"\nEnd Sub\n";
  std::string const secondText =
      "Sub INIT() Constructor\n  Print \"b\"\nEnd Sub\n";
  WriteFile(fix.ws / "first.bas", firstText.c_str());
  WriteFile(fix.ws / "second.bas", secondText.c_str());

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
  input->append(MakeLspFrame(OpenFrame(fix.firstUri, firstText).c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.secondUri, secondText).c_str()));

  std::string const first = PullRequestFor(input, output, "cc1", fix.firstUri);
  Expect(CountCode(first, "duplicate-public-procedure") == 1,
         "the first constructor must report exactly one collision");
  Expect(first.find("\"severity\":1") != std::string::npos,
         "the collision is an Error (severity 1)");
  Expect(first.find("duplicate public procedure 'INIT'") != std::string::npos,
         "the message names the uppercased link symbol");
  Expect(first.find("second.bas") != std::string::npos,
         "the message names the other defining file");
  Expect(first.find("\"start\":{\"line\":0,\"character\":4},\"end\":"
                    "{\"line\":0,\"character\":8}") != std::string::npos,
         "the diagnostic anchors on the definition's name token");

  std::string const second =
      PullRequestFor(input, output, "cc2", fix.secondUri);
  Expect(CountCode(second, "duplicate-public-procedure") == 1,
         "the second constructor must report exactly one collision");
  Expect(second.find("duplicate public procedure 'INIT'") != std::string::npos,
         "the second file's message also names the uppercased symbol");
  Expect(second.find("first.bas") != std::string::npos,
         "the second file's message names the first file");

  session.stop();
}

// `Private` procedures are file-local: one `Private` in the pair ends the
// collision.
void TestPrivateConstructorSuppresses() {
  CollisionFixture fix;
  std::string const firstText =
      "Sub Init() Constructor\n  Print \"a\"\nEnd Sub\n";
  std::string const secondText =
      "Private Sub INIT() Constructor\n  Print \"b\"\nEnd Sub\n";
  WriteFile(fix.ws / "first.bas", firstText.c_str());
  WriteFile(fix.ws / "second.bas", secondText.c_str());

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
  input->append(MakeLspFrame(OpenFrame(fix.firstUri, firstText).c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.secondUri, secondText).c_str()));

  std::string const first = PullRequestFor(input, output, "cc3", fix.firstUri);
  Expect(CountCode(first, "duplicate-public-procedure") == 0,
         "a `Private` twin leaves the public one alone");
  std::string const second =
      PullRequestFor(input, output, "cc4", fix.secondUri);
  Expect(CountCode(second, "duplicate-public-procedure") == 0,
         "a `Private` constructor never reports");

  session.stop();
}

// Collisions do not cross a root boundary.
void TestDuplicateAcrossRootsNoCollision() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const fa = sandbox / "fa";
  std::filesystem::path const fb = sandbox / "fb";
  std::filesystem::create_directories(fa);
  std::filesystem::create_directories(fb);
  std::string const aText = "Sub Init() Constructor\n  Print \"a\"\nEnd Sub\n";
  std::string const bText = "Sub INIT() Constructor\n  Print \"b\"\nEnd Sub\n";
  WriteFile(fa / "first.bas", aText.c_str());
  WriteFile(fb / "second.bas", bText.c_str());

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const faUri = FileUri(fa);
  std::string const fbUri = FileUri(fb);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"capabilities":{"textDocument":{"diagnostic":{"relatedDocumentSupport":true}},"workspace":{"diagnostics":{"refreshSupport":true}}},"workspaceFolders":[)"
      R"({"uri":")" +
      faUri + R"(","name":"fa"},{"uri":")" + fbUri + R"(","name":"fb"}]}})";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"diagnosticProvider\"") != std::string::npos,
         "initialize must advertise the pull provider");
  std::string const aUri = FileUri(fa / "first.bas");
  std::string const bUri = FileUri(fb / "second.bas");
  input->append(MakeLspFrame(OpenFrame(aUri, aText).c_str()));
  input->append(MakeLspFrame(OpenFrame(bUri, bText).c_str()));

  std::string const a = PullRequestFor(input, output, "cc5", aUri);
  Expect(CountCode(a, "duplicate-public-procedure") == 0,
         "a constructor in a sibling root is not a collision");
  std::string const b = PullRequestFor(input, output, "cc6", bUri);
  Expect(CountCode(b, "duplicate-public-procedure") == 0,
         "neither root reports");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// Declarations-only files, distinct constructor names, and a member
// constructor whose name happens to match are not definitions and do not
// collide.
void TestDistinctAndDeclarationsDoNotCollide() {
  CollisionFixture fix;
  std::string const ctorText =
      "Sub Init() Constructor\n  Print \"a\"\nEnd Sub\n";
  std::string const declText = "Declare Sub Init()\n";
  std::string const otherText =
      "Sub Other() Constructor\n  Print \"b\"\nEnd Sub\n";
  std::string const udtText = "Type Widget\n"
                              "  x As Integer\n"
                              "  Declare Static Sub Init()\n"
                              "End Type\n"
                              "Sub Widget.Init() Constructor\n"
                              "  Print \"w\"\n"
                              "End Sub\n";
  WriteFile(fix.ws / "first.bas", ctorText.c_str());
  WriteFile(fix.ws / "second.bas", declText.c_str());
  WriteFile(fix.ws / "third.bas", otherText.c_str());
  std::string const udtUri = FileUri(fix.ws / "udt.bas");
  WriteFile(fix.ws / "udt.bas", udtText.c_str());

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
  input->append(MakeLspFrame(OpenFrame(fix.firstUri, ctorText).c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.secondUri, declText).c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.thirdUri, otherText).c_str()));
  input->append(MakeLspFrame(OpenFrame(udtUri, udtText).c_str()));

  std::string const first = PullRequestFor(input, output, "cc7", fix.firstUri);
  Expect(CountCode(first, "duplicate-public-procedure") == 0,
         "a `Declare` and a distinct constructor do not collide with it");
  std::string const second =
      PullRequestFor(input, output, "cc8", fix.secondUri);
  Expect(CountCode(second, "duplicate-public-procedure") == 0,
         "a declarations-only file never reports");
  std::string const third = PullRequestFor(input, output, "cc9", fix.thirdUri);
  Expect(CountCode(third, "duplicate-public-procedure") == 0,
         "a distinct constructor name does not collide");
  std::string const udt = PullRequestFor(input, output, "c10", udtUri);
  Expect(CountCode(udt, "duplicate-public-procedure") == 0,
         "a member constructor does not collide with a module constructor");

  session.stop();
}

// Three files defining one name: each reports, naming the other two.
void TestThreeFilesEachNamesOtherTwo() {
  CollisionFixture fix;
  std::string const firstText =
      "Sub Init() Constructor\n  Print \"a\"\nEnd Sub\n";
  std::string const secondText =
      "Sub Init() Constructor\n  Print \"b\"\nEnd Sub\n";
  std::string const thirdText =
      "Sub Init() Constructor\n  Print \"c\"\nEnd Sub\n";
  WriteFile(fix.ws / "first.bas", firstText.c_str());
  WriteFile(fix.ws / "second.bas", secondText.c_str());
  WriteFile(fix.ws / "third.bas", thirdText.c_str());

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
  input->append(MakeLspFrame(OpenFrame(fix.firstUri, firstText).c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.secondUri, secondText).c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.thirdUri, thirdText).c_str()));

  std::string const first = PullRequestFor(input, output, "cc11", fix.firstUri);
  Expect(CountCode(first, "duplicate-public-procedure") == 1,
         "each of three files reports");
  Expect(first.find("second.bas") != std::string::npos &&
             first.find("third.bas") != std::string::npos,
         "the message names both other files");
  std::string const second =
      PullRequestFor(input, output, "cc12", fix.secondUri);
  Expect(CountCode(second, "duplicate-public-procedure") == 1,
         "the second of three reports");
  Expect(second.find("first.bas") != std::string::npos &&
             second.find("third.bas") != std::string::npos,
         "its message names the other two");
  std::string const third = PullRequestFor(input, output, "cc13", fix.thirdUri);
  Expect(CountCode(third, "duplicate-public-procedure") == 1,
         "the third of three reports");
  Expect(third.find("first.bas") != std::string::npos &&
             third.find("second.bas") != std::string::npos,
         "its message names the other two");

  session.stop();
}

// Editing one file to rename its constructor clears the diagnostic in the
// untouched file: its delivered payload and resultId move.
void TestRenameClearsOtherFile() {
  CollisionFixture fix;
  std::string const firstText =
      "Sub Init() Constructor\n  Print \"a\"\nEnd Sub\n";
  std::string const secondText =
      "Sub INIT() Constructor\n  Print \"b\"\nEnd Sub\n";
  WriteFile(fix.ws / "first.bas", firstText.c_str());
  WriteFile(fix.ws / "second.bas", secondText.c_str());

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
  input->append(MakeLspFrame(OpenFrame(fix.firstUri, firstText).c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.secondUri, secondText).c_str()));

  std::string const before =
      PullRequestFor(input, output, "cc14", fix.secondUri);
  Expect(CountCode(before, "duplicate-public-procedure") == 1,
         "before the rename the untouched file reports");
  std::string const beforeId = ResultIdOf(before);
  Expect(!beforeId.empty(), "the full report must carry a resultId");

  // Rename `Init` to `Renamed` in the first file: `Sub Init()` becomes
  // `Sub Renamed()` — replace characters 4..8 on line 0.
  input->append(
      MakeLspFrame(ReplaceFrame(fix.firstUri, 0, 4, 0, 8, "Renamed").c_str()));

  std::string const after =
      PullRequestForPrevious(input, output, "cc15", fix.secondUri, beforeId);
  Expect(after.find("\"kind\":\"full\"") != std::string::npos,
         "the untouched file must not be told `unchanged` after the edit");
  Expect(after.find("duplicate-public-procedure") == std::string::npos,
         "the rename clears the diagnostic in the untouched file");
  std::string const afterId = ResultIdOf(after);
  Expect(!afterId.empty() && afterId != beforeId,
         "the collision fold moves the untouched file's resultId");

  session.stop();
}

// Plain `Public Sub` procedures collide by the same uppercased-name rule.
void TestPlainSubPairCollides() {
  CollisionFixture fix;
  std::string const firstText = "Public Sub Helper()\n  Print \"a\"\nEnd Sub\n";
  std::string const secondText =
      "Public Sub Helper()\n  Print \"b\"\nEnd Sub\n";
  WriteFile(fix.ws / "first.bas", firstText.c_str());
  WriteFile(fix.ws / "second.bas", secondText.c_str());

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
  input->append(MakeLspFrame(OpenFrame(fix.firstUri, firstText).c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.secondUri, secondText).c_str()));

  std::string const first = PullRequestFor(input, output, "cc16", fix.firstUri);
  Expect(CountCode(first, "duplicate-public-procedure") == 1,
         "two `Public Sub` with one name collide");
  Expect(first.find("duplicate public procedure 'HELPER'") != std::string::npos,
         "the message names the uppercased symbol");
  Expect(first.find("second.bas") != std::string::npos,
         "its message names the other file");
  std::string const second =
      PullRequestFor(input, output, "cc17", fix.secondUri);
  Expect(CountCode(second, "duplicate-public-procedure") == 1,
         "both plain procedures report");
  Expect(second.find("first.bas") != std::string::npos, "each names the other");

  session.stop();
}

// Plain `Public Function` procedures with one signature collide.
void TestPlainFunctionPairCollides() {
  CollisionFixture fix;
  std::string const firstText = "Public Function F(x As Integer) As Integer\n  "
                                "Return x + 1\nEnd Function\n";
  std::string const secondText = "Public Function F(x As Integer) As Integer\n "
                                 " Return x + 2\nEnd Function\n";
  WriteFile(fix.ws / "first.bas", firstText.c_str());
  WriteFile(fix.ws / "second.bas", secondText.c_str());

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
  input->append(MakeLspFrame(OpenFrame(fix.firstUri, firstText).c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.secondUri, secondText).c_str()));

  std::string const first = PullRequestFor(input, output, "cc18", fix.firstUri);
  Expect(CountCode(first, "duplicate-public-procedure") == 1,
         "two `Public Function` with one name and signature collide");
  Expect(first.find("duplicate public procedure 'F'") != std::string::npos,
         "the message names the uppercased symbol");
  Expect(first.find("second.bas") != std::string::npos,
         "its message names the other file");
  std::string const second =
      PullRequestFor(input, output, "cc19", fix.secondUri);
  Expect(CountCode(second, "duplicate-public-procedure") == 1,
         "both functions report");
  Expect(second.find("first.bas") != std::string::npos, "each names the other");

  session.stop();
}

// Module destructor pairs collide exactly like constructors.
void TestDestructorPairCollides() {
  CollisionFixture fix;
  std::string const firstText =
      "Public Sub Cleanup() Destructor\n  Print \"a\"\nEnd Sub\n";
  std::string const secondText =
      "Public Sub Cleanup() Destructor\n  Print \"b\"\nEnd Sub\n";
  WriteFile(fix.ws / "first.bas", firstText.c_str());
  WriteFile(fix.ws / "second.bas", secondText.c_str());

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
  input->append(MakeLspFrame(OpenFrame(fix.firstUri, firstText).c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.secondUri, secondText).c_str()));

  std::string const first = PullRequestFor(input, output, "cc20", fix.firstUri);
  Expect(CountCode(first, "duplicate-public-procedure") == 1,
         "two `Destructor` with one name collide");
  Expect(first.find("duplicate public procedure 'CLEANUP'") !=
             std::string::npos,
         "the message names the uppercased symbol");
  std::string const second =
      PullRequestFor(input, output, "cc21", fix.secondUri);
  Expect(CountCode(second, "duplicate-public-procedure") == 1,
         "both destructors report");

  session.stop();
}

// Constructor against plain procedure: one link symbol, so they collide.
void TestConstructorAgainstPlainCollides() {
  CollisionFixture fix;
  std::string const firstText =
      "Public Sub Init() Constructor\n  Print \"ctor\"\nEnd Sub\n";
  std::string const secondText =
      "Public Sub Init()\n  Print \"plain\"\nEnd Sub\n";
  WriteFile(fix.ws / "first.bas", firstText.c_str());
  WriteFile(fix.ws / "second.bas", secondText.c_str());

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
  input->append(MakeLspFrame(OpenFrame(fix.firstUri, firstText).c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.secondUri, secondText).c_str()));

  std::string const first = PullRequestFor(input, output, "cc22", fix.firstUri);
  Expect(CountCode(first, "duplicate-public-procedure") == 1,
         "a constructor and a plain procedure with one name collide");
  Expect(first.find("second.bas") != std::string::npos,
         "its message names the other file");
  std::string const second =
      PullRequestFor(input, output, "cc23", fix.secondUri);
  Expect(CountCode(second, "duplicate-public-procedure") == 1,
         "both sides report");
  Expect(second.find("first.bas") != std::string::npos, "each names the other");

  session.stop();
}

// `Overload` procedures with different signatures mangle to distinct link
// symbols and do not collide — the nearest category the probe left out.
void TestOverloadDifferentSignaturesDoNotCollide() {
  CollisionFixture fix;
  std::string const firstText = "Public Function F Overload(x As Integer) As "
                                "Integer\n  Return x\nEnd Function\n";
  std::string const secondText = "Public Function F Overload(x As String) As "
                                 "String\n  Return x\nEnd Function\n";
  WriteFile(fix.ws / "first.bas", firstText.c_str());
  WriteFile(fix.ws / "second.bas", secondText.c_str());

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
  input->append(MakeLspFrame(OpenFrame(fix.firstUri, firstText).c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.secondUri, secondText).c_str()));

  std::string const first = PullRequestFor(input, output, "cc24", fix.firstUri);
  Expect(CountCode(first, "duplicate-public-procedure") == 0,
         "`Overload` with different signatures mangles to distinct symbols");
  std::string const second =
      PullRequestFor(input, output, "cc25", fix.secondUri);
  Expect(CountCode(second, "duplicate-public-procedure") == 0,
         "neither overload reports");

  session.stop();
}

// `Overload` procedures with one signature still collide.
void TestOverloadSameSignatureCollides() {
  CollisionFixture fix;
  std::string const firstText = "Public Function F Overload(x As Integer) As "
                                "Integer\n  Return x\nEnd Function\n";
  std::string const secondText = "Public Function F Overload(x As Integer) As "
                                 "Integer\n  Return x + 1\nEnd Function\n";
  WriteFile(fix.ws / "first.bas", firstText.c_str());
  WriteFile(fix.ws / "second.bas", secondText.c_str());

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
  input->append(MakeLspFrame(OpenFrame(fix.firstUri, firstText).c_str()));
  input->append(MakeLspFrame(OpenFrame(fix.secondUri, secondText).c_str()));

  std::string const first = PullRequestFor(input, output, "cc26", fix.firstUri);
  Expect(CountCode(first, "duplicate-public-procedure") == 1,
         "`Overload` with one signature collides");
  Expect(first.find("second.bas") != std::string::npos,
         "its message names the other file");
  std::string const second =
      PullRequestFor(input, output, "cc27", fix.secondUri);
  Expect(CountCode(second, "duplicate-public-procedure") == 1,
         "both overloads report");

  session.stop();
}

// A single file with one `Public` constructor reports nothing new: the
// no-config default stays silent.
void TestSinglePublicConstructorSilent() {
  CollisionFixture fix;
  std::string const text =
      "Public Sub Init() Constructor\n  Print \"a\"\nEnd Sub\n";
  WriteFile(fix.ws / "first.bas", text.c_str());

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
  input->append(MakeLspFrame(OpenFrame(fix.firstUri, text).c_str()));

  std::string const report =
      PullRequestFor(input, output, "cc28", fix.firstUri);
  Expect(CountCode(report, "duplicate-public-procedure") == 0,
         "one `Public` constructor alone never reports");

  session.stop();
}

} // namespace

namespace fbtest {

void RunCollisionsTests() {
  RUN_TEST(TestDuplicateConstructorPair);
  RUN_TEST(TestPrivateConstructorSuppresses);
  RUN_TEST(TestDuplicateAcrossRootsNoCollision);
  RUN_TEST(TestDistinctAndDeclarationsDoNotCollide);
  RUN_TEST(TestThreeFilesEachNamesOtherTwo);
  RUN_TEST(TestRenameClearsOtherFile);
  RUN_TEST(TestPlainSubPairCollides);
  RUN_TEST(TestPlainFunctionPairCollides);
  RUN_TEST(TestDestructorPairCollides);
  RUN_TEST(TestConstructorAgainstPlainCollides);
  RUN_TEST(TestOverloadDifferentSignaturesDoNotCollide);
  RUN_TEST(TestOverloadSameSignatureCollides);
  RUN_TEST(TestSinglePublicConstructorSilent);
}

} // namespace fbtest
