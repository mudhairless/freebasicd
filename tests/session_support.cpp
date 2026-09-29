/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The harness's definitions; session_support.h declares them and says why the
// suite is split into one file per feature. Every body below moved here
// unchanged from the single-file driver, comments and all, so the section
// headers and the reasoning attached to each helper read the way they did.
//
// The two exceptions are ScopedEnv and TwoFileFixture: a test constructs them,
// so they are complete types in the header and have no definition here.

#include "session_support.h"

namespace fbtest {

// ---------------------------------------------------------------------------
// Naming a failure that has no message
// ---------------------------------------------------------------------------
//
// ctest prints what a test emitted when it fails *or* times out, so the last
// line printed is the answer. That works for an assertion, and for nothing
// else: a process that dies prints no line at all, and on MSVC the ways a
// process can die are exactly the quiet ones — the default terminate handler
// says nothing, an unhandled exception says nothing, and a hardware fault says
// nothing. The Windows leg hit exactly that: 70 tests started, not one
// assertion failed, and the log said only that test 14 had started, because
// the last `[ RUN ]` was the last line the process managed to write.
//
// So the boundaries and the reason are printed explicitly:
//
// - `[ RUN ]` / `[ DONE ]` bracket every test, and a final line after the
// last one says whether main reached its return. That splits "died inside
// a test" from "died on the way out" without knowing which in advance.
// - An exception escaping a test is caught, attributed to that test, and
// counted as a failure, so one bad test cannot hide the rest of the suite.
// - `std::set_terminate` covers what escapes the tests (a destructor,
// static teardown, a noexcept violation, a joinable `std::thread`) and
// says whether an exception was even active, because a bare terminate
// with no active exception is itself the diagnosis.
//
// None of this changes what a passing run prints except the `[ DONE ]` lines,
// and none of it can turn a crash into a pass: every reporter either counts a
// failure or aborts.
//
// A Windows unhandled-exception filter was here too, and is deliberately not:
// it printed a faulting address from a Release runner, where nothing can
// resolve one, and it dragged <Windows.h> into a cross-platform test file for
// that. The markers above already answer the question that address would have
// been asked for — *which* test, or after the suite — and a hardware fault
// needs a local repro whatever it prints. Add it back only if a crash localizes
// to a test and `[ DONE ]` is not enough to act on.
void PrintDiagnostic(std::string const &line) {
  std::fputs(line.c_str(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

void ReportEscapedTest(std::string const &name, char const *what) {
  ++test::Failures();
  PrintDiagnostic("[ THROW ] " + name + ": uncaught exception: " + what);
}

void ReportUncaught() {
  std::string what = "no active exception (a joinable std::thread, or a "
                     "noexcept violation)";
  if (std::exception_ptr const active = std::current_exception()) {
    try {
      std::rethrow_exception(active);
    } catch (std::exception const &e) {
      what = e.what();
    } catch (...) {
      what = "non-std exception";
    }
  }
  PrintDiagnostic("[ THROW ] outside any test: " + what);
  std::abort();
}

// ---------------------------------------------------------------------------
// Document URIs
// ---------------------------------------------------------------------------
//
// Every document in this suite is named by a `file://` URI, and no test builds
// one by gluing "file://" onto path.string(): that is a native path with a
// scheme stapled on, which is not a URI and is not even valid inside the JSON
// frame that carries it. On Windows path.string() has backslashes, and in a
// JSON string a backslash is an escape — "\t" and "\f" are legal and silently
// corrupt the path, and "\w" (from the "\ws" directory the sandboxes use) is
// not a legal escape at all, so the whole message fails to parse, the server
// never answers, and the test fails on a poll budget rather than on its
// assertion. That is what kept every sandbox-backed test red on the Windows
// leg.
//
// So both halves go through one encoder: LspCpp's, over fblang::normalizePath
// output, which is what the server echoes back for the same file. That fixes
// the separators, puts the third slash before a drive letter, percent-encodes
// what is unsafe, and matches the server's own lower-cased Windows spelling
// byte for byte.
std::string FileUri(std::filesystem::path const &path) {
  return make_file_scheme_uri(fblang::normalizePath(path));
}

// The suite's documents all live under a directory of their own, named here
// and never created: no test writes a file at any of these URIs, they all hand
// their text to didOpen, so the directory is a workspace root and nothing else.
// Keeping it out of the platform temp root itself is the point — the scan
// behind that root must not walk the sandboxes the other tests write there, or
// the suite indexes its own leftovers and one test's answer depends on which
// tests ran before it.
std::string const &tmpUriPath() {
  static std::string const base = [] {
    // `FileUri` returns a whole file: URI, so drop the scheme the literals
    // already carry and leave the placeholder holding the path part alone.
    std::string const full =
        FileUri(std::filesystem::temp_directory_path() / "fblsp-docs");
    return full.substr(std::string_view("file://").size());
  }();
  return base;
}

// Expands the {{tmp}} placeholder the URIs in this file are written against, so
// a literal can read as the URI it is (`file://{{tmp}}/hello.bas`) and still
// name a drive on Windows.
std::string Expand(std::string body) {
  static constexpr std::string_view kToken = "{{tmp}}";
  std::string const &base = tmpUriPath();
  for (size_t at = 0;;) {
    size_t const hit = body.find(kToken, at);
    if (hit == std::string::npos) {
      return body;
    }
    body.replace(hit, kToken.size(), base);
    at = hit + base.size();
  }
}

// Shadows test::MakeLspFrame so every frame in the suite is expanded, which
// keeps the placeholder out of all ~200 call sites instead of threading it
// through each one.
std::string MakeLspFrame(std::string const &body) {
  return test::MakeLspFrame(Expand(body));
}

// Escapes FreeBASIC source so it is JSON-safe inside an LSP frame.
std::string ToJsonString(std::string const &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out += c;
      break;
    }
  }
  return out;
}

// Response tail from the last occurrence of an id literal, so negative
// assertions are not tripped by earlier replies of the same session.
std::string TailAfter(std::string const &snapshot,
                      std::string const &idLiteral) {
  std::size_t const pos = snapshot.rfind(idLiteral);
  return pos == std::string::npos ? std::string() : snapshot.substr(pos);
}

// First `"resultId":"..."` value in a response.
std::string ResultIdOf(std::string const &response) {
  std::string const key = "\"resultId\":\"";
  std::size_t const beg = response.find(key);
  if (beg == std::string::npos) {
    return {};
  }
  std::size_t const start = beg + key.size();
  std::size_t const end = response.find('"', start);
  return response.substr(start, end - start);
}

// Append the given request repeatedly (each attempt gets a fresh numeric id)
// and return the first reply that contains `needle` — used to wait for the
// asynchronous workspace scan to settle the index behind a cross-file resolve.
//
// The needle is matched against the *reply*, not against the output stream, and
// that is the whole contract. The stream is cumulative: a needle naming a
// document (`fix.libUri`) is already in it by the time the first request is
// answered, because every didOpen publishes diagnostics for that document. So a
// stream-scoped match returns the first reply whatever it says — the poll
// silently stops waiting, and a caller asserting on the reply then fails on a
// premature answer instead of waiting for the index to settle. That is not a
// hypothetical: with the match scoped to the stream, a probe whose reply can
// never name the header returns `"result":null` on attempt 0 while the header
// URI sits in an earlier publish; scoped to the reply, the same probe retries
// until its budget runs out, which is the behaviour the callers assume.
//
// A poll that does spend its budget says so, because from the caller's side a
// give-up and a wrong answer are the same failed assertion.
std::string
PollRequest(std::shared_ptr<FeedableIStream> const &input,
            std::shared_ptr<StringOStream> const &output,
            std::string const &prefix, std::string const &needle,
            std::function<std::string(std::string const &)> const &frame) {
  // 40 attempts x 50 ms is 2 s per poll, which is ~20x what a settled index
  // needs and still well inside ctest's per-test budget. No caller tunes it.
  int constexpr attempts = 40;
  std::string last;
  for (int n = 0; n < attempts; ++n) {
    std::string const id = "\"id\":\"" + prefix + std::to_string(n) + "\"";
    input->append(MakeLspFrame(frame(prefix + std::to_string(n)).c_str()));
    std::string const snapshot = WaitForOutputContaining(output, id, 50);
    // Clip to this reply so callers can make negative assertions without
    // seeing earlier replies of the same session. rfind can miss when the wait
    // itself expired; an absent reply is a give-up, not a match, and the
    // newest reply we did see stays the answer to return.
    std::size_t const at = snapshot.rfind(id);
    if (at == std::string::npos) {
      continue;
    }
    last = snapshot.substr(at);
    if (last.find(needle) != std::string::npos) {
      return last;
    }
  }
  std::fprintf(stderr,
               "[ poll  ] %s: no reply matched \"%s\" in %d attempts; newest "
               "reply was %.200s\n",
               prefix.c_str(), needle.c_str(), attempts, last.c_str());
  return last;
}

// M7 two-file fixture: a header declaring a shared module var, a plain module
// var, and a proc that reads the shared one; a client .bas that includes it and
// uses the shared var at module level and inside a proc (where the plain var
// must stay invisible); and a completion probe projecting the plain var's name
// under the module-level and in-block prefixes.
char const kLibContent[] = "dim shared globalCount as integer\n"
                           "dim localOnly as integer\n"
                           "sub libProc()\n"
                           "    print globalCount\n"
                           "end sub\n";
char const kMainContent[] = "#include \"lib.bi\"\n"
                            "dim head as integer\n"
                            "head = globalCount + localOnly + earlyBird\n"
                            "sub mainProc()\n"
                            "    globalCount = globalCount + 1\n"
                            "    localOnly = 5\n"
                            "end sub\n";
char const kProgContent[] = "#include \"lib.bi\"\n"
                            "dim counter\n"
                            "sub prog()\n"
                            "    dim counter\n"
                            "end sub\n";
char const kExtraContent[] = "dim shared earlyBird as integer\n";

// M13 call hierarchy: a header declaring the only callee, and a client file
// that calls it twice — once with a parameter list, once bare, which fbc
// accepts in `fb` mode — and calls itself. Both directions of the hierarchy
// therefore cross a file boundary, and recursion is on the wire.
char const kCalleeContent[] = "sub hubProc(n as integer)\n"
                              "end sub\n";
char const kCallerContent[] = "#include \"callee.bi\"\n"
                              "sub driveProc()\n"
                              "    hubProc(1)\n"
                              "    hubProc 2\n"
                              "    driveProc\n"
                              "end sub\n";

// Start a session rooted at the fixture workspace, index it, and didOpen every
// fixture source from its buffer (open buffers, not disk, are the live truth
// the cross-file handlers serve).
std::shared_ptr<FeedableIStream> StartIndexedSession(
    lsp::LanguageSession &session, FreeBasicServer &server,
    std::shared_ptr<StringOStream> const &output, TwoFileFixture const &fix,
    std::vector<std::pair<std::string, std::string>> const &opens) {
  auto input = std::make_shared<FeedableIStream>();
  server.registerHandlers();
  session.start(input, output);

  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      fix.rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  for (auto const &open : opens) {
    std::string const frame =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
        R"({"uri":")" +
        open.first + R"(","languageId":"basic","version":1,"text":")" +
        ToJsonString(open.second) + "\"}}}";
    input->append(MakeLspFrame(frame.c_str()));
  }
  return input;
}

std::string SemDeltaFrame(std::string const &id, std::string const &previous) {
  return "{\"jsonrpc\":\"2.0\",\"id\":\"" + id +
         "\",\"method\":\"textDocument/semanticTokens/full/delta\",\"params\":"
         "{\"textDocument\":{\"uri\":\"file://{{tmp}}/fblsp-sem.bas\"},"
         "\"previousResultId\":\"" +
         previous + "\"}}";
}

std::string WaitForPublishedUri(std::shared_ptr<StringOStream> const &output,
                                size_t count) {
  std::string cur;
  for (int i = 0; i < 100; ++i) {
    cur = output->snapshot();
    size_t found = 0;
    size_t pos = 0;
    while ((pos = cur.find("\"method\":\"textDocument/publishDiagnostics\"",
                           pos)) != std::string::npos) {
      ++found;
      pos += 1;
    }
    if (found >= count) {
      return cur;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return cur;
}

// Count publishDiagnostics notifications present in a stream snapshot.
std::size_t CountPublished(std::string const &snapshot) {
  std::size_t n = 0;
  std::size_t pos = 0;
  while ((pos = snapshot.find("\"method\":\"textDocument/publishDiagnostics\"",
                              pos)) != std::string::npos) {
    ++n;
    pos += 1;
  }
  return n;
}

// The last publishDiagnostics payload in a snapshot (from its method key on),
// so negative assertions cannot be tripped by earlier publishes.
std::string LastPublish(std::string const &snapshot) {
  std::size_t const pos =
      snapshot.rfind("\"method\":\"textDocument/publishDiagnostics\"");
  return pos == std::string::npos ? std::string() : snapshot.substr(pos);
}

// Count server->client `workspace/diagnostic/refresh` requests in a snapshot.
std::size_t CountRefreshes(std::string const &snapshot) {
  std::size_t n = 0;
  std::size_t pos = 0;
  while ((pos = snapshot.find("\"method\":\"workspace/diagnostic/refresh\"",
                              pos)) != std::string::npos) {
    ++n;
    pos += 1;
  }
  return n;
}

// Wait until the stream holds at least `count` refresh requests. The refresh
// arrives at the end of the notification that triggered it, so it is also the
// ordering that lets a test assert on what the same notification did NOT do
// (e.g. publish): once the refresh is in the stream, the publish path it
// replaced has already been skipped.
std::string WaitForRefreshes(std::shared_ptr<StringOStream> const &output,
                             size_t count) {
  std::string cur;
  for (int i = 0; i < 100; ++i) {
    cur = output->snapshot();
    if (CountRefreshes(cur) >= count) {
      return cur;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return cur;
}

std::string
ToJsonString(std::string const &s); // defined below in this namespace

char const *kUri = "file://{{tmp}}/hello.bas";

char const kInitializeFrame[] =
    R"FB({"jsonrpc":"2.0","id":"init","method":"initialize","params":{}})FB";

char const kDidOpenFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hello.bas","languageId":"basic","version":1,"text":"print \"hello\"\n"}}})FB";

char const kDidChangeFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hello.bas","version":2},"contentChanges":[{"range":)FB"
    R"FB({"start":{"line":1,"character":0},"end":{"line":1,"character":0}},"text":"' comment\n"}]}})FB";

char const kDidOpenDupFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hello.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"dim x as integer\ndim x as string"}}})FB";

