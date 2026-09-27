/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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

namespace {
using test::Expect;
using test::FeedableIStream;
using test::StringOStream;
using test::WaitForOutputContaining;

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

std::string
ToJsonString(std::string const &s); // defined below in this namespace

struct TwoFileFixture {
  std::filesystem::path sandbox;
  std::filesystem::path wsDir;
  std::string libUri;
  std::string mainUri;
  std::string progUri;
  std::string extraUri;
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
    libUri = FileUri(wsDir / "lib.bi");
    mainUri = FileUri(wsDir / "main.bas");
    progUri = FileUri(wsDir / "prog.bas");
    extraUri = FileUri(wsDir / "extra.bi");
    rootUri = FileUri(wsDir);
  }

  ~TwoFileFixture() {
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
  }
};

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

// BUGS.md reuse example: a block-local `dim x` shadows the module `dim x`
// (valid FreeBASIC), so must not be reported as a duplicate definition.
char const kDidOpenScopeDupFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hello.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"dim x as integer = 1\nscope\n    dim x as string = \"Hello\"\nend scope\nx = x + 1\n"}}})FB";

char const kDidOpenHierFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hello.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"sub greet(name as string)\n    print name\nend sub\n\n)FB"
    R"FB(function clamp(v as integer, lo as integer, hi as integer) as integer\n)FB"
    R"FB(    if v < lo then return lo\n    if v > hi then return hi\nend function\n"}}})FB";

char const *kDocumentSymbolFrame =
    R"FB({"jsonrpc":"2.0","id":"dsym","method":"textDocument/documentSymbol","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hello.bas"}}})FB";

// Line 4 is the `function clamp(...)` header; the cursor sits on that line.
char const *kHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"hov","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hello.bas"},"position":{"line":4,"character":1}}})FB";

char const *kHoverOnBodyFrame =
    R"FB({"jsonrpc":"2.0","id":"hov2","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hello.bas"},"position":{"line":5,"character":4}}})FB";

// A WITH + FOR + IF document mirroring drd/temp/src/engine.bas: a block-local
// `v1` used inside a `type(...)` initializer. Hovering the *usage* must show
// v1's declaration, not the enclosing block.
char const kDidOpenHoverUsageFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hovuse.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"sub ProcessSectorPhysics(map as map_struct, secIndex as integer)\n)FB"
    R"FB(    with map\n)FB"
    R"FB(        for i as integer = 0 to 3\n)FB"
    R"FB(            dim as Vector2 v1 = .vertices(i)\n)FB"
    R"FB(            if v1.x <> 0 then\n)FB"
    R"FB(                dim as Vector3 b = type(v1.x, .sectors(secIndex).floorHeight, v1.y)\n)FB"
    R"FB(            end if\n)FB"
    R"FB(        next i\n)FB"
    R"FB(    end with\n)FB"
    R"FB(end sub\n"}}})FB";

// Hover the `v1` usage inside the `type(...)` initializer (line 5, char 40).
char const *kHoverUsageFrame =
    R"FB({"jsonrpc":"2.0","id":"hvu","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovuse.bas"},"position":{"line":5,"character":40}}})FB";

// Hover the `v1` declaration itself (line 3, char 27).
char const *kHoverDeclFrame =
    R"FB({"jsonrpc":"2.0","id":"hvd","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovuse.bas"},"position":{"line":3,"character":27}}})FB";

// Hover the loop counter `i` in the for header (line 2, char 12).
char const *kHoverCounterFrame =
    R"FB({"jsonrpc":"2.0","id":"hvc","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovuse.bas"},"position":{"line":2,"character":12}}})FB";

// Hover a module-level Dim usage (resolve.bas line 1, char 0 = `counter`).
char const *kModuleDimHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"hvm","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/resolve.bas"},"position":{"line":1,"character":0}}})FB";

// Member-access hover (the reported regression): a WITH + inline UDT
// document mirroring drd/temp's world.bi/engine.bas shape. Hovering
// `.walls`, `.sectors(i).floorHeight` or `w.v1` must show the *field*
// declaration and its owning variable/type — never the enclosing sub.
char const kDidOpenMemberHoverFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hovmem.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"type Wall\n)FB"
    R"FB(    as integer v1, v2\n)FB"
    R"FB(end type\n)FB"
    R"FB(type Vector2\n)FB"
    R"FB(    as single x, y\n)FB"
    R"FB(end type\n)FB"
    R"FB(type Sector\n)FB"
    R"FB(    as single floorHeight\n)FB"
    R"FB(end type\n)FB"
    R"FB(type Map\n)FB"
    R"FB(    as Vector2 vertices(10)\n)FB"
    R"FB(    as Wall walls(10)\n)FB"
    R"FB(    as Sector sectors(10)\n)FB"
    R"FB(end type\n)FB"
    R"FB(sub run(map as Map, secIndex as integer)\n)FB"
    R"FB(    with map\n)FB"
    R"FB(        dim as Wall w = .walls(secIndex)\n)FB"
    R"FB(        dim as single f = .sectors(secIndex).floorHeight\n)FB"
    R"FB(        dim as single g = w.v1\n)FB"
    R"FB(    end with\n)FB"
    R"FB(end sub\n"}}})FB";

// Hover the `walls` member of the with-target (line 16, char 26).
char const *kMemberHoverWallFrame =
    R"FB({"jsonrpc":"2.0","id":"hmm","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovmem.bas"},"position":{"line":16,"character":26}}})FB";

// Hover `floorHeight` through the indexed chain (line 17, char 50).
char const *kMemberHoverFloorFrame =
    R"FB({"jsonrpc":"2.0","id":"hmf","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovmem.bas"},"position":{"line":17,"character":50}}})FB";

// Hover `v1` of the plain local variable (line 18, char 29).
char const *kMemberHoverLocalFrame =
    R"FB({"jsonrpc":"2.0","id":"hml","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovmem.bas"},"position":{"line":18,"character":29}}})FB";

// Enum conformance hover: an `Explicit` enum's branded value
// (`MyEnum.value_1`), a plain enum's qualified member with a *reserved-word*
// enum name (`color.green` — `color` is the graphics intrinsic), and a bare
// plain-enum member usage (`z = green`) must all resolve to the member
// declaration — the empty/orphan hover regression.
char const kDidOpenEnumHoverFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/enumhov.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"enum MyEnum explicit\n)FB"
    R"FB(    value_1 = 1\n)FB"
    R"FB(    value_2 = 2\n)FB"
    R"FB(end enum\n)FB"
    R"FB(enum color\n)FB"
    R"FB(    red = 1\n)FB"
    R"FB(    green = 2\n)FB"
    R"FB(end enum\n)FB"
    R"FB(dim x = MyEnum.value_1\n)FB"
    R"FB(dim y = color.green\n)FB"
    R"FB(dim z = green\n"}}})FB";

// Hover the explicit enum's branded member (line 8, char 15 = `value_1`).
char const *kEnumHoverBrandedFrame =
    R"FB({"jsonrpc":"2.0","id":"he1","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/enumhov.bas"},"position":{"line":8,"character":15}}})FB";

// Hover the reserved-word enum name's branded member (line 9, char 14 =
// `green` of `color.green`).
char const *kEnumHoverKeywordNameFrame =
    R"FB({"jsonrpc":"2.0","id":"he2","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/enumhov.bas"},"position":{"line":9,"character":14}}})FB";

// Hover a bare plain-enum member usage (line 10, char 8 = `green`).
char const *kEnumHoverBareFrame =
    R"FB({"jsonrpc":"2.0","id":"he3","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/enumhov.bas"},"position":{"line":10,"character":8}}})FB";

char const *kFoldingRangeFrame =
    R"FB({"jsonrpc":"2.0","id":"fold","method":"textDocument/foldingRange","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hello.bas"}}})FB";

// A second document exercising identifier resolution: module dim, a usage,
// and a sub with a shadowing local dim.
char const kDidOpenResolveFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/resolve.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"dim counter as integer\ncounter = counter + 1\n"}}})FB";

char const *kDefinitionFrame =
    R"FB({"jsonrpc":"2.0","id":"def","method":"textDocument/definition","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/resolve.bas"},"position":{"line":1,"character":0}}})FB";

char const *kReferencesFrame =
    R"FB({"jsonrpc":"2.0","id":"ref","method":"textDocument/references","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/resolve.bas"},"position":{"line":1,"character":0},)FB"
    R"FB("context":{"includeDeclaration":true}}})FB";

char const *kHighlightFrame =
    R"FB({"jsonrpc":"2.0","id":"hl","method":"textDocument/documentHighlight","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/resolve.bas"},"position":{"line":1,"character":0}}})FB";

// A module with a function and a call to get signature help inside the call.
char const kDidOpenCallsFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/calls.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"function add(a as integer, b as integer) as integer\n    return a + b\nend function\n\n)FB"
    R"FB(dim x as integer\nx = add(1, \n"}}})FB";

char const *kCompletionFrame =
    R"FB({"jsonrpc":"2.0","id":"comp","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/resolve.bas"},"position":{"line":1,"character":8}}})FB";

// Member completion context (PLAN.md M19): `p.` must complete only the UDT's
// accessible members — never keywords, globals, or intrinsics. The example
// type has a private member, so a module-level `p.` offers just x/y; inside
// the type's own member procedure the private member is offered too
// (FreeBASIC.md §7 Access sections, fbc's error-202 gate). The variable name
// is `p`, distinct
// from the type `Position`: FreeBASIC keys identifiers case-insensitively, so
// `dim position as Position` would collide the variable with its own type
// (fbc separates the namespaces; today's resolver does not — see §12).
char const kDidOpenMemberCompletionModuleFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/memcomp.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"type Position\n)FB"
    R"FB(    x as integer\n)FB"
    R"FB(    y as integer\n)FB"
    R"FB(    private:\n)FB"
    R"FB(    hidden as integer\n)FB"
    R"FB(end type\n)FB"
    R"FB(dim p as Position\n)FB"
    R"FB(p.\n"}}})FB";

// Cursor at the end of line 7 (`p.`), right after the dot.
char const *kMemberCompletionModuleFrame =
    R"FB({"jsonrpc":"2.0","id":"mc1","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/memcomp.bas"},"position":{"line":7,"character":2}}})FB";

char const kDidOpenMemberCompletionProcFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/memcomp2.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"type Position\n)FB"
    R"FB(    x as integer\n)FB"
    R"FB(    y as integer\n)FB"
    R"FB(    private:\n)FB"
    R"FB(    hidden as integer\n)FB"
    R"FB(end type\n)FB"
    R"FB(sub Position.set()\n)FB"
    R"FB(    dim obj as Position\n)FB"
    R"FB(    obj.\n"}}})FB";

// Cursor at the end of line 8 (`    obj.`), right after the dot inside the
// member procedure implementation.
char const *kMemberCompletionProcFrame =
    R"FB({"jsonrpc":"2.0","id":"mc2","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/memcomp2.bas"},"position":{"line":8,"character":8}}})FB";

char const *kSignatureHelpFrame =
    R"FB({"jsonrpc":"2.0","id":"sig","method":"textDocument/signatureHelp","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/calls.bas"},"position":{"line":5,"character":11}}})FB";

char const *kKeywordHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"khh","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/calls.bas"},"position":{"line":4,"character":0}}})FB";

// A procedure whose body is a dense run of reserved keywords: hovering any of
// them must show the keyword wiki link, never the enclosing sub signature (the
// reported regression). `sub` at its own header still shows the signature.
char const kDidOpenKeywordBodyFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/keybody.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"sub run(m as Map, secIndex as integer)\n)FB"
    R"FB(    dim as integer walls\n)FB"
    R"FB(    with map\n)FB"
    R"FB(        dim as integer w = .walls(secIndex)\n)FB"
    R"FB(    end with\n"}}})FB";

// Hover the `dim` inside the body (line 1, char 4).
char const *kKeywordBodyDimFrame =
    R"FB({"jsonrpc":"2.0","id":"hkd","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/keybody.bas"},"position":{"line":1,"character":4}}})FB";

// Hover the `with` inside the body (line 2, char 4).
char const *kKeywordBodyWithFrame =
    R"FB({"jsonrpc":"2.0","id":"hkw","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/keybody.bas"},"position":{"line":2,"character":4}}})FB";

// Hover the `end` closer (line 4, char 4).
char const *kKeywordBodyEndFrame =
    R"FB({"jsonrpc":"2.0","id":"hke","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/keybody.bas"},"position":{"line":4,"character":4}}})FB";

// Hover the `sub` word of its own header (line 0, char 0): the keyword opens
// the declaration, so the signature — not a generic wiki link — is shown.
char const *kKeywordBodySubHeadFrame =
    R"FB({"jsonrpc":"2.0","id":"hks","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/keybody.bas"},"position":{"line":0,"character":0}}})FB";

// Cross-file member hover: the field's type lives in a header the requesting
// file's include closure cannot reach (the `#include` names a missing file),
// so even then a member whose name collides with a local variable must resolve
// to the *member* through the workspace byKey fallback, not to the variable
// and not to the enclosing sub. Types are only in the second buffer.
char const kDidOpenCrossTypeMainFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hovx.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"#include once \"zzz_unresolved.bi\"\n)FB"
    R"FB(sub run(m as Map)\n)FB"
    R"FB(    dim as integer walls\n)FB"
    R"FB(    dim as integer w = m.walls(1)\n)FB"
    R"FB(end sub\n"}}})FB";

char const kDidOpenCrossTypeWorldFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/world.bi","languageId":"basic","version":1,)FB"
    R"FB("text":"type Wall\n)FB"
    R"FB(    as integer v1, v2\n)FB"
    R"FB(end type\n)FB"
    R"FB(type Map\n)FB"
    R"FB(    as Wall walls(10)\n)FB"
    R"FB(end type\n"}}})FB";

// Hover the `walls` member in `m.walls(1)` (line 3, char 26).
char const *kCrossTypeMemberFrame =
    R"FB({"jsonrpc":"2.0","id":"hxm","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovx.bas"},"position":{"line":3,"character":26}}})FB";

// FreeBASIC lets reserved words name type members (`as string name`), so
// hovering `t.name` must show the member/type info — never the intrinsic/
// keyword page that would otherwise attach to the reserved word (fbc-verified,
// FreeBASIC.md §2).
char const kDidOpenKeywordMemberFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hovkw.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"type Mytype\n)FB"
    R"FB(    as string name\n)FB"
    R"FB(    as integer other\n)FB"
    R"FB(end type\n)FB"
    R"FB(dim t as Mytype\n)FB"
    R"FB(t.name = \"A Name\"\n)FB"
    R"FB(sub s\n)FB"
    R"FB(    dim v as Mytype\n)FB"
    R"FB(    v.name = \"x\"\n)FB"
    R"FB(end sub\n"}}})FB";

// Hover `name` *inside* the word in `t.name` at module level (line 5, char
// 3) — a cursor on the first char after `.` lands on the `.` token, which is
// a separate pre-existing wart, so poke the middle of the word.
char const *kKeywordMemberHoverModuleFrame =
    R"FB({"jsonrpc":"2.0","id":"hkm","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovkw.bas"},"position":{"line":5,"character":3}}})FB";

// Hover the member's own declaration `name` in `as string name` (line 1,
// char 15): must behave like any identifier field, not the keyword page.
char const *kKeywordMemberHoverDeclFrame =
    R"FB({"jsonrpc":"2.0","id":"hkd","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovkw.bas"},"position":{"line":1,"character":15}}})FB";

// Hover `name` in `v.name` inside a sub body (line 8, char 7).
char const *kKeywordMemberHoverSubFrame =
    R"FB({"jsonrpc":"2.0","id":"hks","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovkw.bas"},"position":{"line":8,"character":7}}})FB";

// Unknown declared type: `map` is `as Shape`, but no `Shape` type exists
// anywhere (not in this file, not the workspace). Hovering `.walls` inside the
// `with` block must still say "Member of `map`." and must not fall back to the
// colliding local `walls` or to the enclosing sub's signature.
char const kDidOpenHovUnknownTypeFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hovunk.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"sub run(map as Shape, secIndex as integer)\n)FB"
    R"FB(    dim as integer walls\n)FB"
    R"FB(    with map\n)FB"
    R"FB(        dim as integer a = .walls(secIndex)\n)FB"
    R"FB(    end with\n)FB"
    R"FB(end sub\n"}}})FB";

// Hover the `walls` member of `.walls(secIndex)` (line 3, char 28).
char const *kHoverUnknownTypeMemberFrame =
    R"FB({"jsonrpc":"2.0","id":"hxu","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovunk.bas"},"position":{"line":3,"character":28}}})FB";

// Known type, missing member: `Map` exists with only a `walls` field, so
// `m2.missing` cannot resolve to a field — the hover must still name the
// owning variable and its (known) type.
char const kDidOpenHovMissingMemberFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hovmiss.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"type Map\n)FB"
    R"FB(    as integer walls(10)\n)FB"
    R"FB(end type\n)FB"
    R"FB(sub run(m2 as Map)\n)FB"
    R"FB(    dim as integer f = m2.missing\n)FB"
    R"FB(end sub\n"}}})FB";

// Hover the `missing` member of `m2.missing` (line 4, char 26).
char const *kHoverMissingMemberFrame =
    R"FB({"jsonrpc":"2.0","id":"hxq","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovmiss.bas"},"position":{"line":4,"character":26}}})FB";

// Intrinsic catalog document: expression-prefix positions (`s = le`, `s = pr`),
// a statement-position prefix (`pr`), an intrinsic call for signature help, and
// a `$`-suffixed hover target.
char const kDidOpenIntrinsicFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/intr.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"dim s as string\ns = le\ns = pr\npr\ns = mid$( \"abcdef\", 2 )\ns = left$\n"}}})FB";

char const *kIntrinsicExprLeFrame =
    R"FB({"jsonrpc":"2.0","id":"ile","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/intr.bas"},"position":{"line":1,"character":6}}})FB";

char const *kIntrinsicExprPrFrame =
    R"FB({"jsonrpc":"2.0","id":"iep","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/intr.bas"},"position":{"line":2,"character":6}}})FB";

char const *kIntrinsicStmtPrFrame =
    R"FB({"jsonrpc":"2.0","id":"isp","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/intr.bas"},"position":{"line":3,"character":2}}})FB";

char const *kIntrinsicSignatureFrame =
    R"FB({"jsonrpc":"2.0","id":"isg","method":"textDocument/signatureHelp","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/intr.bas"},"position":{"line":4,"character":20}}})FB";

char const *kIntrinsicHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"ihv","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/intr.bas"},"position":{"line":5,"character":4}}})FB";

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

std::string SemDeltaFrame(std::string const &id, std::string const &previous) {
  return "{\"jsonrpc\":\"2.0\",\"id\":\"" + id +
         "\",\"method\":\"textDocument/semanticTokens/full/delta\",\"params\":"
         "{\"textDocument\":{\"uri\":\"file://{{tmp}}/fblsp-sem.bas\"},"
         "\"previousResultId\":\"" +
         previous + "\"}}";
}

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

char const kDidCloseFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hello.bas"}})FB";

// The didChangeConfiguration payload is ignored (settings live in each root's
// freebasicd.toml); the notification only signals the server to re-read.
char const *kDidChangeConfigurationFrame =
    R"({"jsonrpc":"2.0","method":"workspace/didChangeConfiguration","params":{"settings":{}}})";

char const *kShutdownFrame =
    R"FB({"jsonrpc":"2.0","id":2,"method":"shutdown","params":null})FB";
char const *kExitFrame =
    R"FB({"jsonrpc":"2.0","method":"exit","params":null})FB";

// A client that opts into `workspace.didChangeWatchedFiles` dynamic
// registration; must be registered for it on the `initialized` notification.
char const kInitializeDynamicFrame[] =
    R"FB({"jsonrpc":"2.0","id":"init","method":"initialize","params":{)FB"
    R"FB("capabilities":{"workspace":{"didChangeWatchedFiles":{"dynamicRegistration":true}}}}})FB";

char const *kInitializedFrame =
    R"FB({"jsonrpc":"2.0","method":"initialized","params":{}})FB";

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

void TestInitializeReportsSyncCapabilities() {
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

  Expect(response.find("\"id\":\"init\"") != std::string::npos,
         "initialize must receive a response");
  Expect(response.find("\"textDocumentSync\"") != std::string::npos,
         "initialize response must advertise textDocumentSync");
  Expect(response.find("\"openClose\":true") != std::string::npos,
         "textDocumentSync must advertise openClose");
  Expect(response.find("\"change\":2") != std::string::npos,
         "textDocumentSync must advertise incremental changes");
  Expect(response.find("\"documentSymbolProvider\":true") != std::string::npos,
         "initialize response must advertise documentSymbol support");
  Expect(response.find("\"hoverProvider\":true") != std::string::npos,
         "initialize response must advertise hover support");
  Expect(response.find("\"foldingRangeProvider\":true") != std::string::npos,
         "initialize response must advertise foldingRange support");
  Expect(response.find("\"definitionProvider\":true") != std::string::npos,
         "initialize response must advertise definition support");
  Expect(response.find("\"referencesProvider\":true") != std::string::npos,
         "initialize response must advertise references support");
  Expect(response.find("\"documentHighlightProvider\":true") !=
             std::string::npos,
         "initialize response must advertise documentHighlight support");
  Expect(response.find("\"completionProvider\"") != std::string::npos,
         "initialize response must advertise completion support");
  Expect(response.find("\"signatureHelpProvider\"") != std::string::npos,
         "initialize response must advertise signatureHelp support");
  Expect(response.find("\"renameProvider\"") != std::string::npos,
         "initialize response must advertise rename support");
  Expect(response.find("\"prepareProvider\":true") != std::string::npos,
         "initialize response must advertise prepareRename support");
  Expect(response.find("\"codeActionProvider\"") != std::string::npos,
         "initialize response must advertise codeAction support");
  Expect(response.find("\"quickfix\"") != std::string::npos,
         "codeActionProvider must advertise the quickfix kind it serves");

  session.stop();
}

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

  // The SUB never closes, so the parser publishes `unterminated-block` at the
  // opener and nothing else.
  input->append(
      MakeLspFrame(OpenFrame(fix.mainUri, "sub main()\n  print 1\n").c_str()));
  Expect(
      WaitForPublishedUri(output, 1).find("\"code\":\"unterminated-block\"") !=
          std::string::npos,
      "an unterminated block must publish its diagnostic before it is fixable");

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
  Expect(
      LastPublish(WaitForPublishedUri(output, 2)).find("unterminated-block") ==
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
  Expect(WaitForPublishedUri(output, 1).find("\"code\":\"stray-closer\"") !=
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

void TestDidOpenPublishesDiagnostics() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenFrame));
  std::string const output_all = WaitForPublishedUri(output, 1);

  Expect(output_all.find(Expand(kUri)) != std::string::npos,
         "publishDiagnostics must carry the opened uri");
  Expect(output_all.find("\"diagnostics\":[]") != std::string::npos,
         "a healthy document must publish no diagnostics");

  session.stop();
}

void TestDiagnosticsReflectParseErrors() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenDupFrame));
  std::string const output_all = WaitForPublishedUri(output, 1);

  Expect(output_all.find("\"code\":\"duplicate-definition\"") !=
             std::string::npos,
         "duplicate dims must be reported with their diagnostic code");
  Expect(output_all.find("duplicate definition: 'x'") != std::string::npos,
         "duplicate dims must be reported with the offending name");
  Expect(output_all.find("\"severity\":2") != std::string::npos,
         "duplicate dims must be reported as warnings");
  Expect(output_all.find("\"source\":\"freebasicd\"") != std::string::npos,
         "diagnostics must carry the server source name");
  Expect(output_all.find("\"start\":{\"line\":1,\"character\":4}") !=
             std::string::npos,
         "the duplicate range must cover the second 'x' name token (byte -> "
         "utf-16)");
  Expect(output_all.find("\"end\":{\"line\":1,\"character\":5}") !=
             std::string::npos,
         "the duplicate range must end after the second 'x' name token");

  session.stop();
}

// BUGS.md: a block-local `dim` that shadows an outer name must not be
// reported as a duplicate definition — declaration scopes reuse parent names.
void TestDiagnosticsRespectDeclarationScopes() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenScopeDupFrame));
  std::string const output_all = WaitForPublishedUri(output, 1);

  Expect(output_all.find("\"code\":\"duplicate-definition\"") ==
             std::string::npos,
         "a block-local Dim shadowing an outer Dim is not a duplicate");
  Expect(output_all.find("\"diagnostics\":[]") != std::string::npos,
         "a shadowing block must publish a healthy diagnostic list");

  session.stop();
}

void TestDocumentSymbolsReturnHierarchy() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenHierFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kDocumentSymbolFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"dsym\"");

  Expect(response.find("\"id\":\"dsym\"") != std::string::npos,
         "documentSymbol request must receive a response");
  Expect(response.find("\"name\":\"greet\"") != std::string::npos,
         "documentSymbol must include the SUB symbol");
  Expect(response.find("\"name\":\"clamp\"") != std::string::npos,
         "documentSymbol must include the FUNCTION symbol");
  Expect(response.find("\"kind\":6") != std::string::npos,
         "SUB must map to the Method symbol kind");
  Expect(response.find("\"kind\":12") != std::string::npos,
         "FUNCTION must map to the Function symbol kind");
  Expect(response.find("\"kind\":253") != std::string::npos,
         "parameters must map to the Parameter symbol kind");
  Expect(response.find("\"children\"") != std::string::npos,
         "nested parameter symbols must be reported as children");
  Expect(response.find("\"selectionRange\"") != std::string::npos,
         "document symbols must carry a selection range for the name token");

  session.stop();
}

void TestHoverShowsSignatureAndDoc() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenHierFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kHoverFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"hov\"");

  Expect(response.find("\"id\":\"hov\"") != std::string::npos,
         "hover request must receive a response");
  Expect(response.find("\"kind\":\"markdown\"") != std::string::npos,
         "hover contents must be markdown");
  Expect(response.find("function clamp(v as integer, lo as integer, hi as "
                       "integer) as integer") != std::string::npos,
         "hover over the function header must show its signature");
  Expect(response.find("\"range\"") != std::string::npos,
         "hover must carry the selection range of the hovered symbol");

  input->append(MakeLspFrame(kHoverOnBodyFrame));
  std::string const bodyHover =
      WaitForOutputContaining(output, "\"id\":\"hov2\"");
  Expect(bodyHover.find("function clamp(v as integer") != std::string::npos,
         "hover anywhere inside a function must resolve to that function");

  session.stop();
}

