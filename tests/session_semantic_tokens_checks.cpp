/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// M9 semantic tokens and inlay hints.
//
// Full-then-delta with the result id, the capability negotiation that decides
// whether a delta is ever sent, the viewport request, and the inlay hints.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {
// A semantic-tokens document: `dim`/`as` keywords, a declared variable with a
// declaration modifier, its usage, `=`/`+` operators, and two numbers. The
// trailing `1` becomes `12` in the change frame, which shifts the last token.
char const kSemOpenFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/fblsp-sem.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"dim counter as integer\ncounter = 1\n"}}})FB";

char const kSemChangeFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/fblsp-sem.bas","version":2},"contentChanges":[{"range":)FB"
    R"FB({"start":{"line":1,"character":10},"end":{"line":1,"character":11}},"text":"12"}]}})FB";

char const *kSemFullFrame =
    R"FB({"jsonrpc":"2.0","id":"stfull","method":"textDocument/semanticTokens/full","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/fblsp-sem.bas"}}})FB";

// A client that opts into the viewport (`range`) semantic-tokens request.
char const kInitializeSemRangeFrame[] =
    R"FB({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"capabilities":)FB"
    R"FB({"textDocument":{"semanticTokens":{"requests":{"range":true}}}}}})FB";

// Viewport over line 1 only; the response must drop the line-0 tokens.
char const *kSemRangeFrame =
    R"FB({"jsonrpc":"2.0","id":"strag","method":"textDocument/semanticTokens/range","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/fblsp-sem.bas"},"range":{"start":{"line":1,"character":0},)FB"
    R"FB("end":{"line":1,"character":15}}}})FB";

// Inlay-hint document: a SUB block (closer hint) and a suffix-typed dim without
// an AS clause (inferred-type hint).
char const kInlayOpenFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/fblsp-inlay.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"sub greet()\n    print 1\nend sub\ndim x$\n"}}})FB";

char const *kInlayHintFrame =
    R"FB({"jsonrpc":"2.0","id":"inh","method":"textDocument/inlayHint","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/fblsp-inlay.bas"},"range":{"start":{"line":0,"character":0},)FB"
    R"FB("end":{"line":3,"character":0}}}})FB";

void TestSemanticTokensFullThenDelta() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kSemOpenFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "semantic-token document open must publish diagnostics");

  input->append(MakeLspFrame(kSemFullFrame));
  std::string const full =
      TailAfter(WaitForOutputContaining(output, "\"id\":\"stfull\""),
                "\"id\":\"stfull\"");
  Expect(full.find("\"data\":[0,0,3,0,0,0,4,7,6,1,0,8,2,0,0,0,3,7,0,0,"
                   "1,0,7,6,0,0,8,1,5,0,0,2,1,2,0]") != std::string::npos,
         "full semantic tokens must carry the relative-encoded data array");
  std::string const baseline = ResultIdOf(full);
  Expect(!baseline.empty(),
         "full semantic tokens must carry a resultId for later deltas");

  // Change the trailing literal `1` -> `12`: only the last token's length
  // shifts, so the delta is a single tail replacement.
  input->append(MakeLspFrame(kSemChangeFrame));
  Expect(WaitForPublishedUri(output, 2).empty() == false,
         "the edit must publish diagnostics so the buffer is updated");

  input->append(MakeLspFrame(SemDeltaFrame("semd1", baseline)));
  std::string const changed = TailAfter(
      WaitForOutputContaining(output, "\"id\":\"semd1\""), "\"id\":\"semd1\"");
  Expect(changed.find("\"edits\":[{\"start\":30,\"deleteCount\":5,"
                      "\"data\":[0,2,2,2,0]}]") != std::string::npos,
         "the delta must tail-replace the last token with flat-array element "
         "offsets (start/deleteCount x5) and the spec data name");
  std::string const changedId = ResultIdOf(changed);
  Expect(!changedId.empty() && changedId != baseline,
         "a changed delta must carry a fresh resultId");

  // Re-request against the unchanged buffer: an empty edit list and the same
  // resultId, so the client can keep chaining deltas.
  input->append(MakeLspFrame(SemDeltaFrame("semd2", changedId)));
  std::string const unchanged = TailAfter(
      WaitForOutputContaining(output, "\"id\":\"semd2\""), "\"id\":\"semd2\"");
  Expect(unchanged.find("\"edits\":[]") != std::string::npos,
         "an unchanged buffer must yield an empty edit list");
  Expect(unchanged.find("\"resultId\":\"" + changedId + "\"") !=
             std::string::npos,
         "an empty delta must keep the same resultId");

  // An unknown previousResultId (stale, evicted, or a range id) falls back to
  // a full response through the `tokens` arm.
  input->append(MakeLspFrame(SemDeltaFrame("semd3", "stNope")));
  std::string const fallback = TailAfter(
      WaitForOutputContaining(output, "\"id\":\"semd3\""), "\"id\":\"semd3\"");
  Expect(fallback.find("\"tokens\":[") != std::string::npos,
         "an unknown previousResultId must fall back to full tokens");
  Expect(fallback.find("\"edits\"") == std::string::npos,
         "a full fallback must not include an edits field");

  session.stop();
}

