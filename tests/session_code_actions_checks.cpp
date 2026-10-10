/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// M12 quick fixes over the wire.
//
// The missing-include diagnostic and the three fixes that answer it, plus the
// empty-result case (a diagnostic with no fix, and a `context.only` filter that
// excludes the one fix).
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {
// M12 quick-fix fixtures: a sandbox holding the real header one directory
// down, so `#include "config.bai"` resolves nowhere while `inc/config.bi` is a
// workspace file the retarget fix can find.
struct CodeActionFixture {
  std::filesystem::path sandbox;
  std::string mainUri;

  CodeActionFixture() {
    static std::atomic<long> counter{0};
    sandbox = std::filesystem::temp_directory_path() /
              ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
               std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(sandbox / "inc");
    {
      std::ofstream out(sandbox / "inc" / "config.bi");
      out << "dim shared cfg as integer\n";
    }
    mainUri = FileUri(sandbox / "main.bas");
  }

  ~CodeActionFixture() {
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
  }
};

// A codeAction request for line `line` of `uri`, with an empty client-side
// diagnostic list (what a client sends when the user asks from the lightbulb
// with no selection) and an optional `only` kind filter. `endChar` 0 asks at
// the line's first character — a bare cursor; anything larger spans the line,
// which is how a client asks for every diagnostic on it.
std::string CodeActionFrame(std::string const &id, std::string const &uri,
                            int line, std::string const &only = {},
                            int endChar = 0) {
  std::string frame =
      "{\"jsonrpc\":\"2.0\",\"id\":\"" + id +
      "\",\"method\":\"textDocument/codeAction\",\"params\":{\"textDocument\":"
      "{\"uri\":\"" +
      uri + "\"},\"range\":{\"start\":{\"line\":" + std::to_string(line) +
      ",\"character\":0},\"end\":{\"line\":" + std::to_string(line) +
      ",\"character\":" + std::to_string(endChar) +
      "}},\"context\":{\"diagnostics\":[]";
  if (!only.empty()) {
    frame += ",\"only\":[\"" + only + "\"]";
  }
  frame += "}}}";
  return frame;
}

std::string OpenFrame(std::string const &uri, std::string const &text) {
  return R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
         R"({"uri":")" +
         uri + R"(","languageId":"basic","version":1,"text":")" +
         ToJsonString(text) + "\"}}}";
}

// A didChange that replaces [sl,sc]-[el,ec] with `text`, as applying a
// WorkspaceEdit looks to the server.
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

void TestCodeActionInsertsMissingCloser() {
  CodeActionFixture fix;
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  // The SUB never closes, so the parser publishes the missing-closer Error at
  // the opener, as fbc's own `error 125: Expected 'END SUB'`, and nothing else.
  input->append(
      MakeLspFrame(OpenFrame(fix.mainUri, "sub main()\n  print 1\n").c_str()));
  std::string const published = WaitForPublishedUri(output, 1);
  Expect(published.find("\"code\":\"fbc error: 125\"") != std::string::npos,
         "an unterminated block must publish its diagnostic before it is "
         "fixable");
  Expect(published.find(
             "\"codeDescription\":{\"href\":\"https://www.freebasic.net/wiki/"
             "CompilerErrMsg\"}") != std::string::npos,
         "an fbc-numbered diagnostic must link the wiki that documents it");

  std::string const reply =
      PollRequest(input, output, "cacl", "Insert", [&](std::string const &id) {
        return CodeActionFrame(id, fix.mainUri, 0);
      });
  Expect(reply.find("\"title\":\"Insert 'END SUB'\"") != std::string::npos,
         "a code action must be titled with the closer it inserts");
  // A `CodeAction` carrying an `edit`, not a `Command`: the client applies an
  // edit and executes a command id, so a fix shipped as a command (or with a
  // bare path for the `changes` key) lists in the menu and then does nothing.
  Expect(reply.find("\"kind\":\"quickfix\"") != std::string::npos,
         "a fix must carry the kind the client filters on");
  Expect(reply.find("\"command\"") == std::string::npos,
         "a fix must not ship as a command the client would have to execute");
  Expect(reply.find("\"edit\":{\"changes\":{\"") != std::string::npos,
         "a fix must carry the edit the client applies");
  Expect(reply.find("\"diagnostics\":[{\"range\":{\"start\":{\"line\":0,"
                    "\"character\":0}") != std::string::npos,
         "a fix must echo the diagnostic it answers");
  Expect(reply.find("\"newText\":\"END SUB\\n\"") != std::string::npos,
         "the fix must insert the closer with its own line ending");
  Expect(reply.find("\"start\":{\"line\":2,\"character\":0}") !=
             std::string::npos,
         "the closer must land at the end of the buffer");
  // The `changes` key must be the document's URI, verbatim: a key the client
  // cannot match to an open buffer is an edit it silently discards.
  Expect(reply.find("\"" + fix.mainUri + "\":[{\"range\"") != std::string::npos,
         "the edit must be keyed by the request's own document URI");

  // Applying the fix's edit clears the diagnostic it answers: the re-parse sees
  // a closed block and publishes nothing. The inserted closer is uppercase, so
  // this also rides on keywords closing blocks whatever their case.
  input->append(
      MakeLspFrame(ReplaceFrame(fix.mainUri, 2, 0, 2, 0, "END SUB\n").c_str()));
  Expect(LastPublish(WaitForPublishedUri(output, 2)).find("fbc error: 125") ==
             std::string::npos,
         "applying the closer fix must clear the diagnostic on re-parse");

  session.stop();
}