char const kDidOpenHierFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hello.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"sub greet(name as string)\n    print name\nend sub\n\n)FB"
    R"FB(function clamp(v as integer, lo as integer, hi as integer) as integer\n)FB"
    R"FB(    if v < lo then return lo\n    if v > hi then return hi\nend function\n"}}})FB";

// A second document exercising identifier resolution: module dim, a usage,
// and a sub with a shadowing local dim.
char const kDidOpenResolveFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/resolve.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"dim counter as integer\ncounter = counter + 1\n"}}})FB";

// A module with a function and a call to get signature help inside the call.
char const kDidOpenCallsFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/calls.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"function add(a as integer, b as integer) as integer\n    return a + b\nend function\n\n)FB"
    R"FB(dim x as integer\nx = add(1, \n"}}})FB";

// Intrinsic catalog document: expression-prefix positions (`s = le`, `s = pr`),
// a statement-position prefix (`pr`), an intrinsic call for signature help, and
// a `$`-suffixed hover target.
char const kDidOpenIntrinsicFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/intr.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"dim s as string\ns = le\ns = pr\npr\ns = mid$( \"abcdef\", 2 )\ns = left$\n"}}})FB";

// The didChangeConfiguration payload is ignored (settings live in each root's
// freebasicd.toml); the notification only signals the server to re-read.
char const *kDidChangeConfigurationFrame =
    R"({"jsonrpc":"2.0","method":"workspace/didChangeConfiguration","params":{"settings":{}}})";

