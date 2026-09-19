#include "LibLsp/LspCpp.h"
#include "src/session.h"
#include "test_helpers.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using test::Expect;
using test::FeedableIStream;
using test::MakeLspFrame;
using test::StringOStream;
using test::WaitForOutputContaining;

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
    libUri = "file://" + (wsDir / "lib.bi").string();
    mainUri = "file://" + (wsDir / "main.bas").string();
    progUri = "file://" + (wsDir / "prog.bas").string();
    extraUri = "file://" + (wsDir / "extra.bi").string();
    rootUri = "file://" + wsDir.string();
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
std::string
PollRequest(std::shared_ptr<FeedableIStream> const &input,
            std::shared_ptr<StringOStream> const &output,
            std::string const &prefix, std::string const &needle,
            std::function<std::string(std::string const &)> const &frame,
            int attempts = 40) {
  std::string last;
  for (int n = 0; n < attempts; ++n) {
    std::string const id = "\"id\":\"" + prefix + std::to_string(n) + "\"";
    input->append(MakeLspFrame(frame(prefix + std::to_string(n)).c_str()));
    std::string const snapshot = WaitForOutputContaining(output, id, 50);
    if (snapshot.find(needle) != std::string::npos) {
      // Clip to the tail of the stream so callers can make negative
      // assertions without seeing earlier replies of the same session.
      return snapshot.substr(snapshot.rfind(id));
    }
    last = snapshot;
  }
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

char const *kUri = "file:///tmp/hello.bas";

char const kInitializeFrame[] =
    R"FB({"jsonrpc":"2.0","id":"init","method":"initialize","params":{}})FB";

char const kDidOpenFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/hello.bas","languageId":"basic","version":1,"text":"print \"hello\"\n"}}})FB";

char const kDidChangeFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/hello.bas","version":2},"contentChanges":[{"range":)FB"
    R"FB({"start":{"line":1,"character":0},"end":{"line":1,"character":0}},"text":"' comment\n"}]}})FB";

char const kDidOpenDupFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/hello.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"dim x as integer\ndim x as string"}}})FB";

// BUGS.md reuse example: a block-local `dim x` shadows the module `dim x`
// (valid FreeBASIC), so must not be reported as a duplicate definition.
char const kDidOpenScopeDupFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/hello.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"dim x as integer = 1\nscope\n    dim x as string = \"Hello\"\nend scope\nx = x + 1\n"}}})FB";

char const kDidOpenHierFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/hello.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"sub greet(name as string)\n    print name\nend sub\n\n)FB"
    R"FB(function clamp(v as integer, lo as integer, hi as integer) as integer\n)FB"
    R"FB(    if v < lo then return lo\n    if v > hi then return hi\nend function\n"}}})FB";

char const *kDocumentSymbolFrame =
    R"FB({"jsonrpc":"2.0","id":"dsym","method":"textDocument/documentSymbol","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/hello.bas"}}})FB";

// Line 4 is the `function clamp(...)` header; the cursor sits on that line.
char const *kHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"hov","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/hello.bas"},"position":{"line":4,"character":1}}})FB";

char const *kHoverOnBodyFrame =
    R"FB({"jsonrpc":"2.0","id":"hov2","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/hello.bas"},"position":{"line":5,"character":4}}})FB";

// A WITH + FOR + IF document mirroring drd/temp/src/engine.bas: a block-local
// `v1` used inside a `type(...)` initializer. Hovering the *usage* must show
// v1's declaration, not the enclosing block.
char const kDidOpenHoverUsageFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/hovuse.bas","languageId":"basic","version":1,)FB"
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
    R"FB({"textDocument":{"uri":"file:///tmp/hovuse.bas"},"position":{"line":5,"character":40}}})FB";

// Hover the `v1` declaration itself (line 3, char 27).
char const *kHoverDeclFrame =
    R"FB({"jsonrpc":"2.0","id":"hvd","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/hovuse.bas"},"position":{"line":3,"character":27}}})FB";

// Hover the loop counter `i` in the for header (line 2, char 12).
char const *kHoverCounterFrame =
    R"FB({"jsonrpc":"2.0","id":"hvc","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/hovuse.bas"},"position":{"line":2,"character":12}}})FB";

// Hover a module-level Dim usage (resolve.bas line 1, char 0 = `counter`).
char const *kModuleDimHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"hvm","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/resolve.bas"},"position":{"line":1,"character":0}}})FB";

// Member-access hover (the reported regression): a WITH + inline UDT
// document mirroring drd/temp's world.bi/engine.bas shape. Hovering
// `.walls`, `.sectors(i).floorHeight` or `w.v1` must show the *field*
// declaration and its owning variable/type — never the enclosing sub.
char const kDidOpenMemberHoverFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/hovmem.bas","languageId":"basic","version":1,)FB"
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
    R"FB({"textDocument":{"uri":"file:///tmp/hovmem.bas"},"position":{"line":16,"character":26}}})FB";

// Hover `floorHeight` through the indexed chain (line 17, char 50).
char const *kMemberHoverFloorFrame =
    R"FB({"jsonrpc":"2.0","id":"hmf","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/hovmem.bas"},"position":{"line":17,"character":50}}})FB";

