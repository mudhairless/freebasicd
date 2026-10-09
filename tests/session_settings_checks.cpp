/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// M11 settings: didChangeConfiguration and the feature gates.
//
// The notification that re-reads every root's freebasicd.toml, the two things
// it changes (include paths, diagnostics on/off), and the per-root application
// that keeps one root's config off another root's features.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {
// M11: a freebasicd.toml written after open that gains includePaths must
// make a previously-unreachable `#include` resolve on the *next*
// didChangeConfiguration — the root's open buffer is re-resolved (no
// include-not-found any more) and cross-file resolution reaches the header.
void TestDidChangeConfigurationIncludePathResolves() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const ws = sandbox / "ws";
  // The header sits at `ws/vendor/extra/` — reachable only via a configured
  // include path (an immediate root subdir search never descends into
  // vendor/extra).
  std::filesystem::create_directories(ws / "vendor" / "extra");
  {
    std::ofstream out(ws / "vendor" / "extra" / "exthdr.bi");
    out << "dim shared extVal as integer\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const mainUri = FileUri(ws / "main.bas");
  std::string const headerUri = FileUri(ws / "vendor" / "extra" / "exthdr.bi");
  std::string const rootUri = FileUri(ws);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  // No config yet: the include does not resolve, so opening publishes
  // include-not-found.
  std::string const mainText = "#include \"exthdr.bi\"\nprint extVal\n";
  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      mainUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString(mainText) + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));
  std::string const first = WaitForPublishedUri(output, 1);
  Expect(first.find("\"code\":\"include-not-found\"") != std::string::npos,
         "an unreachable include must publish include-not-found");

  // Write the config and signal the change; the notification carries no
  // settings of its own (the payload is ignored, the file is the truth).
  {
    std::ofstream out(ws / "freebasicd.toml");
    out << "includePaths = [\"vendor/extra\"]\n";
  }
  input->append(MakeLspFrame(kDidChangeConfigurationFrame));

  // The open buffer is re-resolved: the next (and last) publish for it no
  // longer reports the include as missing.
  std::string const republished =
      WaitForPublishedUri(output, CountPublished(output->snapshot()) + 1);
  std::string const lastPublish = LastPublish(republished);
  Expect(lastPublish.find("\"code\":\"include-not-found\"") ==
             std::string::npos,
         "the re-published diagnostics must no longer report the include as "
         "missing");
  Expect(lastPublish.find("\"diagnostics\":[]") != std::string::npos,
         "with the include resolved, the buffer must publish a clean list");

  // Cross-file proof: references on extVal in main.bas must reach the header
  // declaration in the (scan-indexed) closure.
  std::string const refs = PollRequest(
      input, output, "xic", "\"uri\":\"" + headerUri + "\"",
      [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/references","params":{"textDocument":{"uri":")" +
               mainUri +
               R"("},"position":{"line":1,"character":6},"context":{"includeDeclaration":true}}})";
      });
  Expect(refs.find("\"start\":{\"line\":0,\"character\":11}") !=
                 std::string::npos &&
             refs.find("\"end\":{\"line\":0,\"character\":17}") !=
                 std::string::npos,
         "references must reach the header declaration once the include "
         "resolves");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// M11: the diagnosticsOn flag gates publishing per root. Off publishes a