char const *kShutdownFrame =
    R"FB({"jsonrpc":"2.0","id":2,"method":"shutdown","params":null})FB";

char const *kExitFrame =
    R"FB({"jsonrpc":"2.0","method":"exit","params":null})FB";

// M14 pull diagnostics: a 3.17+ client that advertises textDocument.diagnostic
// (with relatedDocumentSupport) and workspace.diagnostics.refreshSupport. The
// server must answer the pull requests and stop pushing.
char const kInitializePullFrame[] =
    R"FB({"jsonrpc":"2.0","id":"init","method":"initialize","params":{)FB"
    R"FB("capabilities":{"textDocument":{"diagnostic":{"relatedDocumentSupport":true}},)FB"
    R"FB("workspace":{"diagnostics":{"refreshSupport":true}}}}})FB";

// ---------------------------------------------------------------------------
// Wire-offset helpers
// ---------------------------------------------------------------------------
//
// A test reasons about byte offsets; a reply carries them as UTF-16 line and
// character pairs. These convert, so a position in a test is derived from the
// fixture's own bytes rather than transcribed from a reply it is also checking.
// The call-hierarchy and code-lens files share them, which is why they are here
// rather than beside either one.

// Occurrences of `needle` in `text`, for counting the entries of an array in a
// reply.
std::size_t CountOf(std::string const &text, std::string const &needle) {
  std::size_t count = 0;
  for (std::size_t pos = text.find(needle); pos != std::string::npos;
       pos = text.find(needle, pos + needle.size())) {
    ++count;
  }
  return count;
}

