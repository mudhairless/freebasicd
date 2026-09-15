#include "LibLsp/LspCpp.h"
#include "src/session.h"
#include "test_helpers.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace
{
using test::Expect;
using test::FeedableIStream;
using test::MakeLspFrame;
using test::StringOStream;
using test::WaitForOutputContaining;

char const* kUri = "file:///tmp/hello.bas";

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

char const kDidOpenHierFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/hello.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"sub greet(name as string)\n    print name\nend sub\n\n)FB"
    R"FB(function clamp(v as integer, lo as integer, hi as integer) as integer\n)FB"
    R"FB(    if v < lo then return lo\n    if v > hi then return hi\nend function\n"}}})FB";

char const* kDocumentSymbolFrame =
    R"FB({"jsonrpc":"2.0","id":"dsym","method":"textDocument/documentSymbol","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/hello.bas"}}})FB";

// Line 4 is the `function clamp(...)` header; the cursor sits on that line.
char const* kHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"hov","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/hello.bas"},"position":{"line":4,"character":1}}})FB";

char const* kHoverOnBodyFrame =
    R"FB({"jsonrpc":"2.0","id":"hov2","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/hello.bas"},"position":{"line":5,"character":4}}})FB";

char const* kFoldingRangeFrame =
    R"FB({"jsonrpc":"2.0","id":"fold","method":"textDocument/foldingRange","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/hello.bas"}}})FB";

// A second document exercising identifier resolution: module dim, a usage,
// and a sub with a shadowing local dim.
char const kDidOpenResolveFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/resolve.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"dim counter as integer\ncounter = counter + 1\n"}}})FB";

char const* kDefinitionFrame =
    R"FB({"jsonrpc":"2.0","id":"def","method":"textDocument/definition","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/resolve.bas"},"position":{"line":1,"character":0}}})FB";

char const* kReferencesFrame =
    R"FB({"jsonrpc":"2.0","id":"ref","method":"textDocument/references","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/resolve.bas"},"position":{"line":1,"character":0},)FB"
    R"FB("context":{"includeDeclaration":true}}})FB";

char const* kHighlightFrame =
    R"FB({"jsonrpc":"2.0","id":"hl","method":"textDocument/documentHighlight","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/resolve.bas"},"position":{"line":1,"character":0}}})FB";

// A module with a function and a call to get signature help inside the call.
char const kDidOpenCallsFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/calls.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"function add(a as integer, b as integer) as integer\n    return a + b\nend function\n\n)FB"
    R"FB(dim x as integer\nx = add(1, \n"}}})FB";

char const* kCompletionFrame =
    R"FB({"jsonrpc":"2.0","id":"comp","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/resolve.bas"},"position":{"line":1,"character":8}}})FB";

char const* kSignatureHelpFrame =
    R"FB({"jsonrpc":"2.0","id":"sig","method":"textDocument/signatureHelp","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/calls.bas"},"position":{"line":5,"character":11}}})FB";

char const* kKeywordHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"khh","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file:///tmp/calls.bas"},"position":{"line":4,"character":0}}})FB";

char const kDidCloseFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/hello.bas"}})FB";

char const* kShutdownFrame = R"FB({"jsonrpc":"2.0","id":2,"method":"shutdown","params":null})FB";
char const* kExitFrame = R"FB({"jsonrpc":"2.0","method":"exit","params":null})FB";

// A client that opts into `workspace.didChangeWatchedFiles` dynamic
// registration; must be registered for it on the `initialized` notification.
char const kInitializeDynamicFrame[] =
    R"FB({"jsonrpc":"2.0","id":"init","method":"initialize","params":{)FB"
    R"FB("capabilities":{"workspace":{"didChangeWatchedFiles":{"dynamicRegistration":true}}}}})FB";

char const* kInitializedFrame =
    R"FB({"jsonrpc":"2.0","method":"initialized","params":{}})FB";