void TestHoverResolvesUsageToDeclaration() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenHoverUsageFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kHoverUsageFrame));
  std::string const usageHover =
      WaitForOutputContaining(output, "\"id\":\"hvu\"");
  Expect(usageHover.find("\"id\":\"hvu\"") != std::string::npos,
         "usage hover request must receive a response");
  Expect(usageHover.find("dim as Vector2 v1 = .vertices(i)") !=
             std::string::npos,
         "hover on a usage must show the declaring Dim with its type and "
         "initializer");
  Expect(usageHover.find("Local variable in Sub `ProcessSectorPhysics`, "
                         "inside the `for` block.") != std::string::npos,
         "hover must describe the variable's kind and the block it lives in");
  Expect(usageHover.find("`if`") == std::string::npos,
         "hover must never surface a scope-block node as the symbol");

  input->append(MakeLspFrame(kHoverDeclFrame));
  std::string const declHover =
      WaitForOutputContaining(output, "\"id\":\"hvd\"");
  Expect(declHover.find("dim as Vector2 v1 = .vertices(i)") !=
             std::string::npos,
         "hover on the declaration itself must show the same symbol info");

  input->append(MakeLspFrame(kHoverCounterFrame));
  std::string const counterHover =
      WaitForOutputContaining(output, "\"id\":\"hvc\"");
  Expect(counterHover.find("for i as integer = 0 to 3") != std::string::npos,
         "hover on a for-loop counter must show the header with its type");
  Expect(counterHover.find("Loop counter in Sub `ProcessSectorPhysics`.") !=
             std::string::npos,
         "hover must label the counter as a loop counter in its procedure");

  input->append(MakeLspFrame(kDidOpenResolveFrame));
  Expect(WaitForPublishedUri(output, 2).empty() == false,
         "second didOpen must publish diagnostics");
  input->append(MakeLspFrame(kModuleDimHoverFrame));
  std::string const moduleHover =
      WaitForOutputContaining(output, "\"id\":\"hvm\"");
  Expect(moduleHover.find("Module-level variable") != std::string::npos,
         "hover on a module-level usage must label it module-level");

  session.stop();
}

void TestHoverShowsMemberAccess() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenMemberHoverFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  // `.walls` — the reported regression: the member of the with-target, not
  // the enclosing sub's signature.
  input->append(MakeLspFrame(kMemberHoverWallFrame));
  std::string const wallHover =
      WaitForOutputContaining(output, "\"id\":\"hmm\"");
  Expect(wallHover.find("\"id\":\"hmm\"") != std::string::npos,
         "member hover request must receive a response");
  Expect(wallHover.find("as Wall walls(10)") != std::string::npos,
         "member hover shows the field's declaration line (type + name)");
  Expect(wallHover.find("Member of `map` (`Map`).") != std::string::npos,
         "a with-implicit member names the with-target variable and its type");
  Expect(wallHover.find("sub run(") == std::string::npos,
         "member hover must never fall back to the enclosing sub signature");

  // `.sectors(secIndex).floorHeight` — indexed chain lands on the element
  // type, so the owning type is Sector, not Map.
  input->append(MakeLspFrame(kMemberHoverFloorFrame));
  std::string const floorHover =
      WaitForOutputContaining(output, "\"id\":\"hmf\"");
  Expect(floorHover.find("as single floorHeight") != std::string::npos,
         "chained member hover shows the leaf field's declaration");
  Expect(floorHover.find("Member of `Sector`.") != std::string::npos,
         "a deep member names its owning type (indexed element type)");

  // `w.v1` — plain local variable base names the variable and its type.
  input->append(MakeLspFrame(kMemberHoverLocalFrame));
  std::string const localHover =
      WaitForOutputContaining(output, "\"id\":\"hml\"");
  Expect(localHover.find("as integer v1, v2") != std::string::npos,
         "member hover shows the whole field list of the declaration line");
  Expect(localHover.find("Member of `w` (`Wall`).") != std::string::npos,
         "a variable member names the base variable and its type");

  session.stop();
}

// Enum members resolve on hover: an `Explicit` enum's branded value
// (`MyEnum.value_1`), a plain enum's qualified member with a reserved-word
// enum name (`color.green`), and a bare plain-enum member usage (`z = green`)
// all land on the member declaration and label it as an enum member.
void TestHoverShowsEnumMembers() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenEnumHoverFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  auto hover = [&](char const *frame, char const *id, char const *signature) {
    input->append(MakeLspFrame(frame));
    std::string const h = WaitForOutputContaining(output, id);
    Expect(h.find(id) != std::string::npos,
           "the enum hover request must receive a response");
    Expect(h.find(signature) != std::string::npos,
           "the enum hover must show the member's declaration line");
    return h;
  };

  // Explicit enum, branded member: `MyEnum.value_1`.
  std::string const branded =
      hover(kEnumHoverBrandedFrame, "\"id\":\"he1\"", "value_1 = 1");
  Expect(branded.find("Enum member of `MyEnum`.") != std::string::npos,
         "an explicit enum's branded member names its enum");

  // Plain enum whose name is a reserved word: `color.green` still resolves.
  std::string const kwNamed =
      hover(kEnumHoverKeywordNameFrame, "\"id\":\"he2\"", "green = 2");
  Expect(kwNamed.find("Enum member of `color`.") != std::string::npos,
         "a reserved-word enum name still names the member's enum");

  // Bare usage of a plain enum's member resolves to the member declaration.
  std::string const bare =
      hover(kEnumHoverBareFrame, "\"id\":\"he3\"", "green = 2");
  Expect(bare.find("Enum member of `color`.") != std::string::npos,
         "a bare plain-enum member usage names the member's enum");

  session.stop();
}

void TestKeywordHoverInsideProcedureShowsWikiLink() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenKeywordBodyFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  auto hover = [&](char const *frame, char const *id) {
    input->append(MakeLspFrame(frame));
    return WaitForOutputContaining(output, id);
  };

  // The reported regression: hovering a keyword inside a body used to show the
  // enclosing procedure's signature. It must show the keyword's wiki link.
  std::string const dimHover = hover(kKeywordBodyDimFrame, "\"id\":\"hkd\"");
  Expect(dimHover.find("\"id\":\"hkd\"") != std::string::npos,
         "keyword hover request must receive a response");
  Expect(dimHover.find("KeyPgDim") != std::string::npos,
         "the `dim` keyword inside a body shows its wiki link");
  Expect(dimHover.find("sub run(m as Map") == std::string::npos,
         "the `dim` keyword inside a body must not show the enclosing sub");

  std::string const withHover = hover(kKeywordBodyWithFrame, "\"id\":\"hkw\"");
  Expect(withHover.find("KeyPgWith") != std::string::npos,
         "the `with` keyword inside a body shows its wiki link");
  Expect(withHover.find("sub run(m as Map") == std::string::npos,
         "the `with` keyword inside a body must not show the enclosing sub");

  std::string const endHover = hover(kKeywordBodyEndFrame, "\"id\":\"hke\"");
  Expect(endHover.find("KeyPgEnd") != std::string::npos,
         "the `end` keyword inside a body shows its wiki link");
  Expect(endHover.find("sub run(m as Map") == std::string::npos,
         "the `end` keyword inside a body must not show the enclosing sub");

  // The opener of a declaration is not a generic keyword: hovering the `sub`
  // word of its own header must keep showing the procedure signature.
  std::string const subHead = hover(kKeywordBodySubHeadFrame, "\"id\":\"hks\"");
  Expect(subHead.find("sub run(m as Map") != std::string::npos,
         "the `sub` word of its own header still shows the signature");

  session.stop();
}

void TestMemberHoverResolvesCrossFileTypeOutsideClosure() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  // The include names a file that does not exist, so the type can only be
  // reached through the workspace byKey fallback, not the include closure.
  input->append(MakeLspFrame(kDidOpenCrossTypeMainFrame));
  input->append(MakeLspFrame(kDidOpenCrossTypeWorldFrame));
  Expect(WaitForPublishedUri(output, 2).empty() == false,
         "both didOpens must publish diagnostics");

  input->append(MakeLspFrame(kCrossTypeMemberFrame));
  std::string const hover = WaitForOutputContaining(output, "\"id\":\"hxm\"");
  Expect(hover.find("\"id\":\"hxm\"") != std::string::npos,
         "member hover request must receive a response");
  Expect(hover.find("as Wall walls(10)") != std::string::npos,
         "a member whose type is outside the include closure still resolves to "
         "the field declaration");
  Expect(hover.find("Member of `m` (`Map`).") != std::string::npos,
         "the outside-closure member names the base variable and its type");
  Expect(hover.find("dim as integer walls") == std::string::npos,
         "the member must not fall back to the colliding local variable");
  Expect(hover.find("sub run(") == std::string::npos,
         "the member must not fall back to the enclosing sub signature");

  session.stop();
}

void TestMemberHoverKeywordNamedMember() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenKeywordMemberFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  // `t.name` at module level: the reserved word is a member, so hover shows
  // the field — not the `Name(...)` intrinsic page the lexer kind would
  // otherwise produce.
  input->append(MakeLspFrame(kKeywordMemberHoverModuleFrame));
  std::string const moduleHover =
      WaitForOutputContaining(output, "\"id\":\"hkm\"");
  Expect(moduleHover.find("\"id\":\"hkm\"") != std::string::npos,
         "keyword-member hover request must receive a response");
  Expect(moduleHover.find("as string name") != std::string::npos,
         "hovering a reserved-word member shows its declaration line");
  Expect(moduleHover.find("Member of `t` (`Mytype`).") != std::string::npos,
         "the keyword member names the base variable and its type");
  Expect(moduleHover.find("FreeBASIC intrinsic") == std::string::npos,
         "a reserved-word member must not show the intrinsic page");
  Expect(moduleHover.find("www.freebasic.net") == std::string::npos,
         "a reserved-word member must not show the keyword wiki link");

  // Hovering the member's own declaration behaves like an identifier field.
  input->append(MakeLspFrame(kKeywordMemberHoverDeclFrame));
  std::string const declHover =
      WaitForOutputContaining(output, "\"id\":\"hkd\"");
  Expect(declHover.find("as string name") != std::string::npos,
         "hovering the declaration shows the same field info");
  Expect(declHover.find("Field of type `Mytype`.") != std::string::npos,
         "the reserved-word declaration is a field of its type");
  Expect(declHover.find("FreeBASIC intrinsic") == std::string::npos,
         "the declaration must not show the intrinsic page");

  // Inside a sub body, `v.name` still names the member — never the enclosing
  // procedure's signature.
  input->append(MakeLspFrame(kKeywordMemberHoverSubFrame));
  std::string const subHover =
      WaitForOutputContaining(output, "\"id\":\"hks\"");
  Expect(subHover.find("as string name") != std::string::npos,
         "the in-sub keyword member shows its declaration line");
  Expect(subHover.find("Member of `v` (`Mytype`).") != std::string::npos,
         "the in-sub keyword member names the base variable and its type");
  Expect(subHover.find("sub s(") == std::string::npos,
         "the keyword member must not fall back to the enclosing sub");

  session.stop();
}

void TestMemberHoverFallsBackToOwningVariable() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  // When the declared type of the chain root is unknown anywhere, the hover can
  // still say the access is a member of the owning variable — never a colliding
  // local or the enclosing routine.
  input->append(MakeLspFrame(kDidOpenHovUnknownTypeFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kHoverUnknownTypeMemberFrame));
  std::string const hover = WaitForOutputContaining(output, "\"id\":\"hxu\"");
  Expect(hover.find("\"id\":\"hxu\"") != std::string::npos,
         "member hover request must receive a response");
  Expect(hover.find("Member of `map`.") != std::string::npos,
         "an unknown-type with-implicit member names the with-target");
  Expect(hover.find("Local variable") == std::string::npos,
         "the unknown-type member must not fall back to the colliding local");
  Expect(hover.find("sub run(") == std::string::npos,
         "the unknown-type member must not fall back to the sub signature");

  // A known type with a missing field still names the owning variable and the
  // type, instead of guessing wrong.
  input->append(MakeLspFrame(kDidOpenHovMissingMemberFrame));
  Expect(WaitForPublishedUri(output, 2).empty() == false,
         "second didOpen must publish diagnostics");

  input->append(MakeLspFrame(kHoverMissingMemberFrame));
  std::string const hover2 = WaitForOutputContaining(output, "\"id\":\"hxq\"");
  Expect(hover2.find("Member of `m2` (`Map`).") != std::string::npos,
         "a missing member still names the owning variable and its known type");
  Expect(TailAfter(hover2, "\"id\":\"hxq\"").find("Local variable") ==
             std::string::npos,
         "the missing member must not fall back to a variable");

  session.stop();
}

void TestFoldingRangesReturned() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenHierFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kFoldingRangeFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"fold\"");

  Expect(response.find("\"id\":\"fold\"") != std::string::npos,
         "foldingRange request must receive a response");
  Expect(response.find("\"startLine\":0,\"endLine\":1") != std::string::npos,
         "the SUB block must fold from line 0 up to the END SUB line");
  Expect(
      response.find("\"startLine\":4,\"endLine\":6") != std::string::npos,
      "the FUNCTION block must fold from line 4 up to the END FUNCTION line");

  session.stop();
}

void TestDefinitionResolvesToDeclaration() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenResolveFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kDefinitionFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"def\"");

  Expect(response.find("\"id\":\"def\"") != std::string::npos,
         "definition request must receive a response");
  Expect(response.find("\"start\":{\"line\":0,\"character\":4}") !=
             std::string::npos,
         "definition must jump to the counter declaration name");
  Expect(response.find("\"end\":{\"line\":0,\"character\":11}") !=
             std::string::npos,
         "definition range must cover the full counter name");

  session.stop();
}

void TestReferencesListAllSites() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenResolveFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kReferencesFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"ref\"");

  Expect(response.find("\"id\":\"ref\"") != std::string::npos,
         "references request must receive a response");
  Expect(response.find("\"start\":{\"line\":0,\"character\":4}") !=
             std::string::npos,
         "references must include the declaration site");
  Expect(response.find("\"start\":{\"line\":1,\"character\":0}") !=
             std::string::npos,
         "references must include the first usage");
  Expect(response.find("\"start\":{\"line\":1,\"character\":10}") !=
             std::string::npos,
         "references must include the second usage of counter");

  session.stop();
}

