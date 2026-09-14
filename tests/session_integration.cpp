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
           "M1 publishes an empty diagnostics list on didOpen");

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
    RUN_TEST(TestDidChangePushesDiagnostics);
    RUN_TEST(TestDidCloseEvictsAndPublishes);
    RUN_TEST(TestShutdownReturnsNullResult);
    RUN_TEST(TestExitNotifiesSession);
    RUN_TEST(TestEndToEndLifecycle);
    return test::Failures() == 0 ? 0 : 1;
}