// The wire position of a byte offset in a fixture, derived from the fixture's
// own bytes instead of transcribed from a reply. The fixtures hold no
// non-ASCII and no escapes, so a byte offset inside a line is a UTF-16
// character offset — the conversion `utf16Range` itself performs.
std::string WirePosition(std::string const &src, std::size_t off) {
  std::size_t lineStart = 0;
  int line = 0;
  for (std::size_t i = 0; i < off && i < src.size(); ++i) {
    if (src[i] == '\n') {
      ++line;
      lineStart = i + 1;
    }
  }
  return "{\"line\":" + std::to_string(line) +
         ",\"character\":" + std::to_string(off - lineStart) + "}";
}

std::string WireRange(std::string const &src, std::size_t beg,
                      std::size_t end) {
  return "{\"start\":" + WirePosition(src, beg) +
         ",\"end\":" + WirePosition(src, end) + "}";
}

// The wire position of a needle's first character — the anchor a code lens
// carries, which a click sends back as the position it asks about.
std::string WirePositionOf(std::string const &src, std::string const &needle,
                           std::size_t from) {
  std::size_t const at = src.find(needle, from);
  Expect(at != std::string::npos,
         ("the fixture must contain the needle \"" + needle + "\"").c_str());
  if (at == std::string::npos) {
    return needle; // a broken fixture, already reported
  }
  return WirePosition(src, at);
}