std::string WaitForPublishedUri(std::shared_ptr<StringOStream> const& output, size_t count)
{
    std::string cur;
    for (int i = 0; i < 100; ++i)
    {
        cur = output->snapshot();
        size_t found = 0;
        size_t pos = 0;
        while ((pos = cur.find("\"method\":\"textDocument/publishDiagnostics\"", pos)) != std::string::npos)
        {
            ++found;
            pos += 1;
        }
        if (found >= count)
        {
            return cur;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cur;
}

void TestInitializeReportsSyncCapabilities()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();

    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kInitializeFrame));
    std::string const response = WaitForOutputContaining(output, "\"id\":\"init\"");

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
    Expect(response.find("\"documentHighlightProvider\":true") != std::string::npos,
           "initialize response must advertise documentHighlight support");
    Expect(response.find("\"completionProvider\"") != std::string::npos,
           "initialize response must advertise completion support");
    Expect(response.find("\"signatureHelpProvider\"") != std::string::npos,
           "initialize response must advertise signatureHelp support");

    session.stop();
}

void TestDidOpenPublishesDiagnostics()
{
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

void TestDiagnosticsReflectParseErrors()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();

    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kDidOpenDupFrame));
    std::string const output_all = WaitForPublishedUri(output, 1);

    Expect(output_all.find("\"code\":\"duplicate-definition\"") != std::string::npos,
           "duplicate dims must be reported with their diagnostic code");
    Expect(output_all.find("duplicate definition: 'x'") != std::string::npos,
           "duplicate dims must be reported with the offending name");
    Expect(output_all.find("\"severity\":2") != std::string::npos,
           "duplicate dims must be reported as warnings");
    Expect(output_all.find("\"source\":\"freebasiclsp\"") != std::string::npos,
           "diagnostics must carry the server source name");
    Expect(output_all.find("\"start\":{\"line\":1,\"character\":4}") != std::string::npos,
           "the duplicate range must cover the second 'x' name token (byte -> utf-16)");
    Expect(output_all.find("\"end\":{\"line\":1,\"character\":5}") != std::string::npos,
           "the duplicate range must end after the second 'x' name token");

    session.stop();
}

void TestDocumentSymbolsReturnHierarchy()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();

    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kDidOpenHierFrame));
    Expect(WaitForPublishedUri(output, 1).empty() == false, "didOpen must publish diagnostics");

    input->append(MakeLspFrame(kDocumentSymbolFrame));
    std::string const response = WaitForOutputContaining(output, "\"id\":\"dsym\"");

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

void TestHoverShowsSignatureAndDoc()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();

    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kDidOpenHierFrame));
    Expect(WaitForPublishedUri(output, 1).empty() == false, "didOpen must publish diagnostics");

    input->append(MakeLspFrame(kHoverFrame));
    std::string const response = WaitForOutputContaining(output, "\"id\":\"hov\"");

    Expect(response.find("\"id\":\"hov\"") != std::string::npos,
           "hover request must receive a response");
    Expect(response.find("\"kind\":\"markdown\"") != std::string::npos,
           "hover contents must be markdown");
    Expect(response.find("function clamp(v as integer, lo as integer, hi as integer) as integer") !=
               std::string::npos,
           "hover over the function header must show its signature");
    Expect(response.find("\"range\"") != std::string::npos,
           "hover must carry the selection range of the hovered symbol");

    input->append(MakeLspFrame(kHoverOnBodyFrame));
    std::string const bodyHover = WaitForOutputContaining(output, "\"id\":\"hov2\"");
    Expect(bodyHover.find("function clamp(v as integer") != std::string::npos,
           "hover anywhere inside a function must resolve to that function");

    session.stop();
}

void TestFoldingRangesReturned()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();

    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kDidOpenHierFrame));
    Expect(WaitForPublishedUri(output, 1).empty() == false, "didOpen must publish diagnostics");

    input->append(MakeLspFrame(kFoldingRangeFrame));
    std::string const response = WaitForOutputContaining(output, "\"id\":\"fold\"");

    Expect(response.find("\"id\":\"fold\"") != std::string::npos,
           "foldingRange request must receive a response");
    Expect(response.find("\"startLine\":0,\"endLine\":1") != std::string::npos,
           "the SUB block must fold from line 0 up to the END SUB line");
    Expect(response.find("\"startLine\":4,\"endLine\":6") != std::string::npos,
           "the FUNCTION block must fold from line 4 up to the END FUNCTION line");

    session.stop();
}

