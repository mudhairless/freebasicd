/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

// ---------------------------------------------------------------------------
// The session integration harness
// ---------------------------------------------------------------------------
//
// The suite is one executable (ctest still calls it `session_integration`)
// built from this header, its definitions in session_support.cpp, and one file
// per LSP feature: session_hover_checks.cpp,
// session_pull_diagnostics_checks.cpp and so on. session_integration.cpp holds
// only main(), which calls each file's Run<Feature>Tests() in a fixed order.
//
// The split is by the feature under test, which is also how the suite is read
// and how a failure is triaged: the `[ DONE ]` marker names the test, the test
// lives in the file for its feature, and nothing else in the tree is in play.
// The alternative — one file per milestone — put unrelated tests together and
// left a single 6000-line translation unit that no review could hold.
//
// What is here is everything a test needs that is not the test itself: the
// in-memory session plumbing, the wire frames more than one file sends, the
// fixtures that build a sandbox, and the pollers that wait for an answer. Two
// rules keep the split from eroding:
//
// - A frame or helper only one file's tests send stays in that file, declared
//   just above them. What is here is either shared by more than one file or is
//   machinery every test needs (the reporters, RUN_TEST, the fixtures).
// - Adding a test touches one file: the function, and one RUN_TEST line in that
//   file's runner. Nothing in this header changes, and neither does main().
//
// One executable, not one per feature file, for two reasons. The suite's
// diagnostics are a property of the process — `[ RUN ]` / `[ DONE ]` and the
// final line are what turn a crash or a hang into a named test (see
// "Naming a failure that has no message" in session_support.cpp) — and ctest
// prints one captured stream per test, which is that much easier to read than
// fifteen interleaved ones. The trade is a link step that has to notice a new
// file; CMakeLists.txt lists them, and a missing one is a link error, not a
// silently skipped test.
//
// ScopedEnv and TwoFileFixture are the two types defined here rather than in
// the .cpp: a test constructs them, so they are complete types in the header.

#include "LibLsp/LspCpp.h"
#include "src/session.h"
#include "test_helpers.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fbtest {

using test::Expect;
using test::FeedableIStream;
using test::StringOStream;
using test::WaitForOutputContaining;

// --- the reporters ----------------------------------------------------------
// Definitions, and the reasoning behind every one of them, in
// session_support.cpp under "Naming a failure that has no message".
void PrintDiagnostic(std::string const &line);
void ReportEscapedTest(std::string const &name, char const *what);
void ReportUncaught();

