/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "LibLsp/JsonRpc/Condition.h"
#include "LibLsp/lsp/LanguageSession.h"
#include "i18n.h"
#include "session.h"

#include <cstdio>
#include <memory>

int main() {
  // Message catalogs (env locale -> FBLANG_LOCALEDIR -> build/install trees)
  // before any diagnostic or log message is produced.
  fblang::initI18n();
  // stderr, and still kept: the handshake now reports the version too
  // (`InitializeResult.serverInfo`), but that reaches the editor's UI, not
  // the logs a bug report is read from — a user filing an issue pastes this
  // line, and it is the one thing that says which build they are running.
  // Same prefix as the session's log lines so it reads as one stream.
  std::fprintf(stderr, "[freebasicd] freebasicd %s starting\n", FBLANG_VERSION);
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