void TestDefinitionResolvesToDeclaration()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();

    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kDidOpenResolveFrame));
    Expect(WaitForPublishedUri(output, 1).empty() == false, "didOpen must publish diagnostics");

    input->append(MakeLspFrame(kDefinitionFrame));
    std::string const response = WaitForOutputContaining(output, "\"id\":\"def\"");

    Expect(response.find("\"id\":\"def\"") != std::string::npos,
           "definition request must receive a response");
    Expect(response.find("\"start\":{\"line\":0,\"character\":4}") != std::string::npos,
           "definition must jump to the counter declaration name");
    Expect(response.find("\"end\":{\"line\":0,\"character\":11}") != std::string::npos,
           "definition range must cover the full counter name");

    session.stop();
}

void TestReferencesListAllSites()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();

    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kDidOpenResolveFrame));
    Expect(WaitForPublishedUri(output, 1).empty() == false, "didOpen must publish diagnostics");

    input->append(MakeLspFrame(kReferencesFrame));
    std::string const response = WaitForOutputContaining(output, "\"id\":\"ref\"");

    Expect(response.find("\"id\":\"ref\"") != std::string::npos,
           "references request must receive a response");
    Expect(response.find("\"start\":{\"line\":0,\"character\":4}") != std::string::npos,
           "references must include the declaration site");
    Expect(response.find("\"start\":{\"line\":1,\"character\":0}") != std::string::npos,
           "references must include the first usage");
    Expect(response.find("\"start\":{\"line\":1,\"character\":10}") != std::string::npos,
           "references must include the second usage of counter");

    session.stop();
}

void TestHighlightCoversAllSites()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();

    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kDidOpenResolveFrame));
    Expect(WaitForPublishedUri(output, 1).empty() == false, "didOpen must publish diagnostics");

    input->append(MakeLspFrame(kHighlightFrame));
    std::string const response = WaitForOutputContaining(output, "\"id\":\"hl\"");

    Expect(response.find("\"id\":\"hl\"") != std::string::npos,
           "documentHighlight request must receive a response");
    std::size_t highlightCount = 0;
    std::size_t pos = 0;
    while ((pos = response.find("\"start\":", pos)) != std::string::npos)
    {
        ++highlightCount;
        pos += 8;
    }
    Expect(highlightCount == 3,
           "highlight must cover the declaration and both usages");

    session.stop();
}

void TestCompletionOffersKeywordsAndSymbols()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();

    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kDidOpenResolveFrame));
    Expect(WaitForPublishedUri(output, 1).empty() == false, "didOpen must publish diagnostics");

    input->append(MakeLspFrame(kCompletionFrame));
    std::string const response = WaitForOutputContaining(output, "\"id\":\"comp\"");

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
    Expect(response.find("https://www.freebasic.net/wiki/KeyPgIf") != std::string::npos,
           "keyword documentation must link to the FreeBASIC wiki");

    session.stop();
}

void TestHoverLinksKeywordDocs()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();

    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kDidOpenCallsFrame));
    Expect(WaitForPublishedUri(output, 1).empty() == false, "didOpen must publish diagnostics");

    input->append(MakeLspFrame(kKeywordHoverFrame));
    std::string const response = WaitForOutputContaining(output, "\"id\":\"khh\"");

    Expect(response.find("\"id\":\"khh\"") != std::string::npos,
           "keyword hover request must receive a response");
    Expect(response.find("dim") != std::string::npos,
           "keyword hover must name the keyword");
    Expect(response.find("https://www.freebasic.net/wiki/KeyPgDim") != std::string::npos,
           "keyword hover must link to the FreeBASIC wiki page");

    session.stop();
}

void TestSignatureHelpShowsParamsAndActiveIndex()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();

    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kDidOpenCallsFrame));
    Expect(WaitForPublishedUri(output, 1).empty() == false, "didOpen must publish diagnostics");

    input->append(MakeLspFrame(kSignatureHelpFrame));
    std::string const response = WaitForOutputContaining(output, "\"id\":\"sig\"");

    Expect(response.find("\"id\":\"sig\"") != std::string::npos,
           "signatureHelp request must receive a response");
    Expect(response.find("function add(a as integer, b as integer) as integer") != std::string::npos,
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

void TestDidChangePushesDiagnostics()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();

    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kDidOpenFrame));
    Expect(WaitForPublishedUri(output, 1).empty() == false, "didOpen must trigger a publish");

    input->append(MakeLspFrame(kDidChangeFrame));
    std::string const after = WaitForPublishedUri(output, 2);
    Expect(!after.empty() && after.size() > 1, "didChange must trigger a publish");

    session.stop();
}

