/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// M13 code lens.
//
// The one lens a client draws: the count, the singular form, and the command
// its click sends back.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {

// M13 code lens end to end: the header's procedure carries a lens counting the
// includer's two call sites — the count crosses the include boundary, which is
// the whole reason a lens is worth drawing — the client file's own procedure
// carries the singular form, and the command a click sends back answers with
// exactly the locations the count promised.
void TestCodeLensCountsReferencesAndAnswersItsCommand() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  auto input = StartIndexedSession(
      session, server, output, fix,
      {{fix.callerUri, kCallerContent}, {fix.calleeUri, kCalleeContent}});

  std::string const init = WaitForOutputContaining(output, "\"id\":\"init\"");
  Expect(init.find("\"codeLensProvider\":{\"resolveProvider\":false}") !=
             std::string::npos,
         "initialize must advertise codeLens without a resolve provider");
  Expect(init.find("\"executeCommandProvider\":{\"commands\":"
                   "[\"freebasicd.showReferences\"]}") != std::string::npos,
         "initialize must advertise the one command a lens click can send");

  std::string const calleeSrc = kCalleeContent;
  std::string const callerSrc = kCallerContent;
  std::string const hubName = WireRangeOf(calleeSrc, "hubProc");
  std::string const hubAnchor = WirePositionOf(calleeSrc, "hubProc");
  // The two call sites, in caller.bas: hubProc is called with a parameter list
  // and bare.
  std::string const hubSite1 = WireRangeOf(callerSrc, "hubProc");
  std::string const hubSite2 =
      WireRangeOf(callerSrc, "hubProc", callerSrc.find("hubProc") + 1);

  // Polled: the count needs caller.bas in the index, which the background scan
  // gets to on its own schedule.
  std::string const lenses = PollRequest(
      input, output, "lens1", "2 references", [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/codeLens","params":{"textDocument":{"uri":")" +
               fix.calleeUri + R"("}}})";
      });
  Expect(CountOf(lenses, "\"title\":") == 1,
         "the header declares one procedure, so it carries one lens");
  Expect(lenses.find(hubName) != std::string::npos,
         "the lens range must cover the declaration's name token");
  Expect(lenses.find("\"command\":\"freebasicd.showReferences\"") !=
             std::string::npos,
         "a lens carries no edit field, so the click must name a command");
  Expect(lenses.find(fix.calleeUri) != std::string::npos,
         "the click payload must name the document the lens was drawn in, so "
         "the client can be asked about a document it has since closed");
  Expect(
      lenses.find(hubAnchor) != std::string::npos,
      "the click payload must carry the anchor's own position: a lens has no "
      "data blob, so the position is what identifies the declaration");
  Expect(lenses.find("\"data\":") == std::string::npos,
         "resolveProvider is false, so a data field nothing would send back is "
         "noise on the wire");

  // The plural is the catalog's choice, not a two-msgid guess: one reference
  // (driveProc calls itself once) must read as a singular title.
  std::string const callerLenses = PollRequest(
      input, output, "lens2", "1 reference", [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"textDocument/codeLens","params":{"textDocument":{"uri":")" +
               fix.callerUri + R"("}}})";
      });
  Expect(callerLenses.find("1 reference\"") != std::string::npos,
         "a declaration with one reference must carry the singular title");

  // The click, as a client sends it: the lens's own arguments. The answer is
  // the reference list, which is the count the lens showed — two locations in
  // the includer, neither of them the declaration.
  std::string const shown =
      PollRequest(input, output, "cmd1", hubSite2, [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"workspace/executeCommand","params":{"command":"freebasicd.showReferences","arguments":[")" +
               fix.calleeUri + R"(",)" + hubAnchor + R"(]}})";
      });
  Expect(
      shown.find("\"uri\":\"" + fix.callerUri + "\"") != std::string::npos,
      "the command must answer with the referencing file, not the header the "
      "lens was drawn in");
  Expect(shown.find(hubSite1) != std::string::npos &&
             shown.find(hubSite2) != std::string::npos,
         "the command must list both call sites the lens counted");
  Expect(shown.find("\"uri\":\"" + fix.calleeUri + "\"") == std::string::npos,
         "the declaration itself is not one of its own references");

  // A command this server does not answer, and a payload that is not a
  // lens's: null, never a wrong answer and never a crash on the Any blob.
  std::string const unknown =
      PollRequest(input, output, "cmd2", "\"cmd2", [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"workspace/executeCommand","params":{"command":"freebasicd.somethingElse"}})";
      });
  Expect(unknown.find("null") != std::string::npos,
         "a command this build does not answer must answer null");
  std::string const junk =
      PollRequest(input, output, "cmd3", "\"cmd3", [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"workspace/executeCommand","params":{"command":"freebasicd.showReferences","arguments":[42,"later"]}})";
      });
  Expect(
      junk.find("null") != std::string::npos,
      "a payload that is not a lens's arguments must answer null, not read a "
      "position out of a number");

  session.stop();
}
} // namespace

namespace fbtest {

void RunCodeLensTests() {
  RUN_TEST(TestCodeLensCountsReferencesAndAnswersItsCommand);
}

} // namespace fbtest
