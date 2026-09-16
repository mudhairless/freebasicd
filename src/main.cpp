#include "LibLsp/JsonRpc/Condition.h"
#include "LibLsp/lsp/LanguageSession.h"
#include "index.h"
#include "session.h"

#include <memory>

int main()
{
    // The workspace index is in-memory only; remove any per-workspace JSON
    // cache an older build left on disk so it cannot keep eating disk space.
    fblang::cleanupLegacyDiskIndex();

    lsp::LanguageSession session;
    FreeBasicServer server(session);
    Condition<bool> exit_requested;

    server.setExitHandler(
        [&exit_requested]()
        {
            exit_requested.notify(std::make_unique<bool>(true));
        });
    server.registerHandlers();

    session.startStdio();
    exit_requested.wait();
    session.stop();
    return 0;
}