void TestDidCloseEvictsAndPublishes()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();

    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kDidOpenFrame));
    Expect(WaitForPublishedUri(output, 1).empty() == false, "didOpen must trigger a publish");

    input->append(MakeLspFrame(kDidCloseFrame));
    Expect(WaitForPublishedUri(output, 2).empty() == false, "didClose must trigger a clearing publish");

    session.stop();
}

void TestShutdownReturnsNullResult()
{
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

    Expect(response.find("\"id\":2") != std::string::npos, "shutdown response must preserve the id");
    Expect(response.find("\"result\":null") != std::string::npos, "shutdown response result must be null");

    session.stop();
}

void TestExitNotifiesSession()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();
    std::atomic<bool> exited {false};

    FreeBasicServer server(session);
    server.setExitHandler(
        [&exited]()
        {
            exited.store(true, std::memory_order_relaxed);
        });
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kExitFrame));
    for (int i = 0; i < 100 && !exited.load(std::memory_order_relaxed); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    Expect(exited.load(std::memory_order_relaxed), "exit notification must signal the exit handler");

    session.stop();
}

void TestInitializeServesStaticWatchersToNonDynamicClient()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();

    FreeBasicServer server(session);
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kInitializeFrame));
    std::string const response = WaitForOutputContaining(output, "\"id\":\"init\"");

    Expect(response.find("\"didChangeWatchedFiles\"") != std::string::npos,
           "a non-dynamic client must get the static watcher capability in the initialize reply");
    Expect(response.find("\"globPattern\":\"**/*.{bas,bi}\"") != std::string::npos,
           "the static watcher must watch the FreeBASIC source globs");
    Expect(response.find("\"kind\":7") != std::string::npos,
           "the static watcher must cover create/change/delete events");

    input->append(MakeLspFrame(kInitializedFrame));
    bool sawRegistration = false;
    for (int i = 0; i < 20; ++i)
    {
        if (output->snapshot().find("\"method\":\"client/registerCapability\"") != std::string::npos)
        {
            sawRegistration = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Expect(!sawRegistration,
           "a non-dynamic client must not receive a registerCapability request after initialized");

    session.stop();
}

void TestInitializedRegistersWatchedFilesDynamically()
{
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
    std::string const registered = WaitForOutputContaining(output, "\"client/registerCapability\"");

    Expect(registered.find("\"method\":\"workspace/didChangeWatchedFiles\"") != std::string::npos,
           "the registerCapability request must register the watched-files method");
    Expect(registered.find("\"method\":\"client/registerCapability\"") != std::string::npos,
           "the server must send the registerCapability request to the client");
    Expect(registered.find("\"globPattern\":\"**/*.{bas,bi}\"") != std::string::npos,
           "the registration must watch the FreeBASIC source globs");
    Expect(registered.find("\"kind\":7") != std::string::npos,
           "the registration must cover create/change/delete events");

    std::size_t registrationCount = 0;
    std::size_t pos = 0;
    while ((pos = registered.find("\"method\":\"client/registerCapability\"", pos)) != std::string::npos)
    {
        ++registrationCount;
        pos += 1;
    }
    Expect(registrationCount == 1,
           "a dynamic client must receive exactly one registerCapability request");

    session.stop();
}

void TestEndToEndLifecycle()
{
    lsp::NullLog log;
    lsp::LanguageSession session(log);
    auto input = std::make_shared<FeedableIStream>();
    auto output = std::make_shared<StringOStream>();
    std::atomic<bool> exited {false};

    FreeBasicServer server(session);
    server.setExitHandler(
        [&exited]()
        {
            exited.store(true, std::memory_order_relaxed);
        });
    server.registerHandlers();
    session.start(input, output);

    input->append(MakeLspFrame(kInitializeFrame));
    std::string const init = WaitForOutputContaining(output, "\"id\":\"init\"");
    Expect(init.find("\"change\":2") != std::string::npos, "initialize must advertise incremental sync");

    input->append(MakeLspFrame(kDidOpenFrame));
    Expect(WaitForPublishedUri(output, 1).empty() == false, "didOpen must publish diagnostics");

    input->append(MakeLspFrame(kShutdownFrame));
    std::string const shutdown = WaitForOutputContaining(output, "\"id\":2");
    Expect(shutdown.find("\"result\":null") != std::string::npos, "shutdown response result must be null");

    input->append(MakeLspFrame(kExitFrame));
    for (int i = 0; i < 100 && !exited.load(std::memory_order_relaxed); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Expect(exited.load(std::memory_order_relaxed), "exit must signal the exit handler after shutdown");

    session.stop();
}

// Escapes FreeBASIC source so it is JSON-safe inside an LSP frame.
std::string ToJsonString(std::string const& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

void TestWorkspaceSymbolIndexesWorkspace()
{
    char const* kLibContent =
        "function clamp(v as integer, lo as integer) as integer\n"
        "    if v < lo then return lo\n"
        "    return v\n"
        "end function\n";

    static std::atomic<long> counter{0};
    std::filesystem::path const sandbox = std::filesystem::temp_directory_path() /
                                          ("fblsp-session-" + std::to_string(::time(nullptr))
                                           + "-" + std::to_string(counter.fetch_add(1)));
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
    server.setIndexCacheDir(sandbox / "cache");
    server.registerHandlers();
    session.start(input, output);

    std::string const fileUri = "file://" + (wsDir / "lib.bi").string();
    std::string const rootUri = "file://" + wsDir.string();
    std::string const initFrame =
        R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" + rootUri
        + "\"}}";
    input->append(MakeLspFrame(initFrame.c_str()));
    std::string const init = WaitForOutputContaining(output, "\"id\":\"init\"");
    Expect(init.find("\"workspaceSymbolProvider\":") != std::string::npos,
           "initialize must advertise workspace/symbol");

    std::string const openFrame =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
        R"({"uri":")" + fileUri + R"(","languageId":"basic","version":1,"text":")"
        + ToJsonString(kLibContent) + "\"}}}";
    input->append(MakeLspFrame(openFrame.c_str()));

    bool found = false;
    for (int n = 0; n < 60 && !found; ++n)
    {
        std::string const id = "\"id\":\"ws" + std::to_string(n) + "\"";
std::string const request =
            R"({"jsonrpc":"2.0","id":"ws)" + std::to_string(n)
            + R"(","method":"workspace/symbol","params":{"query":"clamp"}})";
        input->append(MakeLspFrame(request.c_str()));
        std::string const snapshot = WaitForOutputContaining(output, id, 50);
        found = snapshot.find("\"name\":\"clamp\"") != std::string::npos;
    }
    Expect(found, "workspace/symbol must return the clamp function");
    std::string const last = WaitForOutputContaining(output, "\"name\":\"clamp\"", 5);
    Expect(last.find(fileUri) != std::string::npos,
           "workspace/symbol location must point into the workspace file");

    session.stop();
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
}