void TestHighlightCoversAllSites() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenResolveFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kHighlightFrame));
  std::string const response = WaitForOutputContaining(output, "\"id\":\"hl\"");

  Expect(response.find("\"id\":\"hl\"") != std::string::npos,
         "documentHighlight request must receive a response");
  std::size_t highlightCount = 0;
  std::size_t pos = 0;
  while ((pos = response.find("\"start\":", pos)) != std::string::npos) {
    ++highlightCount;
    pos += 8;
  }
  Expect(highlightCount == 3,
         "highlight must cover the declaration and both usages");

  session.stop();
}

void TestCompletionOffersKeywordsAndSymbols() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenResolveFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kCompletionFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"comp\"");

  Expect(response.find("\"id\":\"comp\"") != std::string::npos,
         "completion request must receive a response");
  Expect(response.find("\"label\":\"counter\"") != std::string::npos,
         "completion must offer the in-scope counter symbol");
  Expect(response.find("\"label\":\"counter\"") != std::string::npos &&
             response.find("\"kind\":6") != std::string::npos,
         "a dim symbol must complete as a variable");
  Expect(response.find("\"label\":\"dim\"") != std::string::npos,
         "completion must offer the dim keyword");
  Expect(response.find("\"label\":\"end if\"") != std::string::npos,
         "completion must offer END-block snippets");
  Expect(response.find("\"documentation\"") != std::string::npos,
         "keyword completion items must carry documentation");
  Expect(response.find("https://www.freebasic.net/wiki/KeyPgIf") !=
             std::string::npos,
         "keyword documentation must link to the FreeBASIC wiki");

  session.stop();
}

void TestCompletionFiltersMembersByAccessContext() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  // Module level: `p.` offers only the public members.
  input->append(MakeLspFrame(kDidOpenMemberCompletionModuleFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kMemberCompletionModuleFrame));
  std::string const moduleResponse =
      WaitForOutputContaining(output, "\"id\":\"mc1\"");

  Expect(moduleResponse.find("\"label\":\"x\"") != std::string::npos &&
             moduleResponse.find("\"label\":\"y\"") != std::string::npos,
         "`p.` must offer the public members x and y");
  Expect(moduleResponse.find("\"label\":\"hidden\"") == std::string::npos,
         "the private member must not complete at module level");
  Expect(moduleResponse.find("\"label\":\"dim\"") == std::string::npos,
         "member access must not fall back to keywords");
  Expect(moduleResponse.find("\"label\":\"pow\"") == std::string::npos,
         "member access must not offer intrinsic catalog entries");

  // Inside `sub Position.set()`: the same type's private member completes.
  input->append(MakeLspFrame(kDidOpenMemberCompletionProcFrame));
  Expect(WaitForPublishedUri(output, 2).empty() == false,
         "second didOpen must publish diagnostics");

  input->append(MakeLspFrame(kMemberCompletionProcFrame));
  std::string const procResponse =
      WaitForOutputContaining(output, "\"id\":\"mc2\"");

  Expect(
      procResponse.find("\"label\":\"hidden\"") != std::string::npos,
      "the private member must complete inside the owner's member procedure");
  Expect(procResponse.find("\"label\":\"x\"") != std::string::npos,
         "public members still complete inside the member procedure");

  session.stop();
}

void TestHoverLinksKeywordDocs() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenCallsFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kKeywordHoverFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"khh\"");

  Expect(response.find("\"id\":\"khh\"") != std::string::npos,
         "keyword hover request must receive a response");
  Expect(response.find("dim") != std::string::npos,
         "keyword hover must name the keyword");
  Expect(response.find("https://www.freebasic.net/wiki/KeyPgDim") !=
             std::string::npos,
         "keyword hover must link to the FreeBASIC wiki page");

  session.stop();
}

void TestSignatureHelpShowsParamsAndActiveIndex() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenCallsFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kSignatureHelpFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"sig\"");

  Expect(response.find("\"id\":\"sig\"") != std::string::npos,
         "signatureHelp request must receive a response");
  Expect(response.find("function add(a as integer, b as integer) as integer") !=
             std::string::npos,
         "signature help must show the whole function header");
  Expect(response.find("\"label\":\"a\"") != std::string::npos &&
             response.find("\"label\":\"b\"") != std::string::npos,
         "signature help must list each parameter");
  Expect(response.find("\"activeSignature\":0") != std::string::npos,
         "signature help must mark the only signature active");
  Expect(response.find("\"activeParameter\":1") != std::string::npos,
         "signature help must select the second parameter after the comma");

  session.stop();
}

void TestCompletionOffersIntrinsicCatalogItems() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenIntrinsicFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "intrinsic document didOpen must publish diagnostics");

  input->append(MakeLspFrame(kIntrinsicExprLeFrame));
  std::string const le = WaitForOutputContaining(output, "\"id\":\"ile\"");
  Expect(le.find("\"label\":\"Left$\"") != std::string::npos,
         "completion must offer the Left$ intrinsic under the `le` prefix");
  Expect(le.find(
             "\"detail\":\"Left$( str As String, n As Integer ) As String\"") !=
             std::string::npos,
         "the intrinsic item must carry its canonical signature");
  Expect(le.find("https://www.freebasic.net/wiki/KeyPgLeft") !=
             std::string::npos,
         "the intrinsic item must link to its wiki page");
  Expect(le.find("\"label\":\"left\"") == std::string::npos,
         "the bare keyword entry must be replaced by the catalog item, not "
         "duplicated");

  // Statement rows stay out of expression position...
  input->append(MakeLspFrame(kIntrinsicExprPrFrame));
  std::string const exprPr = WaitForOutputContaining(output, "\"id\":\"iep\"");
  Expect(exprPr.find("\"label\":\"Procptr\"") != std::string::npos,
         "function intrinsics must still complete in expression position");
  Expect(exprPr.find("\"label\":\"Print\"") == std::string::npos,
         "statement intrinsics must not complete in an expression");

  // ...and are offered where a statement may start.
  input->append(MakeLspFrame(kIntrinsicStmtPrFrame));
  std::string const stmtPr = WaitForOutputContaining(output, "\"id\":\"isp\"");
  Expect(stmtPr.find("\"label\":\"Print\"") != std::string::npos,
         "statement intrinsics must complete at statement position");
  Expect(stmtPr.find("\"detail\":\"Print [ #filenum, ]") != std::string::npos,
         "the statement item must carry its usage signature");

  session.stop();
}

void TestHoverShowsIntrinsicSignature() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenIntrinsicFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "intrinsic document didOpen must publish diagnostics");

  input->append(MakeLspFrame(kIntrinsicHoverFrame));
  std::string const hov = WaitForOutputContaining(output, "\"id\":\"ihv\"");
  Expect(hov.find("Left$( str As String, n As Integer ) As String") !=
             std::string::npos,
         "hover on Left$ must show the catalog signature");
  Expect(hov.find("https://www.freebasic.net/wiki/KeyPgLeft") !=
             std::string::npos,
         "intrinsic hover must link to the FreeBASIC wiki page");

  session.stop();
}

void TestSignatureHelpResolvesIntrinsic() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenIntrinsicFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "intrinsic document didOpen must publish diagnostics");

  input->append(MakeLspFrame(kIntrinsicSignatureFrame));
  std::string const sig = WaitForOutputContaining(output, "\"id\":\"isg\"");
  Expect(sig.find("Mid$( str As String, start As Integer ) As String") !=
             std::string::npos,
         "signature help must resolve the keyword-lexed Mid$ intrinsic");
  Expect(sig.find("\"label\":\"str\"") != std::string::npos &&
             sig.find("\"label\":\"start\"") != std::string::npos,
         "intrinsic signature help must list the catalog parameters");
  Expect(sig.find("\"activeParameter\":1") != std::string::npos,
         "the comma must advance the active parameter");

  session.stop();
}

void TestDidChangePushesDiagnostics() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must trigger a publish");

  input->append(MakeLspFrame(kDidChangeFrame));
  std::string const after = WaitForPublishedUri(output, 2);
  Expect(!after.empty() && after.size() > 1,
         "didChange must trigger a publish");

  session.stop();
}

void TestDidCloseEvictsAndPublishes() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must trigger a publish");

  input->append(MakeLspFrame(kDidCloseFrame));
  Expect(WaitForPublishedUri(output, 2).empty() == false,
         "didClose must trigger a clearing publish");

  session.stop();
}

void TestShutdownReturnsNullResult() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kInitializeFrame));
  WaitForOutputContaining(output, "\"id\":\"init\"");

  input->append(MakeLspFrame(kShutdownFrame));
  std::string const response = WaitForOutputContaining(output, "\"id\":2");

  Expect(response.find("\"id\":2") != std::string::npos,
         "shutdown response must preserve the id");
  Expect(response.find("\"result\":null") != std::string::npos,
         "shutdown response result must be null");

  session.stop();
}

void TestExitNotifiesSession() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();
  std::atomic<bool> exited{false};

  FreeBasicServer server(session);
  server.setExitHandler(
      [&exited]() { exited.store(true, std::memory_order_relaxed); });
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kExitFrame));
  for (int i = 0; i < 100 && !exited.load(std::memory_order_relaxed); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  Expect(exited.load(std::memory_order_relaxed),
         "exit notification must signal the exit handler");

  session.stop();
}

void TestInitializeServesStaticWatchersToNonDynamicClient() {
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

  Expect(response.find("\"didChangeWatchedFiles\"") != std::string::npos,
         "a non-dynamic client must get the static watcher capability in the "
         "initialize reply");
  Expect(response.find("\"globPattern\":\"**/*.{bas,bi}\"") !=
             std::string::npos,
         "the static watcher must watch the FreeBASIC source globs");
  Expect(response.find("\"kind\":7") != std::string::npos,
         "the static watcher must cover create/change/delete events");

  input->append(MakeLspFrame(kInitializedFrame));
  bool sawRegistration = false;
  for (int i = 0; i < 20; ++i) {
    if (output->snapshot().find("\"method\":\"client/registerCapability\"") !=
        std::string::npos) {
      sawRegistration = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(!sawRegistration, "a non-dynamic client must not receive a "
                           "registerCapability request after initialized");

  session.stop();
}

void TestInitializedRegistersWatchedFilesDynamically() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kInitializeDynamicFrame));
  std::string const init = WaitForOutputContaining(output, "\"id\":\"init\"");
  Expect(init.find("\"didChangeWatchedFiles\"") == std::string::npos,
         "a dynamic client must not be served the static watcher capability");

  input->append(MakeLspFrame(kInitializedFrame));
  std::string const registered =
      WaitForOutputContaining(output, "\"client/registerCapability\"");

  Expect(
      registered.find("\"method\":\"workspace/didChangeWatchedFiles\"") !=
          std::string::npos,
      "the registerCapability request must register the watched-files method");
  Expect(registered.find("\"method\":\"client/registerCapability\"") !=
             std::string::npos,
         "the server must send the registerCapability request to the client");
  Expect(registered.find("\"globPattern\":\"**/*.{bas,bi}\"") !=
             std::string::npos,
         "the registration must watch the FreeBASIC source globs");
  Expect(registered.find("\"kind\":7") != std::string::npos,
         "the registration must cover create/change/delete events");

  std::size_t registrationCount = 0;
  std::size_t pos = 0;
  while ((pos = registered.find("\"method\":\"client/registerCapability\"",
                                pos)) != std::string::npos) {
    ++registrationCount;
    pos += 1;
  }
  Expect(
      registrationCount == 1,
      "a dynamic client must receive exactly one registerCapability request");

  session.stop();
}