void TestCodeActionRetargetsMissingInclude() {
  CodeActionFixture fix;
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

  // A mistyped extension: `config.bai` resolves nowhere, `inc/config.bi` does.
  input->append(MakeLspFrame(
      OpenFrame(fix.mainUri, "#include \"config.bai\"\nprint 1\n").c_str()));
  Expect(WaitForPublishedUri(output, 1).find(
             "\"code\":\"include-not-found\"") != std::string::npos,
         "an unresolvable include must publish before it is fixable");

  // The candidate comes from the index, so this retries until the background
  // scan has read the sandbox. The request spans the whole line: a client
  // asking about the directive names a range covering it.
  std::string const reply = PollRequest(
      input, output, "cainc", "Change include to", [&](std::string const &id) {
        return CodeActionFrame(id, fix.mainUri, 0, {}, 1000);
      });
  Expect(reply.find("\"title\":\"Change include to \\\"inc/config.bi\\\"\"") !=
             std::string::npos,
         "the include fix must name the target it would write");
  Expect(reply.find("\"start\":{\"line\":0,\"character\":10},\"end\":"
                    "{\"line\":0,\"character\":20}") != std::string::npos,
         "the include fix must replace the literal, quotes left in place");
  Expect(reply.find("\"newText\":\"inc/config.bi\"") != std::string::npos,
         "the include fix must write the resolvable literal");
  Expect(reply.find("\"" + fix.mainUri + "\":[{\"range\"") != std::string::npos,
         "the include fix's edit must be keyed by the document URI");

  input->append(MakeLspFrame(
      ReplaceFrame(fix.mainUri, 0, 10, 0, 20, "inc/config.bi").c_str()));
  Expect(
      LastPublish(WaitForPublishedUri(output, 2)).find("include-not-found") ==
          std::string::npos,
      "retargeting the include must clear the diagnostic on re-parse");

  session.stop();
}

void TestCodeActionRemovesKeywordSuffix() {
  CodeActionFixture fix;
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  // `print%` is PRINT with an ignored suffix (fbc warning 44), so the parser
  // publishes a Warning on the suffix char alone.
  input->append(MakeLspFrame(OpenFrame(fix.mainUri, "print% 1\n").c_str()));
  std::string const published = WaitForPublishedUri(output, 1);
  Expect(published.find("\"code\":\"keyword-suffix\"") != std::string::npos,
         "a suffixed keyword must publish its warning before it is fixable");
  Expect(published.find("\"severity\":2") != std::string::npos,
         "the ignored suffix is a warning, as fbc's warning 44 is");
  Expect(published.find("\"start\":{\"line\":0,\"character\":5},\"end\":"
                        "{\"line\":0,\"character\":6}") != std::string::npos,
         "the warning must sit on the suffix char alone");

  // The request spans the whole line, so the warning's range is inside it.
  std::string const reply = PollRequest(
      input, output, "casfx", "Remove the", [&](std::string const &id) {
        return CodeActionFrame(id, fix.mainUri, 0, {}, 1000);
      });
  Expect(reply.find("\"title\":\"Remove the '%' suffix\"") != std::string::npos,
         "the fix must name the suffix it removes");
  Expect(reply.find("\"kind\":\"quickfix\"") != std::string::npos,
         "the suffix fix must carry the kind the client filters on");
  Expect(reply.find("\"command\"") == std::string::npos,
         "the suffix fix must ship as an edit, not a command");
  Expect(reply.find("\"newText\":\"\"") != std::string::npos,
         "removing a suffix is a pure deletion");
  Expect(reply.find("\"" + fix.mainUri + "\":[{\"range\"") != std::string::npos,
         "the suffix fix's edit must be keyed by the document URI");

  // Applying the deletion clears the warning on re-parse.
  input->append(
      MakeLspFrame(ReplaceFrame(fix.mainUri, 0, 5, 0, 6, "").c_str()));
  Expect(LastPublish(WaitForPublishedUri(output, 2)).find("keyword-suffix") ==
             std::string::npos,
         "removing the suffix must clear the warning on re-parse");

  session.stop();
}