// Files outside the workspace root must never be indexed: opening one and
// querying workspace/symbol must not surface its symbols.
void TestOutsideFileNotIndexed()
{
    static std::atomic<long> counter{0};
    std::filesystem::path const sandbox = std::filesystem::temp_directory_path() /
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
    server.setIndexCacheDir(sandbox / "cache");
    server.registerHandlers();
    session.start(input, output);

    std::string const rootUri = "file://" + wsDir.string();
    std::string const initFrame =
        R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" + rootUri
        + "\"}}";
    input->append(MakeLspFrame(initFrame.c_str()));
    Expect(WaitForOutputContaining(output, "\"id\":\"init\"").find("\"workspaceSymbolProvider\":") !=
               std::string::npos,
           "initialize must advertise workspace/symbol");

    // Open a header living outside the workspace root.
    std::string const outsideUri = "file://" + (outsideDir / "dep.bi").string();
    std::string const openFrame =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
        R"({"uri":")" + outsideUri + R"(","languageId":"basic","version":1,"text":")"
        + ToJsonString("sub outsideFunc()\nend sub\n") + "\"}}}";
    input->append(MakeLspFrame(openFrame.c_str()));

    auto querySymbol = [&](int n, std::string const& name) {
        std::string const id = "\"id\":\"ext" + std::to_string(n) + "\"";
        std::string const request =
            R"({"jsonrpc":"2.0","id":"ext)" + std::to_string(n)
            + R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
        input->append(MakeLspFrame(request.c_str()));
        return WaitForOutputContaining(output, id, 50);
    };

    // Give open+scan time to settle; the outside file's symbol must never
    // appear in workspace/symbol.
    bool sawOutside = false;
    for (int n = 0; n < 40 && !sawOutside; ++n)
    {
        sawOutside = querySymbol(n, "outsideFunc").find("\"name\":\"outsideFunc\"") !=
                     std::string::npos;
    }
    Expect(!sawOutside, "workspace/symbol must not return symbols from outside the root");

    session.stop();
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
}

