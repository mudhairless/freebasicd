/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The extern-placement lint over the wire.
//
// An `Extern` declaration — the multi-module variable form (`extern x as
// integer`) or the C-library block (`Extern "C" ... End Extern`) — belongs in
// a shared header (.bi), not a source module. A `.bas` document carries a
// Warning (code `extern-in-source`, no fbc number: fbc compiles it fine, so
// this is a convention lint, not a parity error) at the keyword's own range;
// a `.bi` — the place externs belong — stays silent. The gating lives here,
// in the session, because only the session knows the path; the scan itself is
// a pure resolve-layer pass (resolve_checks covers it directly).
//
// Both deliveries must agree ("one payload, two deliveries"): one test reads
// the pull report the document handler builds, one reads the push publish the
// reparse sends.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {

// The same `extern` text a source module and a shared header might both
// carry: the variable form, the C-library block, and its closer.
char const kExternText[] =
    "extern ext1 as integer\n"
    "extern \"C\"\n"
    "    declare sub c_func cdecl alias \"c_func\" () as long\n"
    "end extern\n";

std::string OpenFrame(std::string const &uri, std::string const &text) {
  return std::string(
             R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
             R"({"uri":")") +
         uri + R"(","languageId":"basic","version":1,"text":")" +
         ToJsonString(text) + "\"}}}";
}

// The source module: both extern forms pull one Warning each, on the
// `extern` keyword's own range, and the document pull is where the payload
// documentDiagnostics builds is read.
void TestPullExternPlacementInSource() {
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

  std::string const uri = Expand("file://{{tmp}}/extmod.bas");
  input->append(MakeLspFrame(OpenFrame(uri, kExternText)));

  std::string const report = PollRequest(
      input, output, "exts", "\"kind\":\"full\"", [&](std::string const &id) {
        return std::string(
                   R"({"jsonrpc":"2.0","id":")" + id +
                   R"(","method":"textDocument/diagnostic","params":{"textDocument":{"uri":")") +
               uri + "\"}}}";
      });
  Expect(CountOf(report, "\"code\":\"extern-in-source\"") == 2,
         "the variable form and the block form each pull one lint item");
  Expect(report.find("\"start\":{\"line\":0,\"character\":0},\"end\":"
                     "{\"line\":0,\"character\":6}") != std::string::npos,
         "the variable-form item covers the extern keyword");
  Expect(report.find("\"start\":{\"line\":1,\"character\":0},\"end\":"
                     "{\"line\":1,\"character\":6}") != std::string::npos,
         "the block-form item covers the extern keyword");
  Expect(report.find("\"severity\":2") != std::string::npos,
         "the items are Warnings (severity 2)");
  Expect(!ResultIdOf(report).empty(), "the report must carry a resultId");

  session.stop();
}

// The same bytes in a `.bi`: the header is where externs belong, so the lint
// must not fire — even though the report is a real one (the payload builder
// ran; it just had nothing to add).
void TestPullExternPlacementStaysOutOfHeaders() {
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

  std::string const uri = Expand("file://{{tmp}}/exthead.bi");
  input->append(MakeLspFrame(OpenFrame(uri, kExternText)));

  std::string const report = PollRequest(
      input, output, "exth", "\"kind\":\"full\"", [&](std::string const &id) {
        return std::string(
                   R"({"jsonrpc":"2.0","id":")" + id +
                   R"(","method":"textDocument/diagnostic","params":{"textDocument":{"uri":")") +
               uri + "\"}}}";
      });
  Expect(report.find("extern-in-source") == std::string::npos,
         "a header is where externs belong: no lint item");
  Expect(report.find("\"kind\":\"full\"") != std::string::npos,
         "the header was still answered with a real full report");
  Expect(!ResultIdOf(report).empty(), "the report must carry a resultId");

  session.stop();
}

// The push delivery, for a client that never negotiated pull: the header's
// publish carries no lint, and the source module's publish — the newest one —
// carries one item per extern form. The ordering is what scopes the negative
// assertion: the header is opened first, so by the time the module's publish
// lands the header's (empty) one is in the stream.
void TestPushPublishExternPlacement() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kInitializeFrame));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"diagnosticProvider\"") == std::string::npos,
         "a legacy client keeps the push path");

  std::string const headUri = Expand("file://{{tmp}}/exthead.bi");
  input->append(MakeLspFrame(OpenFrame(headUri, kExternText)));
  std::string const afterHead = WaitForPublishedUri(output, 1);
  Expect(LastPublish(afterHead).find("extern-in-source") == std::string::npos,
         "the header's publish must not carry the lint");

  std::string const modUri = Expand("file://{{tmp}}/extmod.bas");
  input->append(MakeLspFrame(OpenFrame(modUri, kExternText)));
  std::string const afterMod = WaitForPublishedUri(output, 2);
  std::string const modPublish = LastPublish(afterMod);
  Expect(CountOf(modPublish, "\"code\":\"extern-in-source\"") == 2,
         "the module's publish carries one lint item per extern form");
  Expect(modPublish.find(modUri) != std::string::npos,
         "the newest publish is the source module's own");

  session.stop();
}

} // namespace

namespace fbtest {

void RunExternPlacementTests() {
  RUN_TEST(TestPullExternPlacementInSource);
  RUN_TEST(TestPullExternPlacementStaysOutOfHeaders);
  RUN_TEST(TestPushPublishExternPlacement);
}

} // namespace fbtest