#include "LibLsp/LspCpp.h"
#include "src/session.h"
#include "test_helpers.h"

#include <atomic>
#include <chrono>
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

char const kDidCloseFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":)FB"
    R"FB({"uri":"file:///tmp/hello.bas"}})FB";

char const* kShutdownFrame = R"FB({"jsonrpc":"2.0","id":2,"method":"shutdown","params":null})FB";
char const* kExitFrame = R"FB({"jsonrpc":"2.0","method":"exit","params":null})FB";

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

} // namespace

int main()
{
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
    RUN_TEST(TestSignatureHelpShowsParamsAndActiveIndex);
    RUN_TEST(TestDidChangePushesDiagnostics);
    RUN_TEST(TestDidCloseEvictsAndPublishes);
    RUN_TEST(TestShutdownReturnsNullResult);
    RUN_TEST(TestExitNotifiesSession);
    RUN_TEST(TestEndToEndLifecycle);
    return test::Failures() == 0 ? 0 : 1;
}