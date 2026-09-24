#include "LibLsp/JsonRpc/Condition.h"
#include "LibLsp/lsp/LanguageSession.h"
#include "i18n.h"
#include "session.h"

#include <memory>

int main() {
  // Message catalogs (env locale -> FBLANG_LOCALEDIR -> build/install trees)
  // before any diagnostic or log message is produced.
  fblang::initI18n();
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