void TestEndToEndLifecycle() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();
  std::atomic<bool> exited{false};

  FreeBasicServer server(session);
  server.setExitHandler(
      [&exited]() { exited.store(true, std::memory_order_relaxed); });
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kInitializeFrame));
  std::string const init = WaitForOutputContaining(output, "\"id\":\"init\"");
  Expect(init.find("\"change\":2") != std::string::npos,
         "initialize must advertise incremental sync");

  input->append(MakeLspFrame(kDidOpenFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kShutdownFrame));
  std::string const shutdown = WaitForOutputContaining(output, "\"id\":2");
  Expect(shutdown.find("\"result\":null") != std::string::npos,
         "shutdown response result must be null");

  input->append(MakeLspFrame(kExitFrame));
  for (int i = 0; i < 100 && !exited.load(std::memory_order_relaxed); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(exited.load(std::memory_order_relaxed),
         "exit must signal the exit handler after shutdown");

  session.stop();
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

void TestWorkspaceSymbolIndexesWorkspace() {
  char const *kLibContent =
      "function clamp(v as integer, lo as integer) as integer\n"
      "    if v < lo then return lo\n"
      "    return v\n"
      "end function\n";

  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const wsDir = sandbox / "ws";
  std::filesystem::create_directories(wsDir);
  {
    std::ofstream out(wsDir / "lib.bi");
    out << kLibContent;
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const fileUri = FileUri(wsDir / "lib.bi");
  std::string const rootUri = FileUri(wsDir);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  std::string const init = WaitForOutputContaining(output, "\"id\":\"init\"");
  Expect(init.find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      fileUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString(kLibContent) + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  bool found = false;
  for (int n = 0; n < 60 && !found; ++n) {
    std::string const id = "\"id\":\"ws" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"ws)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":"clamp"}})";
    input->append(MakeLspFrame(request.c_str()));
    std::string const snapshot = WaitForOutputContaining(output, id, 50);
    found = snapshot.find("\"name\":\"clamp\"") != std::string::npos;
  }
  Expect(found, "workspace/symbol must return the clamp function");
  std::string const last =
      WaitForOutputContaining(output, "\"name\":\"clamp\"", 5);
  Expect(last.find(fileUri) != std::string::npos,
         "workspace/symbol location must point into the workspace file");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// Files outside the workspace root must never be indexed: opening one and
// querying workspace/symbol must not surface its symbols.
void TestOutsideFileNotIndexed() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const wsDir = sandbox / "ws";
  std::filesystem::path const outsideDir = sandbox / "outside";
  std::filesystem::create_directories(wsDir);
  std::filesystem::create_directories(outsideDir);
  {
    std::ofstream out(wsDir / "main.bas");
    out << "dim mainVal as integer\n";
    std::ofstream out2(outsideDir / "dep.bi");
    out2 << "sub outsideFunc()\nend sub\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const rootUri = FileUri(wsDir);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  // Open a header living outside the workspace root.
  std::string const outsideUri = FileUri(outsideDir / "dep.bi");
  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      outsideUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub outsideFunc()\nend sub\n") + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"ext" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"ext)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  // Give open+scan time to settle; the outside file's symbol must never
  // appear in workspace/symbol.
  bool sawOutside = false;
  for (int n = 0; n < 40 && !sawOutside; ++n) {
    sawOutside =
        querySymbol(n, "outsideFunc").find("\"name\":\"outsideFunc\"") !=
        std::string::npos;
  }
  Expect(!sawOutside,
         "workspace/symbol must not return symbols from outside the root");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// The reported Kate regression: a document opened from a sibling project
// OUTSIDE the workspace root (the client root is this project, the file lives
// beside it, headers in the sibling's `inc` dir). Hovering a member access
// must resolve through the requesting file's on-demand #include closure — not
// fall back to the enclosing sub — while workspace/symbol stays strictly
// workspace-scoped (the out-of-root sub must not surface).
void TestHoverWorksForDocOutsideWorkspaceRoot() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const wsDir = sandbox / "ws";
  std::filesystem::create_directories(wsDir / ".git"); // project marker
  std::filesystem::path const projDir = sandbox / "proj";
  std::filesystem::create_directories(projDir / "src");
  std::filesystem::create_directories(projDir / "inc");
  {
    std::ofstream out(projDir / "inc" / "world.bi");
    out << "type Wall\n"
           "    as integer v1, v2\n"
           "end type\n"
           "type Map\n"
           "    as Wall walls(10)\n"
           "end type\n";
  }
  char const *kEngineText = "#include once \"world.bi\"\n"
                            "\n"
                            "sub runPhysics(map as Map, secIndex as integer)\n"
                            "    with map\n"
                            "        dim as Wall w = .walls(secIndex)\n"
                            "    end with\n"
                            "end sub\n";
  {
    std::ofstream out2(projDir / "src" / "engine.bas");
    out2 << kEngineText;
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const rootUri = FileUri(wsDir);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  // Open the out-of-root engine.bas and hover `.walls` (line 4, char 26).
  std::string const engineUri = FileUri(projDir / "src" / "engine.bas");
  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      engineUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString(kEngineText) + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  std::string const hoverFrame =
      R"({"jsonrpc":"2.0","id":"hxw","method":"textDocument/hover","params":)"
      R"({"textDocument":{"uri":")" +
      engineUri + R"("},"position":{"line":4,"character":26}}})";
  input->append(MakeLspFrame(hoverFrame.c_str()));
  std::string const hover = WaitForOutputContaining(output, "\"id\":\"hxw\"");
  Expect(hover.find("\"id\":\"hxw\"") != std::string::npos,
         "the out-of-root hover request must receive a response");
  Expect(hover.find("as Wall walls(10)") != std::string::npos,
         "an out-of-root member hover resolves the field declaration");
  Expect(hover.find("Member of `map` (`Map`).") != std::string::npos,
         "the with-implicit member names the variable and its type");
  Expect(hover.find("sub runPhysics(") == std::string::npos,
         "member hover must never fall back to the enclosing sub");

  // The out-of-root sub must stay out of workspace/symbol.
  bool sawOutside = false;
  auto querySymbol = [&](int n) {
    std::string const id = "\"id\":\"ext" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"ext)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":"runPhysics"}})";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };
  for (int n = 0; n < 40 && !sawOutside; ++n) {
    sawOutside =
        querySymbol(n).find("\"name\":\"runPhysics\"") != std::string::npos;
  }
  Expect(!sawOutside,
         "workspace/symbol must not surface symbols from outside the root");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}
// The source-layout counterpart of the VCS broad-root narrowing: the broad
// client root carries no version-control marker anywhere, so the opened
// document's project is recognized by its `src` child instead of a .git
// directory. The index root becomes the project dir (`inner`, from the
// `/tmp/test/inner/src/file.bas` example), so a sibling dir of `src` inside
// the project is indexed too, while an unrelated sibling tree under the broad
// root never is.
void TestSourceLayoutRootNarrowsToOpenedProject() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const broad = sandbox / "broad";
  std::filesystem::path const proj = broad / "inner";
  std::filesystem::path const sibling = broad / "sibling";
  std::filesystem::create_directories(proj / "src");
  std::filesystem::create_directories(proj / "data");
  std::filesystem::create_directories(sibling);
  {
    std::ofstream out(proj / "src" / "app.bas");
    out << "sub wsOnly()\nend sub\n";
    std::ofstream out2(proj / "data" / "helper.bi");
    out2 << "sub helperOnly()\nend sub\n";
    std::ofstream out3(sibling / "other.bas");
    out3 << "sub siblingOnly()\nend sub\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const appUri = FileUri(proj / "src" / "app.bas");
  std::string const broadRootUri = FileUri(broad);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      broadRootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      appUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub wsOnly()\nend sub\n") + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"layout" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"layout)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  // The opened document's project is indexed...
  bool foundWs = false;
  for (int n = 0; n < 60 && !foundWs; ++n) {
    foundWs = querySymbol(n, "wsOnly").find("\"name\":\"wsOnly\"") !=
              std::string::npos;
  }
  Expect(foundWs, "the opened document's project must be indexed");

  // ...with the project dir (`inner`), not the `src` child, as root: a sibling
  // dir inside the project is indexed too.
  bool foundHelper = false;
  for (int n = 0; n < 40 && !foundHelper; ++n) {
    foundHelper =
        querySymbol(100 + n, "helperOnly").find("\"name\":\"helperOnly\"") !=
        std::string::npos;
  }
  Expect(foundHelper,
         "the layout root must be the project dir, not its src child");

  // ...and the sibling tree under the broad root must never be.
  bool sawSibling = false;
  for (int n = 0; n < 40 && !sawSibling; ++n) {
    sawSibling =
        querySymbol(200 + n, "siblingOnly").find("\"name\":\"siblingOnly\"") !=
        std::string::npos;
  }
  Expect(!sawSibling,
         "a sibling project under a broad root must not be indexed");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// The source-layout detection must recognize every catalogued directory name,
// source *and* include, full and abbreviated — not just `src`: each project
// below the broad root is laid out with a different translated name, opening a
// document in it must narrow the index scope to that project, and the marker-
// less sibling tree must stay out.
void TestSourceLayoutRootRecognizesCatalogNames() {
  struct LayoutCase {
    char const *dir;
    char const *symbol;
  };
  static constexpr LayoutCase const cases[] = {
      {"src", "fromSrc"},         {"source", "fromSource"},
      {"fuente", "fromFuente"},   {"zdr", "fromZdr"},
      {"ein", "fromEin"},         {"inc", "fromInc"},
      {"include", "fromInclude"}, {"incl", "fromIncl"},
      {"inkl", "fromInkl"},       {"sumber", "fromSumber"},
  };

  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const broad = sandbox / "broad";
  std::filesystem::path const sibling = broad / "sibling";
  std::filesystem::create_directories(sibling);
  {
    std::ofstream out2(sibling / "other.bas");
    out2 << "sub siblingOnly()\nend sub\n";
  }
  std::vector<std::string> appUris;
  for (std::size_t i = 0; i < std::size(cases); ++i) {
    std::filesystem::path const proj = broad / ("proj" + std::to_string(i));
    std::filesystem::create_directories(proj / cases[i].dir);
    std::ofstream out(proj / cases[i].dir / "app.bas");
    out << "sub " << cases[i].symbol << "()\nend sub\n";
    appUris.push_back(FileUri(proj / cases[i].dir / "app.bas"));
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const broadRootUri = FileUri(broad);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      broadRootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"cat" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"cat)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  int idx = 0;
  bool sawSibling = false;
  for (std::size_t i = 0; i < std::size(cases); ++i) {
    std::string const openFrame =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
        R"({"uri":")" +
        appUris[i] + R"(","languageId":"basic","version":1,"text":")" +
        ToJsonString(std::string("sub ") + cases[i].symbol + "()\nend sub\n") +
        "\"}}}";
    input->append(MakeLspFrame(openFrame.c_str()));

    bool found = false;
    for (int n = 0; n < 60 && !found; ++n) {
      found = querySymbol(idx++, cases[i].symbol)
                  .find(std::string("\"name\":\"") + cases[i].symbol + "\"") !=
              std::string::npos;
    }
    Expect(found, (std::string("project laid out as `") + cases[i].dir +
                   "` must narrow the index scope")
                      .c_str());
    for (int n = 0; !sawSibling && n < 25; ++n) {
      sawSibling =
          querySymbol(idx++, "siblingOnly").find("\"name\":\"siblingOnly\"") !=
          std::string::npos;
    }
  }
  Expect(!sawSibling,
         "a sibling project under a broad root must not be indexed");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// Single-file mode (no client root): the source/include layout walk still
// names the project. Opening `/tmp/test/inner/src/file.bas` roots at
// `/tmp/test/inner`, so a sibling dir of `src` inside the project is indexed
// and a tree beside it is not. Without any layout marker the workspace stays
// the opened file's own directory (pre-existing behavior).
void TestSourceLayoutRootSingleFileMode() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const proj = sandbox / "lone" / "inner";
  std::filesystem::create_directories(proj / "src");
  std::filesystem::create_directories(proj / "data");
  std::filesystem::create_directories(sandbox / "lone" / "step");
  std::filesystem::create_directories(sandbox / "plain");
  std::filesystem::create_directories(sandbox / "park");
  {
    std::ofstream out(proj / "src" / "app.bas");
    out << "sub wsOnly()\nend sub\n";
    std::ofstream out2(proj / "data" / "mod.bas");
    out2 << "sub dataOnly()\nend sub\n";
    std::ofstream out3(sandbox / "lone" / "step" / "x.bas");
    out3 << "sub stepOnly()\nend sub\n";
    std::ofstream out4(sandbox / "plain" / "file.bas");
    out4 << "sub fileOnly()\nend sub\n";
    std::ofstream out5(sandbox / "plain" / "near.bas");
    out5 << "sub nearOnly()\nend sub\n";
    std::ofstream out6(sandbox / "park" / "y.bas");
    out6 << "sub parkOnly()\nend sub\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  // No rootUri: single-file mode.
  input->append(MakeLspFrame(kInitializeFrame));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  std::string const appUri = FileUri(proj / "src" / "app.bas");
  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      appUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub wsOnly()\nend sub\n") + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"single" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"single)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  bool foundWs = false;
  for (int n = 0; n < 60 && !foundWs; ++n) {
    foundWs = querySymbol(n, "wsOnly").find("\"name\":\"wsOnly\"") !=
              std::string::npos;
  }
  Expect(foundWs, "the opened document's project must be indexed");

  bool foundData = false;
  for (int n = 0; n < 40 && !foundData; ++n) {
    foundData =
        querySymbol(100 + n, "dataOnly").find("\"name\":\"dataOnly\"") !=
        std::string::npos;
  }
  Expect(foundData,
         "single-file mode must root at the source-layout project dir");

  bool sawStep = false;
  for (int n = 0; n < 40 && !sawStep; ++n) {
    sawStep = querySymbol(200 + n, "stepOnly").find("\"name\":\"stepOnly\"") !=
              std::string::npos;
  }
  Expect(!sawStep, "a tree beside the layout project must not be indexed");

  session.stop();

  // Without any layout marker the workspace is the file's own directory.
  lsp::NullLog log2;
  lsp::LanguageSession session2(log2);
  auto input2 = std::make_shared<FeedableIStream>();
  auto output2 = std::make_shared<StringOStream>();
  FreeBasicServer server2(session2);
  server2.registerHandlers();
  session2.start(input2, output2);

  input2->append(MakeLspFrame(kInitializeFrame));
  Expect(WaitForOutputContaining(output2, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  std::string const plainUri = FileUri(sandbox / "plain" / "file.bas");
  std::string const openPlain =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      plainUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub fileOnly()\nend sub\n") + "\"}}}";
  input2->append(MakeLspFrame(openPlain.c_str()));

  auto queryPlain = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"plain" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"plain)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input2->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output2, id, 50);
  };

  bool foundNear = false;
  for (int n = 0; n < 60 && !foundNear; ++n) {
    foundNear = queryPlain(n, "nearOnly").find("\"name\":\"nearOnly\"") !=
                std::string::npos;
  }
  Expect(foundNear, "the file's own directory stays the single-file workspace");

  bool sawPark = false;
  for (int n = 0; n < 40 && !sawPark; ++n) {
    sawPark = queryPlain(100 + n, "parkOnly").find("\"name\":\"parkOnly\"") !=
              std::string::npos;
  }
  Expect(!sawPark,
         "the single-file workspace must not widen beyond the file's dir");

  session2.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// The home-folder guard that ends the two unbounded upward walks (the
// single-file marker walk and the source/include layout walk) has to decide "is
// this the home folder" by identity, not by comparing path objects: the two
// spellings come from different places and routinely disagree. Windows reports
// %USERPROFILE% in long form (C:\Users\runneradmin) while %TEMP% — and so every
// document URI built from it — names the same folder 8.3-short
// (C:\Users\RUNNER~1), and a home folder reached through a symlink keeps the
// link in the walk's spelling. Miss the guard and the walk climbs out of the
// home folder and roots the index at the profile or above: every stray .bas
// under it joins the workspace, so one project's symbols surface in another's
// workspace/symbol, and the project's own freebasicd.toml is never read, since
// the settings lookup only looks in the chosen root. That is how the Windows
// leg lost three tests, with %TEMP% handed out short.
//
// Each spelling below names the same directory the walk reaches but cannot be
// matched to it by comparing paths, so the guard has to ask the filesystem:
// `<home>/src/..` needs no privilege and runs everywhere, and the symlink is
// the case a *lexical* normalization cannot answer either — the portable
// stand-in for the 8.3 short name. Skipped with a printed note where the
// platform will not create a symlink, which is a privilege, not a behaviour.
void TestHomeFolderGuardAsksTheFilesystem() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const homeDir = sandbox / "home";
  // Laid out the way the layout walk looks for a project: a `src` child at the
  // top, so the walk finds a root the moment the guard lets it past home.
  std::filesystem::create_directories(homeDir / "src");
  std::filesystem::create_directories(homeDir / "lone");
  {
    std::ofstream out(homeDir / "src" / "stray.bas");
    out << "sub homeStray()\nend sub\n";
  }
  std::vector<std::filesystem::path> spellings{homeDir / "src" / ".."};
  {
    std::error_code ec;
    std::filesystem::create_directory_symlink(homeDir, sandbox / "homelink",
                                              ec);
    if (ec) {
      std::printf("[ NOTE    ] home-folder guard: no symlink spelling (%s)\n",
                  ec.message().c_str());
    } else {
      spellings.push_back(sandbox / "homelink");
    }
  }

  for (std::filesystem::path const &spelling : spellings) {
    ScopedEnv const home("HOME", spelling.string());

    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();
    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    // No rootUri: single-file mode, so the layout walk names the workspace.
    input->append(MakeLspFrame(kInitializeFrame));
    Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                   .find("\"workspaceSymbolProvider\":") != std::string::npos,
           "initialize must advertise workspace/symbol");

    std::string const appUri = FileUri(homeDir / "lone" / "app.bas");
    input->append(MakeLspFrame(
        (R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
         R"({"uri":")" +
         appUri + R"(","languageId":"basic","version":1,"text":")" +
         ToJsonString("sub loneOnly()\nend sub\n") + "\"}}}")
            .c_str()));

    auto querySymbol = [&](char const *tag, int n, std::string const &name) {
      std::string const id =
          std::string("\"id\":\"") + tag + std::to_string(n) + "\"";
      input->append(MakeLspFrame(
          (std::string(R"({"jsonrpc":"2.0","id":")") + tag + std::to_string(n) +
           R"(","method":"workspace/symbol","params":{"query":")" + name +
           R"("}})")
              .c_str()));
      return WaitForOutputContaining(output, id, 50);
    };

    // The file's own directory is the workspace, so its declaration is there...
    bool foundLone = false;
    for (int n = 0; n < 60 && !foundLone; ++n) {
      foundLone =
          querySymbol("lone", n, "loneOnly").find("\"name\":\"loneOnly\"") !=
          std::string::npos;
    }
    Expect(foundLone, "the opened file's own directory stays the workspace");

    // ...and the layout project at the home folder is not, however the guard
    // was spelled.
    bool sawStray = false;
    for (int n = 0; n < 40 && !sawStray; ++n) {
      sawStray =
          querySymbol("stray", n, "homeStray").find("\"name\":\"homeStray\"") !=
          std::string::npos;
    }
    Expect(!sawStray,
           "the home folder's own layout project must stay out of the "
           "workspace of a file below it");

    session.stop();
  }
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// A client root that is itself a single project (has a .git marker) is used
// as-is; a *broad* root (e.g. an editor reporting the home directory, which
// hosts several sibling projects) is narrowed to the opened document's project
// on the first didOpen. Sibling trees under the broad root must never surface.
void TestBroadRootNarrowsToOpenedProject() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const broad = sandbox / "broad";
  std::filesystem::path const proj = broad / "proj";
  std::filesystem::path const sibling = broad / "sibling";
  std::filesystem::create_directories(proj / ".git");
  std::filesystem::create_directories(sibling);
  {
    std::ofstream out(proj / "app.bas");
    out << "sub wsOnly()\nend sub\n";
    std::ofstream out2(sibling / "other.bas");
    out2 << "sub siblingOnly()\nend sub\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const appUri = FileUri(proj / "app.bas");
  std::string const broadRootUri = FileUri(broad);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      broadRootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      appUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub wsOnly()\nend sub\n") + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"narrow" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"narrow)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  // The opened document's project is indexed...
  bool foundWs = false;
  for (int n = 0; n < 60 && !foundWs; ++n) {
    foundWs = querySymbol(n, "wsOnly").find("\"name\":\"wsOnly\"") !=
              std::string::npos;
  }
  Expect(foundWs, "the opened document's project must be indexed");

  // ...and the sibling tree under the broad root must never be.
  bool sawSibling = false;
  for (int n = 0; n < 40 && !sawSibling; ++n) {
    sawSibling =
        querySymbol(100 + n, "siblingOnly").find("\"name\":\"siblingOnly\"") !=
        std::string::npos;
  }
  Expect(!sawSibling,
         "a sibling project under a broad root must not be indexed");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// The broad-root narrowing must recognize every version-control marker the