// single empty result per open buffer and then silence (an edit that would
// otherwise add diagnostics publishes nothing); on re-publishes the buffer's
// current errors.
void TestDidChangeConfigurationDiagnosticsToggle() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(sandbox);

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const mainUri = FileUri(sandbox / "main.bas");
  std::string const rootUri = FileUri(sandbox);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  // A bare `else` is a stray closer — a reliable Error to observe.
  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      mainUri + R"(","languageId":"basic","version":1,"text":"else\n"}}})";
  input->append(MakeLspFrame(openFrame.c_str()));
  std::string const first = WaitForPublishedUri(output, 1);
  Expect(first.find("\"code\":\"fbc error: 117\"") != std::string::npos,
         "the open must publish the stray-closer Error");

  // Off: one empty publish per open buffer...
  {
    std::ofstream out(sandbox / "freebasicd.toml");
    out << "diagnosticsOn = false\n";
  }
  input->append(MakeLspFrame(kDidChangeConfigurationFrame));
  std::string const cleared =
      WaitForPublishedUri(output, CountPublished(output->snapshot()) + 1);
  std::string const clearing = LastPublish(cleared);
  Expect(clearing.find("\"diagnostics\":[]") != std::string::npos,
         "turning diagnostics off must publish one empty result for the open "
         "buffer");
  Expect(clearing.find("\"code\"") == std::string::npos,
         "the clearing publish must carry no diagnostics");

  // ...then silence: an edit that now parses to another stray closer must
  // publish nothing.
  std::string const changeFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":)"
      R"({"uri":")" +
      mainUri +
      R"(","version":2},"contentChanges":[{"range":{"start":{"line":0,"character":0},)"
      R"("end":{"line":0,"character":4}},"text":"case 1\n"}]}})";
  input->append(MakeLspFrame(changeFrame.c_str()));
  // WaitForPublishedUri gives up after ~1s; the snapshot must still hold just
  // the two publishes from open + clearing.
  std::string const silenced =
      WaitForPublishedUri(output, CountPublished(output->snapshot()) + 1);
  Expect(CountPublished(silenced) == 2,
         "with diagnostics off, an edit must publish nothing");

  // On again: the root's open buffer is re-published with its current errors
  // (the buffer now holds the `case 1` stray closer).
  {
    std::ofstream out(sandbox / "freebasicd.toml");
    out << "diagnosticsOn = true\n";
  }
  input->append(MakeLspFrame(kDidChangeConfigurationFrame));
  std::string const restored =
      WaitForPublishedUri(output, CountPublished(output->snapshot()) + 1);
  std::string const restoring = LastPublish(restored);
  Expect(restoring.find("\"code\":\"fbc error: 118\"") != std::string::npos,
         "turning diagnostics on must re-publish the buffer's errors");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// M11: semanticTokensOn / inlayHintsOn gate their features per root while the
// capabilities stay advertised. A root that disables them serves empty results
// (full/delta/range all keep a fresh resultId; inlay an empty result), a root
// with the defaults serves the populated payloads.
void TestSemanticTokensAndInlayHintsGates() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const fa = sandbox / "fa";
  std::filesystem::path const fb = sandbox / "fb";
  std::filesystem::create_directories(fa);
  std::filesystem::create_directories(fb);
  {
    // fa disables both features; fb keeps the defaults.
    std::ofstream out(fa / "freebasicd.toml");
    out << "semanticTokensOn = false\ninlayHintsOn = false\n";
    std::ofstream aSem(fa / "a.bas");
    aSem << "dim counter as integer\ncounter = 1\n";
    std::ofstream aInlay(fa / "ai.bas");
    aInlay << "sub greet()\n    print 1\nend sub\ndim x$\n";
    std::ofstream out2(fb / "freebasicd.toml");
    out2 << "[server]\n";
    std::ofstream bSem(fb / "b.bas");
    bSem << "dim counter as integer\ncounter = 1\n";
    std::ofstream bInlay(fb / "bi.bas");
    bInlay << "sub greet()\n    print 1\nend sub\ndim x$\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const faUri = FileUri(fa);
  std::string const fbUri = FileUri(fb);
  std::string const aSemUri = FileUri(fa / "a.bas");
  std::string const aInlayUri = FileUri(fa / "ai.bas");
  std::string const bSemUri = FileUri(fb / "b.bas");
  std::string const bInlayUri = FileUri(fb / "bi.bas");
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"workspaceFolders":[)"
      R"({"uri":")" +
      faUri + R"(","name":"fa"},{"uri":")" + fbUri + R"(","name":"fb"}]}})";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  auto openFrame = [](std::string const &uri, std::string const &text) {
    return R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
           R"({"uri":")" +
           uri + R"(","languageId":"basic","version":1,"text":")" +
           ToJsonString(text) + "\"}}}";
  };
  input->append(MakeLspFrame(
      openFrame(aSemUri, "dim counter as integer\ncounter = 1\n").c_str()));
  input->append(MakeLspFrame(
      openFrame(aInlayUri, "sub greet()\n    print 1\nend sub\ndim x$\n")
          .c_str()));
  input->append(MakeLspFrame(
      openFrame(bSemUri, "dim counter as integer\ncounter = 1\n").c_str()));
  input->append(MakeLspFrame(
      openFrame(bInlayUri, "sub greet()\n    print 1\nend sub\ndim x$\n")
          .c_str()));
  WaitForPublishedUri(output, 4); // let all four opens settle

  auto semFull = [](std::string const &uri, std::string const &id) {
    return R"({"jsonrpc":"2.0","id":")" + id +
           R"(","method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":")" +
           uri + "\"}}}";
  };
  auto semDelta = [](std::string const &uri, std::string const &id,
                     std::string const &previous) {
    return R"({"jsonrpc":"2.0","id":")" + id +
           R"(","method":"textDocument/semanticTokens/full/delta","params":{"textDocument":{"uri":")" +
           uri + R"("},"previousResultId":")" + previous + "\"}}";
  };
  auto semRange = [](std::string const &uri, std::string const &id) {
    return R"({"jsonrpc":"2.0","id":")" + id +
           R"(","method":"textDocument/semanticTokens/range","params":{"textDocument":{"uri":")" +
           uri +
           R"("},"range":{"start":{"line":0,"character":0},"end":{"line":1,"character":15}}}})";
  };
  auto inlay = [](std::string const &uri, std::string const &id) {
    return R"({"jsonrpc":"2.0","id":")" + id +
           R"(","method":"textDocument/inlayHint","params":{"textDocument":{"uri":")" +
           uri +
           R"("},"range":{"start":{"line":0,"character":0},"end":{"line":3,"character":0}}}})";
  };

  // fa (off): full serves an empty data set with a fresh cached resultId.
  input->append(MakeLspFrame(semFull(aSemUri, "gfaFull").c_str()));
  std::string const gatedFull =
      TailAfter(WaitForOutputContaining(output, "\"id\":\"gfaFull\""),
                "\"id\":\"gfaFull\"");
  Expect(gatedFull.find("\"data\":[]") != std::string::npos,
         "a root with semanticTokensOn=false must serve empty full tokens");
  Expect(!ResultIdOf(gatedFull).empty(),
         "a gated full result must still carry a resultId");
  Expect(gatedFull.find("\"data\":[0,0,3") == std::string::npos,
         "a gated full result must not carry real tokens");

  // fa (off): delta serves a full-empty variant with a fresh resultId.
  input->append(MakeLspFrame(semDelta(aSemUri, "gfaDelta", "stGone").c_str()));
  std::string const gatedDelta =
      TailAfter(WaitForOutputContaining(output, "\"id\":\"gfaDelta\""),
                "\"id\":\"gfaDelta\"");
  Expect(gatedDelta.find("\"tokens\":[]") != std::string::npos,
         "a gated delta must serve a full-empty token variant");
  Expect(gatedDelta.find("\"edits\"") == std::string::npos,
         "a gated delta must not diff anything");
  Expect(!ResultIdOf(gatedDelta).empty(),
         "a gated delta must still carry a fresh resultId");

  // fa (off): range serves empty data with a fresh uncached resultId.
  input->append(MakeLspFrame(semRange(aSemUri, "gfaRange").c_str()));
  std::string const gatedRange =
      TailAfter(WaitForOutputContaining(output, "\"id\":\"gfaRange\""),
                "\"id\":\"gfaRange\"");
  Expect(gatedRange.find("\"data\":[]") != std::string::npos,
         "a gated range result must serve empty data");
  Expect(!ResultIdOf(gatedRange).empty(),
         "a gated range result must still carry a resultId");

  // fa (off): inlay serves an empty hint list.
  input->append(MakeLspFrame(inlay(aInlayUri, "gfaInlay").c_str()));
  std::string const gatedInlay =
      TailAfter(WaitForOutputContaining(output, "\"id\":\"gfaInlay\""),
                "\"id\":\"gfaInlay\"");
  Expect(gatedInlay.find("\"result\":[]") != std::string::npos,
         "a root with inlayHintsOn=false must serve an empty hint list");

  // fb (defaults): the same documents on a default root serve the populated
  // payloads the existing feature tests assert.
  input->append(MakeLspFrame(semFull(bSemUri, "gfbFull").c_str()));
  std::string const fullTokens =
      TailAfter(WaitForOutputContaining(output, "\"id\":\"gfbFull\""),
                "\"id\":\"gfbFull\"");
  Expect(fullTokens.find("\"data\":[0,0,3,0,0,0,4,7") != std::string::npos,
         "a default root must serve the full token data");
  Expect(fullTokens.find("\"data\":[]") == std::string::npos,
         "a default root must not serve an empty token set");

  input->append(MakeLspFrame(inlay(bInlayUri, "gfbInlay").c_str()));
  std::string const hints =
      TailAfter(WaitForOutputContaining(output, "\"id\":\"gfbInlay\""),
                "\"id\":\"gfbInlay\"");
  Expect(hints.find("\"label\":\"END SUB\"") != std::string::npos &&
             hints.find("\"label\":\"As String\"") != std::string::npos,
         "a default root must serve the inlay hints");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// M11: settings apply only to their own root. A didChangeConfiguration new
