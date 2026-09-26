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

#ifndef FBLANG_VERSION
// A build that forgets the definition (an out-of-tree consumer of the
// sources) still runs; it just cannot name its own version.
#define FBLANG_VERSION "unknown"
#endif

int main() {
  // Message catalogs (env locale -> FBLANG_LOCALEDIR -> build/install trees)
  // before any diagnostic or log message is produced.
  fblang::initI18n();
  // stderr, not the protocol stream: the version is the one thing a user
  // cannot get from the LSP handshake (LspCpp's InitializeResult carries no
  // `serverInfo`). Same prefix as the session's log lines so it reads as one
  // stream.
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