// Hover `v1` of the plain local variable (line 18, char 29).
char const *kMemberHoverLocalFrame =
    R"FB({"jsonrpc":"2.0","id":"hml","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/hovmem.bas"},"position":{"line":18,"character":29}}})FB";

char const *kFoldingRangeFrame =
    R"FB({"jsonrpc":"2.0","id":"fold","method":"textDocument/foldingRange","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/hello.bas"}}})FB";

// A second document exercising identifier resolution: module dim, a usage,
// and a sub with a shadowing local dim.
char const kDidOpenResolveFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/resolve.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"dim counter as integer\ncounter = counter + 1\n"}}})FB";

char const *kDefinitionFrame =
    R"FB({"jsonrpc":"2.0","id":"def","method":"textDocument/definition","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/resolve.bas"},"position":{"line":1,"character":0}}})FB";

char const *kReferencesFrame =
    R"FB({"jsonrpc":"2.0","id":"ref","method":"textDocument/references","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/resolve.bas"},"position":{"line":1,"character":0},)FB"
    R"FB("context":{"includeDeclaration":true}}})FB";

char const *kHighlightFrame =
    R"FB({"jsonrpc":"2.0","id":"hl","method":"textDocument/documentHighlight","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/resolve.bas"},"position":{"line":1,"character":0}}})FB";

// A module with a function and a call to get signature help inside the call.
char const kDidOpenCallsFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/calls.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"function add(a as integer, b as integer) as integer\n    return a + b\nend function\n\n)FB"
    R"FB(dim x as integer\nx = add(1, \n"}}})FB";

char const *kCompletionFrame =
    R"FB({"jsonrpc":"2.0","id":"comp","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/resolve.bas"},"position":{"line":1,"character":8}}})FB";

char const *kSignatureHelpFrame =
    R"FB({"jsonrpc":"2.0","id":"sig","method":"textDocument/signatureHelp","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/calls.bas"},"position":{"line":5,"character":11}}})FB";

char const *kKeywordHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"khh","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/calls.bas"},"position":{"line":4,"character":0}}})FB";

// Intrinsic catalog document: expression-prefix positions (`s = le`, `s = pr`),
// a statement-position prefix (`pr`), an intrinsic call for signature help, and
// a `$`-suffixed hover target.
char const kDidOpenIntrinsicFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/intr.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"dim s as string\ns = le\ns = pr\npr\ns = mid$( \"abcdef\", 2 )\ns = left$\n"}}})FB";

char const *kIntrinsicExprLeFrame =
    R"FB({"jsonrpc":"2.0","id":"ile","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/intr.bas"},"position":{"line":1,"character":6}}})FB";

char const *kIntrinsicExprPrFrame =
    R"FB({"jsonrpc":"2.0","id":"iep","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/intr.bas"},"position":{"line":2,"character":6}}})FB";

char const *kIntrinsicStmtPrFrame =
    R"FB({"jsonrpc":"2.0","id":"isp","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/intr.bas"},"position":{"line":3,"character":2}}})FB";

char const *kIntrinsicSignatureFrame =
    R"FB({"jsonrpc":"2.0","id":"isg","method":"textDocument/signatureHelp","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/intr.bas"},"position":{"line":4,"character":20}}})FB";

char const *kIntrinsicHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"ihv","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/intr.bas"},"position":{"line":5,"character":4}}})FB";

// A semantic-tokens document: `dim`/`as` keywords, a declared variable with a
// declaration modifier, its usage, `=`/`+` operators, and two numbers. The
// trailing `1` becomes `12` in the change frame, which shifts the last token.
char const kSemOpenFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/fblsp-sem.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"dim counter as integer\ncounter = 1\n"}}})FB";

char const kSemChangeFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/fblsp-sem.bas","version":2},"contentChanges":[{"range":)FB"
    R"FB({"start":{"line":1,"character":10},"end":{"line":1,"character":11}},"text":"12"}]}})FB";

char const *kSemFullFrame =
    R"FB({"jsonrpc":"2.0","id":"stfull","method":"textDocument/semanticTokens/full","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/fblsp-sem.bas"}}})FB";

std::string SemDeltaFrame(std::string const &id, std::string const &previous) {
  return "{\"jsonrpc\":\"2.0\",\"id\":\"" + id +
         "\",\"method\":\"textDocument/semanticTokens/full/delta\",\"params\":"
         "{\"textDocument\":{\"uri\":\"file:///tmp/fblsp-sem.bas\"},"
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
    R"FB({"textDocument":{"uri":"file:///tmp/fblsp-sem.bas"},"range":{"start":{"line":1,"character":0},)FB"
    R"FB("end":{"line":1,"character":15}}}})FB";

// Inlay-hint document: a SUB block (closer hint) and a suffix-typed dim without
// an AS clause (inferred-type hint).
char const kInlayOpenFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/fblsp-inlay.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"sub greet()\n    print 1\nend sub\ndim x$\n"}}})FB";

