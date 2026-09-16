#include "LibLsp/JsonRpc/Condition.h"
#include "LibLsp/lsp/LanguageSession.h"
#include "session.h"

#include <memory>

int main() {
  lsp::LanguageSession session;
  FreeBasicServer server(session);
  Condition<bool> exit_requested;

  server.setExitHandler([&exit_requested]() {
    exit_requested.notify(std::make_unique<bool>(true));
  });
  server.registerHandlers();

  session.startStdio();
  exit_requested.wait();
  session.stop();
  return 0;
}