// A client root that is itself a single project (has a .git marker) is used
// as-is; a *broad* root (e.g. an editor reporting the home directory, which
// hosts several sibling projects) is narrowed to the opened document's project
// on the first didOpen. Sibling trees under the broad root must never surface.
void TestBroadRootNarrowsToOpenedProject()
{
    static std::atomic<long> counter{0};
    std::filesystem::path const sandbox = std::filesystem::temp_directory_path() /
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
    server.setIndexCacheDir(sandbox / "cache");
    server.registerHandlers();
    session.start(input, output);

    std::string const appUri = "file://" + (proj / "app.bas").string();
    std::string const broadRootUri = "file://" + broad.string();
    std::string const initFrame =
        R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" +
        broadRootUri + "\"}}";
    input->append(MakeLspFrame(initFrame.c_str()));
    Expect(WaitForOutputContaining(output, "\"id\":\"init\"").find("\"workspaceSymbolProvider\":") !=
               std::string::npos,
           "initialize must advertise workspace/symbol");

    std::string const openFrame =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
        R"({"uri":")" + appUri + R"(","languageId":"basic","version":1,"text":")"
        + ToJsonString("sub wsOnly()\nend sub\n") + "\"}}}";
    input->append(MakeLspFrame(openFrame.c_str()));

    auto querySymbol = [&](int n, std::string const& name) {
        std::string const id = "\"id\":\"narrow" + std::to_string(n) + "\"";
        std::string const request =
            R"({"jsonrpc":"2.0","id":"narrow)" + std::to_string(n)
            + R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
        input->append(MakeLspFrame(request.c_str()));
        return WaitForOutputContaining(output, id, 50);
    };

    // The opened document's project is indexed...
    bool foundWs = false;
    for (int n = 0; n < 60 && !foundWs; ++n)
    {
        foundWs = querySymbol(n, "wsOnly").find("\"name\":\"wsOnly\"") != std::string::npos;
    }
    Expect(foundWs, "the opened document's project must be indexed");

    // ...and the sibling tree under the broad root must never be.
    bool sawSibling = false;
    for (int n = 0; n < 40 && !sawSibling; ++n)
    {
        sawSibling = querySymbol(100 + n, "siblingOnly").find("\"name\":\"siblingOnly\"") !=
                     std::string::npos;
    }
    Expect(!sawSibling, "a sibling project under a broad root must not be indexed");

    session.stop();
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
}

void TestMissingIncludePublishesDiagnostic()
{
    static std::atomic<long> counter{0};
    std::filesystem::path const sandbox = std::filesystem::temp_directory_path() /
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
    server.setIndexCacheDir(sandbox / "cache");
    server.registerHandlers();
    session.start(input, output);

    std::string const badUri = "file://" + (sandbox / "main.bas").string();
    std::string const badOpenFrame =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
        R"({"uri":")" + badUri + R"(","languageId":"basic","version":1,"text":")"
        + ToJsonString("#include \"missing.bi\"\nprint \"hi\"\n") + "\"}}}";
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
    Expect(published.find("\"start\":{\"line\":0,\"character\":10}") != std::string::npos,
           "the include range must cover the filename literal, not the whole line");

    // A resolvable include must not produce an include-not-found diagnostic.
    std::string const goodUri = "file://" + (sandbox / "uses.bas").string();
    std::string const goodOpenFrame =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
        R"({"uri":")" + goodUri + R"(","languageId":"basic","version":1,"text":")"
        + ToJsonString("#include \"ok.bi\"\n") + "\"}}}";
    input->append(MakeLspFrame(goodOpenFrame.c_str()));
    std::string const both = WaitForPublishedUri(output, 2);
    std::size_t notFound = 0;
    std::size_t pos = 0;
    while ((pos = both.find("\"code\":\"include-not-found\"", pos)) != std::string::npos)
    {
        ++notFound;
        pos += 1;
    }
    Expect(notFound == 1, "a resolvable include must not publish include-not-found");

    session.stop();
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
}