// need is one root re-reads its config; a sibling root whose config did not
// change is untouched — its include stays unresolved and nothing is
// re-published for its open buffers.
void TestSettingsApplyPerRootOnly() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const fa = sandbox / "fa";
  std::filesystem::path const fb = sandbox / "fb";
  std::filesystem::create_directories(fa / "vendor" / "extra");
  std::filesystem::create_directories(fb / "vendor" / "extra");
  {
    std::ofstream out(fa / "freebasicd.toml");
    out << "[server]\n";
    std::ofstream faHdr(fa / "vendor" / "extra" / "inner.bi");
    faHdr << "dim shared aInner as integer\n";
    std::ofstream faSrc(fa / "a.bas");
    faSrc << "#include \"inner.bi\"\nelse\n";
    std::ofstream out2(fb / "freebasicd.toml");
    out2 << "[server]\n";
    std::ofstream fbHdr(fb / "vendor" / "extra" / "inner.bi");
    fbHdr << "dim shared bInner as integer\n";
    std::ofstream fbSrc(fb / "b.bas");
    fbSrc << "#include \"inner.bi\"\nelse\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const faUri = FileUri(fa);
  std::string const fbUri = FileUri(fb);
  std::string const aUri = FileUri(fa / "a.bas");
  std::string const bUri = FileUri(fb / "b.bas");
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"workspaceFolders":[)"
      R"({"uri":")" +
      faUri + R"(","name":"fa"},{"uri":")" + fbUri + R"(","name":"fb"}]}})";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  std::string const text = "#include \"inner.bi\"\nelse\n";
  std::string const openA =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      aUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString(text) + "\"}}}";
  std::string const openB =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      bUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString(text) + "\"}}}";
  input->append(MakeLspFrame(openA.c_str()));
  input->append(MakeLspFrame(openB.c_str()));
  std::string const both = WaitForPublishedUri(output, 2);
  Expect(CountPublished(both) == 2, "both opens must publish diagnostics");
  std::size_t notFoundA = 0;
  std::size_t ra = 0;
  while ((ra = both.find("\"code\":\"include-not-found\"", ra)) !=
         std::string::npos) {
    ++notFoundA;
    ra += 1;
  }
  Expect(notFoundA == 2,
         "both roots' opens must report their unreachable include");

  // Only fa's config changes: an include path that resolves fa's header. fb's
  // file stays byte-identical, so fb's settings are unchanged and the
  // notification must leave fb's buffers alone.
  {
    std::ofstream out(fa / "freebasicd.toml");
    out << "includePaths = [\"vendor/extra\"]\n";
  }
  input->append(MakeLspFrame(kDidChangeConfigurationFrame));
  std::string const after =
      WaitForPublishedUri(output, CountPublished(output->snapshot()) + 1);

  // Exactly one more publish (fa's re-resolved buffer): fb published nothing.
  Expect(CountPublished(after) == 3,
         "only the changed root's open buffer may be re-published");
  std::string const last = LastPublish(after);
  Expect(last.find(aUri) != std::string::npos,
         "the re-published buffer must be the changed root's document");
  Expect(last.find(bUri) == std::string::npos,
         "the unchanged root must not re-publish its document");
  Expect(last.find("\"code\":\"include-not-found\"") == std::string::npos,
         "the changed root's include must now resolve");
  Expect(last.find("\"code\":\"fbc error: 117\"") != std::string::npos,
         "the changed root's other errors must survive the re-publish");
  std::size_t remaining = 0;
  std::size_t rb = 0;
  while ((rb = after.find("\"code\":\"include-not-found\"", rb)) !=
         std::string::npos) {
    ++remaining;
    rb += 1;
  }
  Expect(remaining == 2,
         "the unchanged root's include-not-found must remain published");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}
} // namespace

namespace fbtest {

void RunSettingsTests() {
  RUN_TEST(TestDidChangeConfigurationIncludePathResolves);
  RUN_TEST(TestDidChangeConfigurationDiagnosticsToggle);
  RUN_TEST(TestSemanticTokensAndInlayHintsGates);
  RUN_TEST(TestSettingsApplyPerRootOnly);
}

} // namespace fbtest