void TestCodeActionOffersNothingUnfixable() {
  CodeActionFixture fix;
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  // A stray closer is a parse diagnostic with no registered fix, and it is
  // also the request's whole range, so nothing may be offered for it.
  input->append(
      MakeLspFrame(OpenFrame(fix.mainUri, "end sub\nprint 1\n").c_str()));
  Expect(WaitForPublishedUri(output, 1).find("\"code\":\"fbc error: 112\"") !=
             std::string::npos,
         "a stray closer must publish its diagnostic");
  std::string const unfixable = PollRequest(
      input, output, "caun", "\"result\":[]", [&](std::string const &id) {
        return CodeActionFrame(id, fix.mainUri, 0);
      });
  Expect(unfixable.find("\"result\":[]") != std::string::npos,
         "a diagnostic with no registered fix must offer no code actions");

  // And a request for a kind this server does not serve gets nothing, even
  // where a fix exists: every fix is a quickfix, so the filter is honored here
  // rather than by the client.
  input->append(
      MakeLspFrame(OpenFrame(fix.mainUri, "sub main()\n  print 1\n").c_str()));
  WaitForPublishedUri(output, 2);
  std::string const refactor = PollRequest(
      input, output, "caref", "\"result\":[]", [&](std::string const &id) {
        return CodeActionFrame(id, fix.mainUri, 0, "refactor");
      });
  Expect(refactor.find("\"result\":[]") != std::string::npos,
         "a non-quickfix kind filter must return no code actions");
  std::string const quickfix =
      PollRequest(input, output, "caqf", "Insert", [&](std::string const &id) {
        return CodeActionFrame(id, fix.mainUri, 0, "quickfix");
      });
  Expect(quickfix.find("Insert 'END SUB'") != std::string::npos,
         "an explicit quickfix filter must still return the fixes");

  session.stop();
}

void TestMissingIncludePublishesDiagnostic() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(sandbox);
  {
    std::ofstream out(sandbox / "ok.bi");
    out << "dim okVal as integer\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const badUri = FileUri(sandbox / "main.bas");
  std::string const badOpenFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      badUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("#include \"missing.bi\"\nprint \"hi\"\n") + "\"}}}";
  input->append(MakeLspFrame(badOpenFrame.c_str()));
  std::string const published = WaitForPublishedUri(output, 1);
  Expect(published.find("\"code\":\"include-not-found\"") != std::string::npos,
         "a missing include must be reported with its diagnostic code");
  Expect(published.find("include file not found") != std::string::npos,
         "a missing include must carry a readable message");
  Expect(published.find("missing.bi") != std::string::npos,
         "the diagnostic must name the missing file");
  Expect(published.find("\"severity\":1") != std::string::npos,
         "a missing include must publish at Error severity");
  Expect(
      published.find("\"start\":{\"line\":0,\"character\":10}") !=
          std::string::npos,
      "the include range must cover the filename literal, not the whole line");

  // A resolvable include must not produce an include-not-found diagnostic.
  std::string const goodUri = FileUri(sandbox / "uses.bas");
  std::string const goodOpenFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      goodUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("#include \"ok.bi\"\n") + "\"}}}";
  input->append(MakeLspFrame(goodOpenFrame.c_str()));
  std::string const both = WaitForPublishedUri(output, 2);
  std::size_t notFound = 0;
  std::size_t pos = 0;
  while ((pos = both.find("\"code\":\"include-not-found\"", pos)) !=
         std::string::npos) {
    ++notFound;
    pos += 1;
  }
  Expect(notFound == 1,
         "a resolvable include must not publish include-not-found");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}
} // namespace

namespace fbtest {

void RunCodeActionsTests() {
  RUN_TEST(TestCodeActionInsertsMissingCloser);
  RUN_TEST(TestCodeActionRetargetsMissingInclude);
  RUN_TEST(TestCodeActionRemovesKeywordSuffix);
  RUN_TEST(TestCodeActionOffersNothingUnfixable);
  RUN_TEST(TestMissingIncludePublishesDiagnostic);
}

} // namespace fbtest