// finder supports, not just `.git`: mercurial, svn, bazaar, fossil, etc. Each
// project sits under the same broad root; opening a document in it must narrow
// the index scope to that project and leave the sibling tree alone.
void TestBroadRootNarrowsToAnyVcsProject() {
  struct MarkerCase {
    char const *marker;
    char const *symbol;
  };
  static constexpr MarkerCase const cases[] = {
      {".git", "fromGit"},         {".hg", "fromHg"},
      {".svn", "fromSvn"},         {".bzr", "fromBzr"},
      {".fslckout", "fromFossil"}, {"_FOSSIL_", "fromFossilLegacy"},
      {".darcs", "fromDarcs"},     {".pijul", "fromPijul"},
      {"_MTN", "fromMonotone"},
  };

  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const broad = sandbox / "broad";
  std::filesystem::path const sibling = broad / "sibling";
  std::filesystem::create_directories(sibling);
  {
    std::ofstream out2(sibling / "other.bas");
    out2 << "sub siblingOnly()\nend sub\n";
  }
  std::vector<std::string> appUris;
  for (std::size_t i = 0; i < std::size(cases); ++i) {
    std::filesystem::path const proj = broad / ("proj" + std::to_string(i));
    std::filesystem::create_directories(proj / cases[i].marker);
    std::ofstream out(proj / "app.bas");
    out << "sub " << cases[i].symbol << "()\nend sub\n";
    appUris.push_back(FileUri(proj / "app.bas"));
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const broadRootUri = FileUri(broad);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      broadRootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"vcs" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"vcs)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  int idx = 0;
  bool sawSibling = false;
  for (std::size_t i = 0; i < std::size(cases); ++i) {
    std::string const openFrame =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
        R"({"uri":")" +
        appUris[i] + R"(","languageId":"basic","version":1,"text":")" +
        ToJsonString(std::string("sub ") + cases[i].symbol + "()\nend sub\n") +
        "\"}}}";
    input->append(MakeLspFrame(openFrame.c_str()));

    bool found = false;
    for (int n = 0; n < 60 && !found; ++n) {
      found = querySymbol(idx++, cases[i].symbol)
                  .find(std::string("\"name\":\"") + cases[i].symbol + "\"") !=
              std::string::npos;
    }
    Expect(found, (std::string("project marked by ") + cases[i].marker +
                   " must narrow the index scope")
                      .c_str());
    for (int n = 0; !sawSibling && n < 25; ++n) {
      sawSibling =
          querySymbol(idx++, "siblingOnly").find("\"name\":\"siblingOnly\"") !=
          std::string::npos;
    }
  }
  Expect(!sawSibling,
         "a sibling project under a broad root must not be indexed");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
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

void TestWatchedFilesRescanConverges() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::path const wsDir = sandbox / "ws";
  std::filesystem::create_directories(wsDir);
  std::string const lib = "sub greet()\nend sub\n";
  {
    std::ofstream out(wsDir / "lib.bi");
    out << lib;
  }
  // The header stays *closed*, because that is the case where disk is the
  // truth: the rescan converges it. An open buffer is the opposite case -- the
  // client owns those bytes until didClose, and a rescan that re-parsed the
  // disk copy would leave the index's offsets describing a different file than
  // the one a reply is measured against (see
  // TestScanKeepsOpenBufferAheadOfDisk). Opening main.bas is also what a real
  // editor does, and what picks the index root for this non-project client.
  std::string const main = "#include \"lib.bi\"\nsub mainProc()\nend sub\n";
  {
    std::ofstream out(wsDir / "main.bas");
    out << main;
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const fileUri = FileUri(wsDir / "lib.bi");
  std::string const mainUri = FileUri(wsDir / "main.bas");
  std::string const rootUri = FileUri(wsDir);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  // A non-project client root defers index creation to the first opened
  // document, exactly like a real editor always opens one.
  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      mainUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString(main) + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"wat" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"wat)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  // The initial background scan must index the closed header before the edit --
  // and since nothing opens it, only the scan can have read it.
  bool primed = false;
  for (int n = 0; n < 60 && !primed; ++n) {
    primed =
        querySymbol(n, "greet").find("\"name\":\"greet\"") != std::string::npos;
  }
  Expect(primed,
         "workspace/symbol must find the header symbol from the initial scan");

  // A disk edit converges through the watched-files notification: no reopen,
  // no didChange, no restart.
  {
    std::ofstream out(wsDir / "lib.bi");
    out << lib << "sub farewell()\nend sub\n";
  }
  std::string const watchedFrame =
      R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[)"
      R"({"uri":")" +
      fileUri + R"(","type":2}]}})";
  input->append(MakeLspFrame(watchedFrame.c_str()));

  bool converged = false;
  for (int n = 0; n < 100 && !converged; ++n) {
    converged =
        querySymbol(100 + n, "farewell").find("\"name\":\"farewell\"") !=
        std::string::npos;
  }
  Expect(converged, "a watched-files event must converge an external header "
                    "edit into workspace/symbol");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// An open buffer outranks disk, and a cross-file range is measured against the
// buffer -- so a rescan that re-parses the disk copy moves the answer.
//
// The Windows leg is what proved the pairing matters. Every fixture here writes
// through a text-mode ofstream, so MSVC put CRLF on disk under the LF text the
// didOpen carried, and `dim localOnly` came back at 1:5-1:14 instead of
// 1:4-1:13: the index held the disk parse's byte offsets while contentForPath
// still served the buffer, one column out for every line the two copies
// disagreed on. Nothing about the request was wrong and nothing about the index
// was stale -- they were two different files.
//
// So the rescan here is *proven* to have run, or the assertion below is
// vacuous: `converged` exists only in the new disk bytes of a file nothing
// opened, so only a scan can have read them.
void TestScanKeepsOpenBufferAheadOfDisk() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(
      session, server, output, fix,
      {{fix.mainUri, kMainContent}, {fix.libUri, kLibContent}});

  auto definition = [&](std::string const &tag) {
    return PollRequest(input, output, tag, fix.libUri, [&](std::string const &id) {
      return R"({"jsonrpc":"2.0","id":")" + id +
             R"(","method":"textDocument/definition","params":{"textDocument":{"uri":")" +
             fix.mainUri + R"("},"position":{"line":2,"character":22}}})";
    });
  };
  // The header's own columns: `dim localOnly` puts the name at 4..13.
  auto inHeader = [](std::string const &reply) {
    return reply.find("\"start\":{\"line\":1,\"character\":4}") !=
               std::string::npos &&
           reply.find("\"end\":{\"line\":1,\"character\":13}") !=
               std::string::npos;
  };

  std::string const before = definition("sob");
  Expect(inHeader(before),
         ("the header's own columns must be reported before any rescan; "
          "reply was " +
          before.substr(0, 200))
             .c_str());

  // Binary, so these are the exact bytes on every platform: the disk copy
  // diverges from the buffer by CRLF (one column per line after the first),
  // and a closed file gains a symbol.
  {
    std::ofstream out(fix.wsDir / "lib.bi", std::ios::binary);
    for (char c : std::string(kLibContent)) {
      if (c == '\n') {
        out.put('\r');
      }
      out.put(c);
    }
  }
  {
    std::ofstream out(fix.wsDir / "prog.bas", std::ios::binary);
    out << kProgContent << "sub converged()\nend sub\n";
  }
  std::string const watchedFrame =
      R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[)"
      R"({"uri":")" +
      fix.libUri + R"(","type":2},{"uri":")" + fix.progUri +
      R"(","type":2}]}})";
  input->append(MakeLspFrame(watchedFrame.c_str()));

  bool rescanned = false;
  for (int n = 0; n < 100 && !rescanned; ++n) {
    std::string const id = "\"id\":\"sob" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"sob)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":"converged"}})";
    input->append(MakeLspFrame(request.c_str()));
    rescanned = WaitForOutputContaining(output, id, 50)
                    .find("\"name\":\"converged\"") != std::string::npos;
  }
  Expect(rescanned,
         "the rescan must read a closed file's new disk bytes, or the "
         "assertion below proves nothing");

  std::string const after = definition("soa");
  Expect(inHeader(after),
         ("a rescan must not move the header's columns off the open buffer; "
          "reply was " +
          after.substr(0, 200))
             .c_str());

  session.stop();
}