// --- RUN_TEST ---------------------------------------------------------------
// Run one test unless --filter= / LSPCPP_TEST_FILTER excludes it, bracketing it
// with the [ RUN ] / [ DONE ] markers the suite's diagnostics are read from,
// and catching an exception so one bad test cannot take the suite with it.
//
// RUN_TEST is silent when a test passes, so a hang or a crash on a platform we
// cannot reproduce locally leaves ctest's captured output with nothing in it —
// the Windows leg once sat on session_integration for eleven minutes and the
// log said only that test 14 had started. Print the name first and flush it:
// ctest prints what a test emitted when it fails *or times out*, so the last
// line printed is then the answer.
//
// The closing `[ DONE ]` and the catch are the other half of that: a run that
// stops after a test's last `[ RUN ]` cannot say whether the test finished or
// the process died inside it, and a test that throws would take the remaining
// seventy with it. See "Naming a failure that has no message" in
// session_support.cpp.
//
// Redefined rather than added: LspCpp's test_helpers.h defines a RUN_TEST of
// its own that brackets nothing and lets an exception escape, and it is
// vendored, so the #undef is not optional.
#undef RUN_TEST
#define RUN_TEST(fn)                                                           \
  do {                                                                         \
    if (test::ShouldRunTest(#fn)) {                                            \
      std::printf("[ RUN      ] %s\n", #fn);                                   \
      std::fflush(stdout);                                                     \
      try {                                                                    \
        (fn)();                                                                \
      } catch (std::exception const &e) {                                      \
        ReportEscapedTest(#fn, e.what());                                      \
      } catch (...) {                                                          \
        ReportEscapedTest(#fn, "non-std exception");                           \
      }                                                                        \
      std::printf("[ DONE  ] %s\n", #fn);                                      \
      std::fflush(stdout);                                                     \
    } else {                                                                   \
      ++test::SkippedTests();                                                  \
    }                                                                          \
  } while (0)

// --- document URIs and the frames built on them -----------------------------
// No test builds a `file://` URI by gluing a scheme onto path.string(): on
// Windows that puts backslashes in a JSON string, where a backslash is an
// escape, and the message never parses. The reasoning is in the .cpp; the rule
// is that both halves go through FileUri.
std::string FileUri(std::filesystem::path const &path);
std::string const &tmpUriPath();

// Substitute the per-platform {{tmp}} placeholder. A frame that hardcodes
// `file:///tmp/x.bas` decodes to a drive-less path on Windows, so no frame in
// this suite writes one out by hand.
std::string Expand(std::string body);
std::string MakeLspFrame(std::string const &body);

// Escape a value for a JSON string literal.
std::string ToJsonString(std::string const &s);

// The reply's own text, so an assertion cannot match a diagnostic publish that
// happened to mention the same document. See "A poll's needle must be scoped
// to the reply it returns" in AGENTS.md.
std::string TailAfter(std::string const &snapshot,
                      std::string const &idLiteral);

// The `resultId` out of a textDocument/diagnostic response, for the
// `unchanged` round trip (M14).
std::string ResultIdOf(std::string const &response);

// Send a request and wait for a reply carrying `needle`, resending on each
// budget until it appears: the index behind a cross-file resolve settles
// asynchronously, so the first answer can be the wrong one. `needle` is
// matched against the *reply's tail*, never the whole stream.
std::string
PollRequest(std::shared_ptr<FeedableIStream> const &input,
            std::shared_ptr<StringOStream> const &output,
            std::string const &prefix, std::string const &needle,
            std::function<std::string(std::string const &)> const &frame);

// --- ScopedEnv --------------------------------------------------------------
// Set an environment variable for the length of a test and put it back, so a
// test can steer the server's home-folder guard. homeDirectory() reads HOME
// before USERPROFILE, so one variable covers both platforms; MSVC has no
// setenv, and its _putenv_s removes the variable when given an empty value,
// which is how the destructor undoes one that was not set to begin with.
class ScopedEnv {
public:
  ScopedEnv(char const *name, std::string const &value) : name_(name) {
    if (char const *const old = std::getenv(name); old != nullptr) {
      hadOld_ = true;
      old_ = old;
    }
    assign(value);
  }
  ~ScopedEnv() { assign(hadOld_ ? old_ : std::string()); }

  ScopedEnv(ScopedEnv const &) = delete;
  ScopedEnv &operator=(ScopedEnv const &) = delete;

private:
  void assign(std::string const &value) {
#ifdef _WIN32
    (void)_putenv_s(name_.c_str(), value.c_str());
#else
    if (value.empty()) {
      (void)::unsetenv(name_.c_str());
    } else {
      (void)::setenv(name_.c_str(), value.c_str(), 1);
    }
#endif
  }
  std::string name_;
  std::string old_;
  bool hadOld_ = false;
};

// --- the sandbox ------------------------------------------------------------
// A workspace under the platform temp root with six fixed documents, each with
// its URI built by FileUri. Every path a test uses is a member, so a test
// never spells a file out itself.
extern char const kLibContent[];
extern char const kMainContent[];
extern char const kProgContent[];
extern char const kExtraContent[];
extern char const kCalleeContent[];
extern char const kCallerContent[];

struct TwoFileFixture {
  std::filesystem::path sandbox;
  std::filesystem::path wsDir;
  std::string libUri;
  std::string mainUri;
  std::string progUri;
  std::string extraUri;
  std::string callerUri;
  std::string calleeUri;
  std::string rootUri;

  TwoFileFixture() {
    static std::atomic<long> counter{0};
    sandbox = std::filesystem::temp_directory_path() /
              ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
               std::to_string(counter.fetch_add(1)));
    wsDir = sandbox / "ws";
    std::filesystem::create_directories(wsDir);
    {
      std::ofstream out(wsDir / "lib.bi");
      out << kLibContent;
    }
    {
      std::ofstream out(wsDir / "main.bas");
      out << kMainContent;
    }
    {
      std::ofstream out(wsDir / "prog.bas");
      out << kProgContent;
    }
    {
      std::ofstream out(wsDir / "extra.bi");
      out << kExtraContent;
    }
    {
      std::ofstream out(wsDir / "callee.bi");
      out << kCalleeContent;
    }
    {
      std::ofstream out(wsDir / "caller.bas");
      out << kCallerContent;
    }
    libUri = FileUri(wsDir / "lib.bi");
    mainUri = FileUri(wsDir / "main.bas");
    progUri = FileUri(wsDir / "prog.bas");
    extraUri = FileUri(wsDir / "extra.bi");
    callerUri = FileUri(wsDir / "caller.bas");
    calleeUri = FileUri(wsDir / "callee.bi");
    rootUri = FileUri(wsDir);
  }

  ~TwoFileFixture() {
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
  }
};

// A started session, its server, and a workspace root already registered, with
// `opens` handed to didOpen in order. This is the whole handshake every test
// that needs an index would otherwise repeat.
std::shared_ptr<FeedableIStream> StartIndexedSession(
    lsp::LanguageSession &session, FreeBasicServer &server,
    std::shared_ptr<StringOStream> const &output, TwoFileFixture const &fix,
    std::vector<std::pair<std::string, std::string>> const &opens);

// --- waiters ----------------------------------------------------------------
// Each waits for a *count*, not a predicate on a whole stream, so a document
// published by an earlier test in the same run cannot satisfy the wait early.

// textDocument/semanticTokens/full-delta, the one request that carries the
// previous result id (M9).
std::string SemDeltaFrame(std::string const &id, std::string const &previous);

// `count` publishes from now, and the URI of the last one. A publish is what
// a didOpen or a didChange produces; a session that publishes nothing is the
// shape a deadlock takes when a test waits on an answer that will not come.
std::string WaitForPublishedUri(std::shared_ptr<StringOStream> const &output,
                                size_t count);
std::size_t CountPublished(std::string const &snapshot);
std::string LastPublish(std::string const &snapshot);

// workspace/diagnostic/refresh, the server hint a pull client is re-pulled by
// (M14). Counted the same way for the same reason.
std::size_t CountRefreshes(std::string const &snapshot);
std::string WaitForRefreshes(std::shared_ptr<StringOStream> const &output,
                             size_t count);

// --- the frames more than one feature file sends ---------------------------
// A frame only one file's tests send is a local constant in that file, next to
// them. This list is what is left after that rule is applied.

// The document every test that needs a plain single-file workspace opens.
extern char const *kUri;

extern char const kInitializeFrame[];
extern char const kDidOpenFrame[];
extern char const kDidChangeFrame[];
extern char const kDidOpenDupFrame[];
extern char const kDidOpenHierFrame[];
extern char const kDidOpenResolveFrame[];
extern char const kDidOpenCallsFrame[];
extern char const kDidOpenIntrinsicFrame[];

// workspace/didChangeConfiguration. The payload is deliberately ignored by the
// server (the config file is the truth, see src/settings.cpp), so this is the
// notification with an empty `settings` object.
extern char const *kDidChangeConfigurationFrame;
extern char const *kShutdownFrame;
extern char const *kExitFrame;

// A client that advertises textDocument.diagnostic, so the session negotiates
// pull diagnostics and never publishes (M14).
extern char const kInitializePullFrame[];

// --- wire-offset helpers ----------------------------------------------------
// The byte offsets a test reasons about are the server's, and a reply carries
// them as UTF-16 line/character pairs. These convert between the two, and
// build the range fragments the call-hierarchy and code-lens requests need in
// their params.

std::size_t CountOf(std::string const &text, std::string const &needle);

std::string WirePosition(std::string const &src, std::size_t off);
std::string WireRange(std::string const &src, std::size_t beg, std::size_t end);

// Position/range of the first `needle` at or after `from`. The `from` is what
// makes a second call find the *second* occurrence rather than the first.
std::string WirePositionOf(std::string const &src, std::string const &needle,
                           std::size_t from = 0);
std::string WireRangeOf(std::string const &src, std::string const &needle,
                        std::size_t from = 0);
std::string WireRangeBetween(std::string const &src,
                             std::string const &begNeedle,
                             std::string const &endNeedle);

// The text of the line holding `needle`, quoted and escaped for a params
// object (signature help, which takes a `label` rather than a position).
std::string SignatureLine(std::string const &src, std::string const &needle);

// One textDocument/incomingCalls item, for a call-hierarchy test that answers
// a forward call it did not itself drive.
std::string CallItemJson(std::string const &uri, std::string const &name,
                         std::string const &range,
                         std::string const &selectionRange);

} // namespace fbtest