void TestWatchedFilesRescanConverges()
{
    static std::atomic<long> counter{0};
    std::filesystem::path const sandbox = std::filesystem::temp_directory_path() /
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
    server.setIndexCacheDir(sandbox / "cache");
    server.registerHandlers();
    session.start(input, output);

    std::string const fileUri = "file://" + (wsDir / "lib.bi").string();
    std::string const rootUri = "file://" + wsDir.string();
    std::string const initFrame =
        R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"rootUri":")" + rootUri
        + "\"}}";
    input->append(MakeLspFrame(initFrame.c_str()));
    Expect(WaitForOutputContaining(output, "\"id\":\"init\"").find("\"workspaceSymbolProvider\":") !=
               std::string::npos,
           "initialize must advertise workspace/symbol");

    // A non-project client root defers index creation to the first opened
    // document, exactly like a real editor always opens one.
    std::string const openFrame =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)"
        R"({"uri":")" + fileUri + R"(","languageId":"basic","version":1,"text":")"
        + ToJsonString(lib) + "\"}}}";
    input->append(MakeLspFrame(openFrame.c_str()));

    auto querySymbol = [&](int n, std::string const& name) {
        std::string const id = "\"id\":\"wat" + std::to_string(n) + "\"";
        std::string const request =
            R"({"jsonrpc":"2.0","id":"wat)" + std::to_string(n)
            + R"(","method":"workspace/symbol","params":{"query":")" + name + "\"}}";
        input->append(MakeLspFrame(request.c_str()));
        return WaitForOutputContaining(output, id, 50);
    };

    // The initial background scan must index lib.bi before the edit.
    bool primed = false;
    for (int n = 0; n < 60 && !primed; ++n)
    {
        primed = querySymbol(n, "greet").find("\"name\":\"greet\"") != std::string::npos;
    }
    Expect(primed, "workspace/symbol must find the header symbol from the initial scan");

    // A disk edit converges through the watched-files notification: no reopen,
    // no didChange, no restart.
    {
        std::ofstream out(wsDir / "lib.bi");
        out << lib << "sub farewell()\nend sub\n";
    }
    std::string const watchedFrame =
        R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[)"
        R"({"uri":")" + fileUri + R"(","type":2}]}})";
    input->append(MakeLspFrame(watchedFrame.c_str()));

    bool converged = false;
    for (int n = 0; n < 100 && !converged; ++n)
    {
        converged = querySymbol(100 + n, "farewell").find("\"name\":\"farewell\"") !=
                    std::string::npos;
    }
    Expect(converged,
           "a watched-files event must converge an external header edit into workspace/symbol");

    session.stop();
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
}

} // namespace

int main(int argc, char** argv)
{
    test::InitTestFilter(argc, argv);
    RUN_TEST(TestInitializeReportsSyncCapabilities);
    RUN_TEST(TestDidOpenPublishesDiagnostics);
    RUN_TEST(TestDiagnosticsReflectParseErrors);
    RUN_TEST(TestDocumentSymbolsReturnHierarchy);
    RUN_TEST(TestHoverShowsSignatureAndDoc);
    RUN_TEST(TestFoldingRangesReturned);
    RUN_TEST(TestDefinitionResolvesToDeclaration);
    RUN_TEST(TestReferencesListAllSites);
    RUN_TEST(TestHighlightCoversAllSites);
    RUN_TEST(TestCompletionOffersKeywordsAndSymbols);
    RUN_TEST(TestHoverLinksKeywordDocs);
    RUN_TEST(TestSignatureHelpShowsParamsAndActiveIndex);
    RUN_TEST(TestWorkspaceSymbolIndexesWorkspace);
    RUN_TEST(TestOutsideFileNotIndexed);
    RUN_TEST(TestBroadRootNarrowsToOpenedProject);
    RUN_TEST(TestDidChangePushesDiagnostics);
    RUN_TEST(TestDidCloseEvictsAndPublishes);
    RUN_TEST(TestShutdownReturnsNullResult);
    RUN_TEST(TestInitializeServesStaticWatchersToNonDynamicClient);
    RUN_TEST(TestInitializedRegistersWatchedFilesDynamically);
    RUN_TEST(TestMissingIncludePublishesDiagnostic);
    RUN_TEST(TestWatchedFilesRescanConverges);
    RUN_TEST(TestExitNotifiesSession);
    RUN_TEST(TestEndToEndLifecycle);
    return test::Failures() == 0 ? 0 : 1;
}