void TestSemanticTokensCapabilities() {
  // Without a client request for the viewport provider, advertise the legend
  // and full+delta but no range.
  {
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
    std::size_t const beg = response.find("\"semanticTokensProvider\"");
    Expect(beg != std::string::npos,
           "initialize must advertise semanticTokensProvider");
    std::string const provider = response.substr(beg);
    Expect(provider.find("\"legend\"") != std::string::npos &&
               provider.find("\"tokenTypes\"") != std::string::npos,
           "semanticTokensProvider must carry the token legend");
    Expect(provider.find("\"full\":{\"delta\":true}") != std::string::npos,
           "semanticTokensProvider must advertise full/delta support");
    Expect(provider.find("\"range\":") == std::string::npos,
           "a client that did not request range must not be offered it");
    Expect(response.find("\"inlayHintProvider\"") != std::string::npos,
           "initialize must advertise inlayHint support");

    session.stop();
  }

  // With `textDocument.semanticTokens.requests.range`, advertise the range
  // provider too.
  {
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();

    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kInitializeSemRangeFrame));
    std::string const response =
        WaitForOutputContaining(output, "\"id\":\"init\"");
    std::size_t const beg = response.find("\"semanticTokensProvider\"");
    Expect(beg != std::string::npos,
           "initialize must advertise semanticTokensProvider");
    Expect(response.substr(beg).find("\"range\":true") != std::string::npos,
           "a client requesting range must be offered the range provider");

    session.stop();
  }
}

void TestSemanticTokensRangeAndFullOnlyCache() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kSemOpenFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "semantic-token document open must publish diagnostics");

  // Viewport over line 1: only that line's three tokens, re-based so the first
  // entry reports deltaLine 1.
  input->append(MakeLspFrame(kSemRangeFrame));
  std::string const range = TailAfter(
      WaitForOutputContaining(output, "\"id\":\"strag\""), "\"id\":\"strag\"");
  Expect(range.find("\"data\":[1,0,7,6,0,0,8,1,5,0,0,2,1,2,0]") !=
             std::string::npos,
         "the range provider must return only the viewport's tokens");
  std::string const rangeId = ResultIdOf(range);
  Expect(!rangeId.empty(), "the range provider must carry a resultId");

  // A range resultId is never cached: sending it to full/delta must fall back
  // to a full token set, not diff against the viewport-scoped data.
  input->append(MakeLspFrame(SemDeltaFrame("stragd", rangeId)));
  std::string const fallback =
      TailAfter(WaitForOutputContaining(output, "\"id\":\"stragd\""),
                "\"id\":\"stragd\"");
  Expect(fallback.find("\"tokens\":[") != std::string::npos,
         "a delta against a range resultId must fall back to full tokens");
  Expect(fallback.find("\"edits\"") == std::string::npos,
         "a range resultId must never be diffed into an edit list");

  session.stop();
}

void TestInlayHintsReturned() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kInlayOpenFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "inlay-hint document open must publish diagnostics");

  input->append(MakeLspFrame(kInlayHintFrame));
  std::string const hints = TailAfter(
      WaitForOutputContaining(output, "\"id\":\"inh\""), "\"id\":\"inh\"");
  Expect(hints.find("\"label\":\"END SUB\"") != std::string::npos,
         "the SUB opener must offer its END SUB closer");
  Expect(hints.find("\"position\":{\"line\":0,\"character\":11}") !=
             std::string::npos,
         "the closer hint must anchor at the end of the opener's line");
  Expect(hints.find("\"label\":\"As String\"") != std::string::npos,
         "a suffix-typed dim must offer its inferred type");
  Expect(hints.find("\"position\":{\"line\":3,\"character\":6}") !=
             std::string::npos,
         "the inferred-type hint must anchor after the identifier");

  session.stop();
}
} // namespace

namespace fbtest {

void RunSemanticTokensTests() {
  RUN_TEST(TestSemanticTokensFullThenDelta);
  RUN_TEST(TestSemanticTokensCapabilities);
  RUN_TEST(TestSemanticTokensRangeAndFullOnlyCache);
  RUN_TEST(TestInlayHintsReturned);
}

} // namespace fbtest