// The range of a needle's own text, and the range from the start of one needle
// to the end of another (a whole `sub ... end sub` construct). A needle the
// fixture does not contain fails here, by name, instead of answering with an
// offset nothing can ever match.
std::string WireRangeOf(std::string const &src, std::string const &needle,
                        std::size_t from) {
  std::size_t const beg = src.find(needle, from);
  Expect(beg != std::string::npos,
         ("the fixture must contain the needle \"" + needle + "\"").c_str());
  return WireRange(src, beg, beg + needle.size());
}

std::string WireRangeBetween(std::string const &src,
                             std::string const &begNeedle,
                             std::string const &endNeedle) {
  std::size_t const beg = src.find(begNeedle);
  std::size_t const last = src.rfind(endNeedle);
  Expect(beg != std::string::npos && last != std::string::npos,
         ("the fixture must contain the needles \"" + begNeedle + "\" and \"" +
          endNeedle + "\"")
             .c_str());
  return WireRange(src, beg, last + endNeedle.size());
}

// The signature of the declaration starting at `needle`: the text from there to
// the end of its line, which is what the server reports as an item's `detail`
// (the same text documentSymbol shows).
std::string SignatureLine(std::string const &src, std::string const &needle) {
  std::size_t const beg = src.find(needle);
  Expect(
      beg != std::string::npos,
      ("the fixture must contain the declaration \"" + needle + "\"").c_str());
  if (beg == std::string::npos) {
    return needle; // a broken fixture, already reported: never substr past the
                   // end
  }
  return src.substr(beg, src.find('\n', beg) - beg);
}

// A `CallHierarchyItem` as a client hands it back in a follow-up request: the
// protocol identifies a node by its uri and selectionRange, which is the pair
// the server answers against. Built from the fixture's bytes for the same
// reason as the ranges above.
std::string CallItemJson(std::string const &uri, std::string const &name,
                         std::string const &range,
                         std::string const &selectionRange) {
  return "{\"name\":\"" + name + "\",\"kind\":6,\"uri\":\"" + uri +
         "\",\"range\":" + range + ",\"selectionRange\":" + selectionRange +
         "}";
}

} // namespace fbtest