// --- M11: per-workspace indexes ---

// A freebasicd.toml marks its directory as a workspace root (priority-3
// detection). Under a *broad* client root the nearest config file between the
// opened document and the client root names the project; in single-file mode
// the nearest config file above the document does. Sibling trees stay outside
// the index either way.
void TestConfigFileRootDetection() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  // Phase 1: a broad client root; the opened document's project carries the
  // config file.
  std::filesystem::path const broad = sandbox / "broad";
  std::filesystem::path const proj = broad / "proj";
  std::filesystem::path const sibling = broad / "sibling";
  std::filesystem::create_directories(proj / "src");
  std::filesystem::create_directories(proj / "data");
  std::filesystem::create_directories(sibling);
  {
    std::ofstream out(proj / "freebasicd.toml");
    out << "[server]\n";
    std::ofstream out2(proj / "src" / "app.bas");
    out2 << "sub cfgProjOnly()\nend sub\n";
    std::ofstream out3(proj / "data" / "mod.bi");
    out3 << "sub cfgDataOnly()\nend sub\n";
    std::ofstream out4(sibling / "other.bas");
    out4 << "sub cfgSiblingOnly()\nend sub\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const appUri = FileUri(proj / "src" / "app.bas");
  std::string const broadRootUri = FileUri(broad);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      broadRootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      appUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub cfgProjOnly()\nend sub\n") + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"cfg" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"cfg)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  bool foundProj = false;
  for (int n = 0; n < 60 && !foundProj; ++n) {
    foundProj =
        querySymbol(n, "cfgProjOnly").find("\"name\":\"cfgProjOnly\"") !=
        std::string::npos;
  }
  Expect(foundProj, "the config-carrying project must be indexed");

  bool foundData = false;
  for (int n = 0; n < 40 && !foundData; ++n) {
    foundData =
        querySymbol(100 + n, "cfgDataOnly").find("\"name\":\"cfgDataOnly\"") !=
        std::string::npos;
  }
  Expect(foundData,
         "the config root must be the project dir, so a sibling dir of `src` "
         "inside it is indexed too");

  bool sawSibling = false;
  for (int n = 0; n < 40 && !sawSibling; ++n) {
    sawSibling = querySymbol(200 + n, "cfgSiblingOnly")
                     .find("\"name\":\"cfgSiblingOnly\"") != std::string::npos;
  }
  Expect(!sawSibling,
         "a sibling tree under the broad root must not be indexed");

  session.stop();

  // Phase 2: single-file mode (no client root): the nearest config file above
  // the document names the project.
  std::filesystem::path const lone = sandbox / "lone";
  std::filesystem::path const lp = lone / "proj";
  std::filesystem::create_directories(lp / "src");
  std::filesystem::create_directories(lp / "data");
  std::filesystem::create_directories(lone / "step");
  {
    std::ofstream out(lp / "freebasicd.toml");
    out << "[server]\n";
    std::ofstream out2(lp / "src" / "app.bas");
    out2 << "sub cfgSfProjOnly()\nend sub\n";
    std::ofstream out3(lp / "data" / "mod.bas");
    out3 << "sub cfgSfDataOnly()\nend sub\n";
    std::ofstream out4(lone / "step" / "x.bas");
    out4 << "sub cfgSfStepOnly()\nend sub\n";
  }

  lsp::NullLog log2;
  lsp::LanguageSession session2(log2);
  auto input2 = std::make_shared<FeedableIStream>();
  auto output2 = std::make_shared<StringOStream>();
  FreeBasicServer server2(session2);
  server2.registerHandlers();
  session2.start(input2, output2);

  input2->append(MakeLspFrame(kInitializeFrame));
  Expect(WaitForOutputContaining(output2, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  std::string const sfUri = FileUri(lp / "src" / "app.bas");
  std::string const openSf =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      sfUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub cfgSfProjOnly()\nend sub\n") + "\"}}}";
  input2->append(MakeLspFrame(openSf.c_str()));

  auto querySf = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"cfs" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"cfs)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input2->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output2, id, 50);
  };

  bool foundSfProj = false;
  for (int n = 0; n < 60 && !foundSfProj; ++n) {
    foundSfProj =
        querySf(n, "cfgSfProjOnly").find("\"name\":\"cfgSfProjOnly\"") !=
        std::string::npos;
  }
  Expect(foundSfProj,
         "single-file mode must root at the config-carrying project");

  bool foundSfData = false;
  for (int n = 0; n < 40 && !foundSfData; ++n) {
    foundSfData =
        querySf(100 + n, "cfgSfDataOnly").find("\"name\":\"cfgSfDataOnly\"") !=
        std::string::npos;
  }
  Expect(foundSfData, "single-file mode must index the config project dir");

  bool sawStep = false;
  for (int n = 0; n < 40 && !sawStep; ++n) {
    sawStep =
        querySf(200 + n, "cfgSfStepOnly").find("\"name\":\"cfgSfStepOnly\"") !=
        std::string::npos;
  }
  Expect(!sawStep, "a tree beside the config project must not be indexed");

  session2.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// A client root that bears a freebasicd.toml (rootUri or a registered
// workspace folder) is *itself* a workspace root: it is used as-is and indexed
// eagerly at initialize, so workspace/symbol serves its symbols before any
// document is opened. A config-carrying registered folder joins the priority-0
// registered-root set the same way.
void TestConfigMarkerRootUsedAsIsEagerIndex() {
  static std::atomic<long> counter{0};
  std::filesystem::path const sandbox =
      std::filesystem::temp_directory_path() /
      ("fblsp-session-" + std::to_string(::time(nullptr)) + "-" +
       std::to_string(counter.fetch_add(1)));
  // Phase 1: rootUri points at a config-carrying directory.
  std::filesystem::path const proj = sandbox / "proj";
  std::filesystem::create_directories(proj / "sub");
  std::filesystem::create_directories(sandbox / "elsewhere");
  {
    std::ofstream out(proj / "freebasicd.toml");
    out << "[server]\n";
    std::ofstream out2(proj / "sub" / "app.bas");
    out2 << "sub eagerOnly()\nend sub\n";
    std::ofstream out3(sandbox / "elsewhere" / "x.bas");
    out3 << "sub elsewhereOnly()\nend sub\n";
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const rootUri = FileUri(proj);
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"cmk" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"cmk)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  // Eager: no didOpen has been sent; the initialize-period scan must have
  // indexed the config-carrying root already.
  bool foundEager = false;
  for (int n = 0; n < 60 && !foundEager; ++n) {
    foundEager = querySymbol(n, "eagerOnly").find("\"name\":\"eagerOnly\"") !=
                 std::string::npos;
  }
  Expect(foundEager,
         "a config-carrying client root must be indexed at initialize");

  bool sawElsewhere = false;
  for (int n = 0; n < 40 && !sawElsewhere; ++n) {
    sawElsewhere = querySymbol(100 + n, "elsewhereOnly")
                       .find("\"name\":\"elsewhereOnly\"") != std::string::npos;
  }
  Expect(!sawElsewhere,
         "the as-is root must never reach beyond the config directory");

  session.stop();

  // Phase 2: a registered workspace folder with a config file is indexed
  // eagerly too, from the initialize reply's folder list alone.
  std::filesystem::path const fa = sandbox / "fa";
  std::filesystem::create_directories(fa);
  {
    std::ofstream out(fa / "freebasicd.toml");
    out << "[server]\n";
    std::ofstream out2(fa / "a.bas");
    out2 << "sub folderCfgOnly()\nend sub\n";
  }

  lsp::NullLog log2;
  lsp::LanguageSession session2(log2);
  auto input2 = std::make_shared<FeedableIStream>();
  auto output2 = std::make_shared<StringOStream>();
  FreeBasicServer server2(session2);
  server2.registerHandlers();
  session2.start(input2, output2);

  std::string const faUri = FileUri(fa);
  std::string const init2 =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"workspaceFolders":[)"
      R"({"uri":")" +
      faUri + R"(","name":"fa"}]}})";
  input2->append(MakeLspFrame(init2.c_str()));
  Expect(WaitForOutputContaining(output2, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  auto queryFolder = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"cmf" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"cmf)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input2->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output2, id, 50);
  };

  bool foundFolder = false;
  for (int n = 0; n < 60 && !foundFolder; ++n) {
    foundFolder =
        queryFolder(n, "folderCfgOnly").find("\"name\":\"folderCfgOnly\"") !=
        std::string::npos;
  }
  Expect(foundFolder,
         "a config-carrying workspace folder must be indexed at initialize");

  session2.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// One index per registered workspace folder. workspace/symbol aggregates every
// live index, but a document opened in one folder is served strictly by its
// own folder's index: sibling-folder module roots must never leak into its
// completion closure.
void TestMultiWorkspaceFoldersStayIsolated() {
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
    std::ofstream out(fa / "freebasicd.toml");
    out << "[server]\n";
    std::ofstream out2(fa / "a.bas");
    out2 << "sub alphaOnly()\nend sub\n";
    std::ofstream out3(fb / "freebasicd.toml");
    out3 << "[server]\n";
    std::ofstream out4(fb / "b.bas");
    out4 << "sub betaOnly()\nend sub\n";
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
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"workspaceFolders":[)"
      R"({"uri":")" +
      faUri + R"(","name":"fa"},{"uri":")" + fbUri + R"(","name":"fb"}]}})";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"mul" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"mul)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  // Aggregation: both folder roots are eager (config markers) and both must
  // contribute to workspace/symbol with no didOpen at all.
  bool foundAlpha = false;
  for (int n = 0; n < 60 && !foundAlpha; ++n) {
    foundAlpha = querySymbol(n, "alphaOnly").find("\"name\":\"alphaOnly\"") !=
                 std::string::npos;
  }
  Expect(foundAlpha, "workspace/symbol must aggregate folder A's symbols");
  bool foundBeta = false;
  for (int n = 0; n < 60 && !foundBeta; ++n) {
    foundBeta =
        querySymbol(100 + n, "betaOnly").find("\"name\":\"betaOnly\"") !=
        std::string::npos;
  }
  Expect(foundBeta, "workspace/symbol must aggregate folder B's symbols");

  // Isolation: opening B's document and completing at module level must offer
  // B's own module root but never A's (B is served by B's index alone).
  std::string const bUri = FileUri(fb / "b.bas");
  std::string const openFrame =
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
      R"({"uri":")" +
      bUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString("sub betaOnly()\nend sub\n") + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  std::string completion;
  for (int n = 0; n < 40; ++n) {
    std::string const id = "\"id\":\"mulc" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"mulc)" + std::to_string(n) +
        R"(","method":"textDocument/completion","params":{"textDocument":{"uri":")" +
        bUri + R"("},"position":{"line":2,"character":0}}})";
    input->append(MakeLspFrame(request.c_str()));
    std::string const snapshot = WaitForOutputContaining(output, id, 50);
    if (snapshot.find("\"label\":\"betaOnly\"") != std::string::npos) {
      // Clip to this reply: `alphaOnly` must be judged against B's own
      // completion only.
      completion = snapshot.substr(snapshot.rfind(id));
      break;
    }
    completion = snapshot;
  }
  Expect(completion.find("\"label\":\"betaOnly\"") != std::string::npos,
         "completion in folder B must offer B's own module root");
  Expect(completion.find("\"label\":\"alphaOnly\"") == std::string::npos,
         "completion in folder B must never offer folder A's module root");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
}