char const *kInlayHintFrame =
    R"FB({"jsonrpc":"2.0","id":"inh","method":"textDocument/inlayHint","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/fblsp-inlay.bas"},"range":{"start":{"line":0,"character":0},)FB"
    R"FB("end":{"line":3,"character":0}}}})FB";

char const kDidCloseFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/hello.bas"}})FB";

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

  Expect(output_all.find(kUri) != std::string::npos,
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
  Expect(output_all.find("\"source\":\"freebasiclsp\"") != std::string::npos,
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

  std::string const fileUri = "file://" + (wsDir / "lib.bi").string();
  std::string const rootUri = "file://" + wsDir.string();
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

  std::string const rootUri = "file://" + wsDir.string();
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  // Open a header living outside the workspace root.
  std::string const outsideUri = "file://" + (outsideDir / "dep.bi").string();
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

  std::string const rootUri = "file://" + wsDir.string();
  std::string const initFrame =
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
      rootUri + "\"}}";
  input->append(MakeLspFrame(initFrame.c_str()));
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"workspaceSymbolProvider\":") != std::string::npos,
         "initialize must advertise workspace/symbol");

  // Open the out-of-root engine.bas and hover `.walls` (line 4, char 26).
  std::string const engineUri =
      "file://" + (projDir / "src" / "engine.bas").string();
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

  std::string const appUri = "file://" + (proj / "app.bas").string();
  std::string const broadRootUri = "file://" + broad.string();
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
    appUris.push_back("file://" + (proj / "app.bas").string());
  }

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const broadRootUri = "file://" + broad.string();
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

  std::string const badUri = "file://" + (sandbox / "main.bas").string();
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
  std::string const goodUri = "file://" + (sandbox / "uses.bas").string();
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

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  std::string const fileUri = "file://" + (wsDir / "lib.bi").string();
  std::string const rootUri = "file://" + wsDir.string();
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
      fileUri + R"(","languageId":"basic","version":1,"text":")" +
      ToJsonString(lib) + "\"}}}";
  input->append(MakeLspFrame(openFrame.c_str()));

  auto querySymbol = [&](int n, std::string const &name) {
    std::string const id = "\"id\":\"wat" + std::to_string(n) + "\"";
    std::string const request =
        R"({"jsonrpc":"2.0","id":"wat)" + std::to_string(n) +
        R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
    input->append(MakeLspFrame(request.c_str()));
    return WaitForOutputContaining(output, id, 50);
  };

  // The initial background scan must index lib.bi before the edit.
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
  Expect(moduleLevel.find("\"start\":{\"line\":1,\"character\":4}") !=
                 std::string::npos &&
             moduleLevel.find("\"end\":{\"line\":1,\"character\":13}") !=
                 std::string::npos,
         "module-level use of a plain header dim must resolve into the header");

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
           R"(","method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/resolve.bas"},"position":{"line":1,"character":8}}})";
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
      R"FB({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///tmp/resolve.bas"},"contentChanges":[{"range":{"start":{"line":0,"character":0},"end":{"line":1,"character":21}},"text":"dim total as integer\ntotal = total + 1\n"}]}})FB"));
  Expect(WaitForPublishedUri(output, 2).empty() == false,
         "the versionless change must publish diagnostics");

  input->append(MakeLspFrame(
      R"({"jsonrpc":"2.0","id":"ds2","method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///tmp/resolve.bas"}}})"));
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

int main(int argc, char **argv) {
  test::InitTestFilter(argc, argv);
  RUN_TEST(TestInitializeReportsSyncCapabilities);
  RUN_TEST(TestDidOpenPublishesDiagnostics);
  RUN_TEST(TestDiagnosticsReflectParseErrors);
  RUN_TEST(TestDiagnosticsRespectDeclarationScopes);
  RUN_TEST(TestDocumentSymbolsReturnHierarchy);
  RUN_TEST(TestHoverShowsSignatureAndDoc);
  RUN_TEST(TestHoverResolvesUsageToDeclaration);
  RUN_TEST(TestHoverShowsMemberAccess);
  RUN_TEST(TestFoldingRangesReturned);
  RUN_TEST(TestDefinitionResolvesToDeclaration);
  RUN_TEST(TestReferencesListAllSites);
  RUN_TEST(TestHighlightCoversAllSites);
  RUN_TEST(TestCompletionOffersKeywordsAndSymbols);
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
  RUN_TEST(TestDidChangePushesDiagnostics);
  RUN_TEST(TestDidCloseEvictsAndPublishes);
  RUN_TEST(TestShutdownReturnsNullResult);
  RUN_TEST(TestInitializeServesStaticWatchersToNonDynamicClient);
  RUN_TEST(TestInitializedRegistersWatchedFilesDynamically);
  RUN_TEST(TestMissingIncludePublishesDiagnostic);
  RUN_TEST(TestWatchedFilesRescanConverges);
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
  RUN_TEST(TestRepeatRequestsShareAnalysis);
  RUN_TEST(TestVersionlessChangeServesFreshAnalysis);
  RUN_TEST(TestReferencesClosureReusesAnalysis);
  return test::Failures() == 0 ? 0 : 1;
}