// workspace/didChangeWorkspaceFolders adds and removes indexes live: an added
// config-carrying folder is indexed immediately; removing it closes its index
// and its symbols leave workspace/symbol — unless the session root or another
// registered folder still needs it, in which case it survives. The initialize
// reply advertises folder support and change notifications.
void TestWorkspaceFoldersChangedAddRemove() {
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
    std::ofstream out(fa / "freebasicd.toml");
    out << "[server]\n";
    std::ofstream out2(fa / "a.bas");
    out2 << "sub addOnly()\nend sub\n";
    std::ofstream out3(fb / "freebasicd.toml");
    out3 << "[server]\n";
    std::ofstream out4(fb / "b.bas");
    out4 << "sub addedOnly()\nend sub\n";
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
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"workspaceFolders":[)"
      R"({"uri":")" +
      faUri + R"(","name":"fa"}]}})";
  input->append(MakeLspFrame(initFrame.c_str()));
  std::string const init = WaitForOutputContaining(output, "\"id\":\"init\"");
  Expect(init.find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");
  Expect(init.find("\"workspaceFolders\":{\"supported\":true,"
                   "\"changeNotifications\":true}") != std::string::npos,
         "initialize must advertise folder support and change notifications");

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"chg" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"chg)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    const std::string snapshot = WaitForOutputContaining(output, id, 50);
    // Tail from this reply's id, so a stale mention in an earlier reply (this
    // session queries the same names again) never trips an assertion.
    return snapshot.substr(snapshot.rfind(id));
  };

  bool foundAdd = false;
  for (int n = 0; n < 60 && !foundAdd; ++n) {
    foundAdd = querySymbol(n, "addOnly").find("\"name\":\"addOnly\"") !=
               std::string::npos;
  }
  Expect(foundAdd, "the registered folder must be indexed at initialize");

  // Add fb at runtime: its symbols must appear without any didOpen.
  std::string const addFrame =
      R"({"jsonrpc":"2.0","method":"workspace/didChangeWorkspaceFolders","params":{"event":{"added":[)"
      R"({"uri":")" +
      fbUri + R"(","name":"fb"}],"removed":[]}}})";
  input->append(MakeLspFrame(addFrame.c_str()));

  bool foundAdded = false;
  for (int n = 0; n < 60 && !foundAdded; ++n) {
    foundAdded =
        querySymbol(100 + n, "addedOnly").find("\"name\":\"addedOnly\"") !=
        std::string::npos;
  }
  Expect(foundAdded, "an added config-carrying folder must be indexed at once");

  // Remove fb: its index closes and its symbols leave workspace/symbol, while
  // fa keeps serving.
  std::string const removeFrame =
      R"({"jsonrpc":"2.0","method":"workspace/didChangeWorkspaceFolders","params":{"event":{"added":[],)"
      R"("removed":[{"uri":")" +
      fbUri + R"(","name":"fb"}]}}})";
  input->append(MakeLspFrame(removeFrame.c_str()));
  // The removal runs on the notification FIFO thread; let it land before the
  // negative poll so a straggling pre-removal reply cannot trip the assertion.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  bool sawGone = false;
  for (int n = 0; n < 40 && !sawGone; ++n) {
    sawGone =
        querySymbol(200 + n, "addedOnly").find("\"name\":\"addedOnly\"") !=
        std::string::npos;
  }
  Expect(!sawGone, "removing a folder must drop its symbols from "
                   "workspace/symbol");

  bool stillAdd = false;
  for (int n = 0; n < 20 && !stillAdd; ++n) {
    stillAdd = querySymbol(300 + n, "addOnly").find("\"name\":\"addOnly\"") !=
               std::string::npos;
  }
  Expect(stillAdd, "an unaffected registered folder must keep serving");

  session.stop();
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
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

// --- M8: prepareRename + rename ---

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
  Expect(first.find("\"code\":\"stray-closer\"") != std::string::npos,
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
  Expect(restoring.find("\"code\":\"stray-closer\"") != std::string::npos,
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
  Expect(last.find("\"code\":\"stray-closer\"") != std::string::npos,
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

// M10: repeat requests against an unchanged open buffer must be served from
// the content-addressed analysis cache — the cache's miss counter must not
// move, and its hit counter must. This is the "no reparse observable" gate.
void TestRepeatRequestsShareAnalysis() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenResolveFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");
  fblang::AnalysisCache::Stats const afterOpen = server.analysisStats();

  auto completionFrame = [](std::string const &id) {
    return R"({"jsonrpc":"2.0","id":")" + id +
           R"(","method":"textDocument/completion","params":{"textDocument":{"uri":"file://{{tmp}}/resolve.bas"},"position":{"line":1,"character":8}}})";
  };

  input->append(MakeLspFrame(completionFrame("c1").c_str()));
  std::string const first = WaitForOutputContaining(output, "\"id\":\"c1\"");
  Expect(first.find("\"label\":\"counter\"") != std::string::npos,
         "the first completion must offer the in-scope symbol");
  fblang::AnalysisCache::Stats const afterFirst = server.analysisStats();

  input->append(MakeLspFrame(completionFrame("c2").c_str()));
  std::string const second = WaitForOutputContaining(output, "\"id\":\"c2\"");
  Expect(second.find("\"label\":\"counter\"") != std::string::npos,
         "the repeat completion must reply identically");
  fblang::AnalysisCache::Stats const afterSecond = server.analysisStats();

  Expect(afterFirst.misses == afterOpen.misses,
         "the first request on an open buffer must hit the didOpen analysis");
  Expect(afterSecond.misses == afterFirst.misses,
         "a repeat request must not re-analyze the unchanged buffer");
  Expect(afterSecond.hits > afterFirst.hits,
         "the repeat request must be served from the cache");

  session.stop();
}

// M10: a didChange with no `version` field still leaves the WorkingFile version
// unchanged; a version-keyed cache would serve the pre-change parse. The
// content-addressed key must re-analyze the new bytes and serve fresh symbols.
void TestVersionlessChangeServesFreshAnalysis() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenResolveFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");
  fblang::AnalysisCache::Stats const afterOpen = server.analysisStats();

  // Full-document replacement, no "version" key on the textDocument.
  input->append(MakeLspFrame(
      R"FB({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file://{{tmp}}/resolve.bas"},"contentChanges":[{"range":{"start":{"line":0,"character":0},"end":{"line":1,"character":21}},"text":"dim total as integer\ntotal = total + 1\n"}]}})FB"));
  Expect(WaitForPublishedUri(output, 2).empty() == false,
         "the versionless change must publish diagnostics");

  input->append(MakeLspFrame(
      R"({"jsonrpc":"2.0","id":"ds2","method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file://{{tmp}}/resolve.bas"}}})"));
  std::string const syms = WaitForOutputContaining(output, "\"id\":\"ds2\"");
  Expect(syms.find("\"name\":\"total\"") != std::string::npos,
         "the versionless change's new declaration must be visible");
  Expect(syms.find("\"name\":\"counter\"") == std::string::npos,
         "the stale pre-change parse must never be served");

  fblang::AnalysisCache::Stats const afterChange = server.analysisStats();
  Expect(afterChange.misses == afterOpen.misses + 1,
         "changed bytes must force exactly one re-analysis, version or not");

  session.stop();
}

// M10: the cross-file closure tax is gone — a references pass over the
// requesting buffer plus a closed closure file warms the cache on first use
// (one disk-read miss), and a repeat pass re-analyzes nothing.
void TestReferencesClosureReusesAnalysis() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  // Only main.bas is open; lib.bi stays closed and is read from disk by the
  // closure provider.
  auto input = StartIndexedSession(session, server, output, fix,
                                   {{fix.mainUri, kMainContent}});

  auto refsFrame = [&](std::string const &id) {
    return R"({"jsonrpc":"2.0","id":")" + id +
           R"(","method":"textDocument/references","params":{"textDocument":{"uri":")" +
           fix.mainUri +
           R"("},"position":{"line":2,"character":7},"context":{"includeDeclaration":true}}})";
  };

  std::string const first = PollRequest(
      input, output, "rf1", "\"start\":{\"line\":0,\"character\":11}",
      [&](std::string const &id) { return refsFrame(id); });
  Expect(first.find("\"start\":{\"line\":0,\"character\":11}") !=
             std::string::npos,
         "the references pass must reach the header declaration");
  fblang::AnalysisCache::Stats const afterFirst = server.analysisStats();

  std::string const second = PollRequest(
      input, output, "rf2", "\"start\":{\"line\":0,\"character\":11}",
      [&](std::string const &id) { return refsFrame(id); });
  Expect(second.find("\"start\":{\"line\":0,\"character\":11}") !=
             std::string::npos,
         "the repeat references pass must reply identically");
  fblang::AnalysisCache::Stats const afterSecond = server.analysisStats();

  Expect(afterSecond.misses == afterFirst.misses,
         "a repeat references pass must reuse the closure analyses");
  Expect(afterSecond.hits > afterFirst.hits,
         "the repeat references pass must count cache hits");

  session.stop();
}

// RUN_TEST is silent when a test passes, so a hang or a crash on a platform we
// cannot reproduce locally leaves ctest's captured output with nothing in it —
// the Windows leg once sat on session_integration for eleven minutes and the
// log said only that test 14 had started. Print the name first and flush it:
// ctest prints what a test emitted when it fails *or times out*, so the last
// line printed is then the answer. Redefined here rather than in LspCpp's
// test_helpers.h, which is vendored and not ours to change.
//
// The closing `[ DONE ]` and the catch are the other half of that: a run that
// stops after a test's last `[ RUN ]` cannot say whether the test finished or
// the process died inside it, and a test that throws would take the remaining
// seventy with it. See "Naming a failure that has no message" above.
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

int main(int argc, char **argv) {
  test::InitTestFilter(argc, argv);
  std::set_terminate(ReportUncaught);
  RUN_TEST(TestInitializeReportsSyncCapabilities);
  RUN_TEST(TestDidOpenPublishesDiagnostics);
  RUN_TEST(TestDiagnosticsReflectParseErrors);
  RUN_TEST(TestDiagnosticsRespectDeclarationScopes);
  RUN_TEST(TestDocumentSymbolsReturnHierarchy);
  RUN_TEST(TestHoverShowsSignatureAndDoc);
  RUN_TEST(TestHoverResolvesUsageToDeclaration);
  RUN_TEST(TestHoverShowsMemberAccess);
  RUN_TEST(TestHoverShowsEnumMembers);
  RUN_TEST(TestKeywordHoverInsideProcedureShowsWikiLink);
  RUN_TEST(TestMemberHoverResolvesCrossFileTypeOutsideClosure);
  RUN_TEST(TestMemberHoverKeywordNamedMember);
  RUN_TEST(TestMemberHoverFallsBackToOwningVariable);
  RUN_TEST(TestFoldingRangesReturned);
  RUN_TEST(TestDefinitionResolvesToDeclaration);
  RUN_TEST(TestReferencesListAllSites);
  RUN_TEST(TestHighlightCoversAllSites);
  RUN_TEST(TestCompletionOffersKeywordsAndSymbols);
  RUN_TEST(TestCompletionFiltersMembersByAccessContext);
  RUN_TEST(TestHoverLinksKeywordDocs);
  RUN_TEST(TestSignatureHelpShowsParamsAndActiveIndex);
  RUN_TEST(TestCompletionOffersIntrinsicCatalogItems);
  RUN_TEST(TestHoverShowsIntrinsicSignature);
  RUN_TEST(TestSignatureHelpResolvesIntrinsic);
  RUN_TEST(TestWorkspaceSymbolIndexesWorkspace);
  RUN_TEST(TestOutsideFileNotIndexed);
  RUN_TEST(TestHoverWorksForDocOutsideWorkspaceRoot);
  RUN_TEST(TestBroadRootNarrowsToOpenedProject);
  RUN_TEST(TestBroadRootNarrowsToAnyVcsProject);
  RUN_TEST(TestSourceLayoutRootNarrowsToOpenedProject);
  RUN_TEST(TestSourceLayoutRootRecognizesCatalogNames);
  RUN_TEST(TestSourceLayoutRootSingleFileMode);
  RUN_TEST(TestHomeFolderGuardAsksTheFilesystem);
  RUN_TEST(TestDidChangePushesDiagnostics);
  RUN_TEST(TestDidCloseEvictsAndPublishes);
  RUN_TEST(TestShutdownReturnsNullResult);
  RUN_TEST(TestInitializeServesStaticWatchersToNonDynamicClient);
  RUN_TEST(TestInitializedRegistersWatchedFilesDynamically);
  RUN_TEST(TestMissingIncludePublishesDiagnostic);
  RUN_TEST(TestCodeActionInsertsMissingCloser);
  RUN_TEST(TestCodeActionRetargetsMissingInclude);
  RUN_TEST(TestCodeActionOffersNothingUnfixable);
  RUN_TEST(TestWatchedFilesRescanConverges);
  RUN_TEST(TestScanKeepsOpenBufferAheadOfDisk);
  RUN_TEST(TestConfigFileRootDetection);
  RUN_TEST(TestConfigMarkerRootUsedAsIsEagerIndex);
  RUN_TEST(TestMultiWorkspaceFoldersStayIsolated);
  RUN_TEST(TestWorkspaceFoldersChangedAddRemove);
  RUN_TEST(TestExitNotifiesSession);
  RUN_TEST(TestEndToEndLifecycle);
  RUN_TEST(TestCrossFileDefinitionReferencesHighlight);
  RUN_TEST(TestCrossFileStorageGate);
  RUN_TEST(TestCrossFileCompletionHonorsGate);
  RUN_TEST(TestCrossFileLenientByKeyFallback);
  RUN_TEST(TestPrepareRenameReturnsRangeAndPlaceholder);
  RUN_TEST(TestPrepareRenameOnKeywordReturnsNull);
  RUN_TEST(TestRenameCrossFileRewritesBothFiles);
  RUN_TEST(TestRenameLocalOnlyStaysInFile);
  RUN_TEST(TestRenameRejectsInvalidName);
  RUN_TEST(TestRenameRejectsCollision);
  RUN_TEST(TestSemanticTokensFullThenDelta);
  RUN_TEST(TestSemanticTokensCapabilities);
  RUN_TEST(TestSemanticTokensRangeAndFullOnlyCache);
  RUN_TEST(TestInlayHintsReturned);
  RUN_TEST(TestDidChangeConfigurationIncludePathResolves);
  RUN_TEST(TestDidChangeConfigurationDiagnosticsToggle);
  RUN_TEST(TestSemanticTokensAndInlayHintsGates);
  RUN_TEST(TestSettingsApplyPerRootOnly);
  RUN_TEST(TestRepeatRequestsShareAnalysis);
  RUN_TEST(TestVersionlessChangeServesFreshAnalysis);
  RUN_TEST(TestReferencesClosureReusesAnalysis);
  // The last line of a healthy run. If it is missing, main never got here, and
  // the last `[ DONE ]` names the test the process died in; if it is present,
  // the death was after the suite — during teardown, static destruction, or a
  // thread that outlived it. Those are different bugs with different fixes, and
  // a log that cannot tell them apart costs a CI run per guess.
  std::printf("[ DONE  ] all tests ran: %d failure(s), %d skipped\n",
              test::Failures(), test::SkippedTests());
  std::fflush(stdout);
  return test::Failures() == 0 ? 0 : 1;
}
