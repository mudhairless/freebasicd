/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// M13 call hierarchy.
//
// PrepareCallHierarchy anchoring the procedure under the cursor, and both
// directions of the hierarchy crossing an include boundary.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {
// M13 call hierarchy end to end: prepare anchors the procedure under the
// cursor, outgoingCalls answers "what does it call?" and incomingCalls "who
// calls it?", both across the include boundary and in the shape a client
// navigates by.
void TestCallHierarchyOutgoingAndIncoming() {
  TwoFileFixture const fix;

  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto output = std::make_shared<StringOStream>();
  FreeBasicServer server(session);
  // Both files are opened: every range below is measured against the buffer the
  // client sent, which is also the buffer the scan parses.
  auto input = StartIndexedSession(
      session, server, output, fix,
      {{fix.callerUri, kCallerContent}, {fix.calleeUri, kCalleeContent}});

  // The capability is asserted before the requests, as elsewhere: a client only
  // asks for a feature the server advertised.
  Expect(WaitForOutputContaining(output, "\"id\":\"init\"")
                 .find("\"callHierarchyProvider\":true") != std::string::npos,
         "initialize must advertise callHierarchy");

  // The two nodes, as a client would carry them between the three requests.
  std::string const callerSrc = kCallerContent;
  std::string const calleeSrc = kCalleeContent;
  std::string const driveRange =
      WireRangeBetween(callerSrc, "sub driveProc", "end sub");
  std::string const driveName = WireRangeOf(callerSrc, "driveProc");
  std::string const driveItem =
      CallItemJson(fix.callerUri, "driveProc", driveRange, driveName);
  std::string const hubRange =
      WireRangeBetween(calleeSrc, "sub hubProc", "end sub");
  std::string const hubName = WireRangeOf(calleeSrc, "hubProc");
  std::string const hubItem =
      CallItemJson(fix.calleeUri, "hubProc", hubRange, hubName);

  // The call sites, in caller.bas: hubProc is called twice (with a parameter
  // list, and bare), and driveProc calls itself.
  std::string const hubSite1 = WireRangeOf(callerSrc, "hubProc");
  std::string const hubSite2 =
      WireRangeOf(callerSrc, "hubProc", callerSrc.find("hubProc") + 1);
  std::string const selfSite =
      WireRangeOf(callerSrc, "driveProc", callerSrc.find("hubProc 2") + 1);

  // prepare: the cursor is on a call inside driveProc's body, so the node is
  // the procedure around it — not the call, and not the include above it.
  input->append(MakeLspFrame(
      (R"({"jsonrpc":"2.0","id":"chprep","method":"textDocument/prepareCallHierarchy","params":{"textDocument":{"uri":")" +
       fix.callerUri + R"("},"position":{"line":2,"character":6}}})")
          .c_str()));
  std::string const prep =
      TailAfter(WaitForOutputContaining(output, "\"id\":\"chprep\""),
                "\"id\":\"chprep\"");
  Expect(prep.find("\"name\":\"driveProc\"") != std::string::npos,
         "prepare must name the procedure containing the cursor");
  Expect(prep.find("\"uri\":\"" + fix.callerUri + "\"") != std::string::npos,
         "the prepared node must live in the document the cursor is in");
  Expect(prep.find(driveName) != std::string::npos,
         "the prepared node's selectionRange must cover its name token");
  Expect(prep.find(driveRange) != std::string::npos,
         "the prepared node's range must cover the whole declaration");
  Expect(
      prep.find("\"detail\":\"" + SignatureLine(callerSrc, "sub driveProc") +
                "\"") != std::string::npos,
      "the prepared node must carry the declaration signature as its detail");
  Expect(prep.find("\"data\":") == std::string::npos,
         "the prepared node must carry no data field: uri and selectionRange "
         "already identify it, and nothing here could read a blob back");

  // prepare at module level: there is no declaration to anchor a hierarchy on,
  // so the answer is an absent result rather than a node the client cannot
  // navigate to.
  input->append(MakeLspFrame(
      (R"({"jsonrpc":"2.0","id":"chmod","method":"textDocument/prepareCallHierarchy","params":{"textDocument":{"uri":")" +
       fix.callerUri + R"("},"position":{"line":0,"character":3}}})")
          .c_str()));
  std::string const atModule = TailAfter(
      WaitForOutputContaining(output, "\"id\":\"chmod\""), "\"id\":\"chmod\"");
  Expect(atModule.find("\"name\":") == std::string::npos,
         "prepare at module level must return no item");

  // outgoingCalls: one entry per callee, both hubProc sites merged into one
  // entry, and the self-call as a second. Polled, because resolving hubProc
  // across the include waits on the background scan.
  std::string const outgoing = PollRequest(
      input, output, "chout", "\"name\":\"hubProc\"",
      [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"callHierarchy/outgoingCalls","params":{"item":)" +
               driveItem + "}}";
      });
  Expect(outgoing.find("\"name\":\"hubProc\"") != std::string::npos,
         "outgoingCalls must name the callee");
  Expect(outgoing.find("\"uri\":\"" + fix.calleeUri + "\"") !=
             std::string::npos,
         "the callee node must point at the file that declares it");
  Expect(outgoing.find(hubName) != std::string::npos,
         "the callee node selectionRange must be its name token in the file "
         "that declares it, not in the caller");
  Expect(outgoing.find(hubRange) != std::string::npos,
         "the callee node range must be measured against the bytes of the "
         "callee file, which are not the bytes of the caller");
  Expect(
      outgoing.find("\"detail\":\"" + SignatureLine(calleeSrc, "sub hubProc") +
                    "\"") != std::string::npos,
      "the callee node must carry the signature of the declaration it names");
  Expect(outgoing.find(hubSite1) != std::string::npos &&
             outgoing.find(hubSite2) != std::string::npos,
         "both call sites of hubProc must be reported in fromRanges");
  Expect(outgoing.find("\"name\":\"driveProc\"") != std::string::npos &&
             outgoing.find(selfSite) != std::string::npos,
         "a recursive call must be an outgoing call of the procedure onto "
         "itself");
  Expect(CountOf(outgoing, "\"fromRanges\":") == 2,
         "outgoingCalls must return exactly one entry per callee: hubProc and "
         "driveProc, with hubProc's two sites merged");

  // incomingCalls: the callers of hubProc, with both sites merged into the one
  // caller. Polled for the same reason as above — the answer walks every file
  // whose include closure reaches the callee.
  std::string const incoming = PollRequest(
      input, output, "chin", "\"name\":\"driveProc\"",
      [&](std::string const &id) {
        return R"({"jsonrpc":"2.0","id":")" + id +
               R"(","method":"callHierarchy/incomingCalls","params":{"item":)" +
               hubItem + "}}";
      });
  Expect(incoming.find("\"name\":\"driveProc\"") != std::string::npos,
         "incomingCalls must name the calling procedure");
  Expect(incoming.find(driveRange) != std::string::npos,
         "the caller node range must be measured against the bytes of the "
         "calling file");
  Expect(incoming.find("\"uri\":\"" + fix.callerUri + "\"") !=
             std::string::npos,
         "incomingCalls must name the file that does the calling");
  Expect(incoming.find(hubSite1) != std::string::npos &&
             incoming.find(hubSite2) != std::string::npos,
         "incomingCalls must report both call sites in the caller");
  Expect(CountOf(incoming, "\"fromRanges\":") == 1,
         "one calling procedure means one incoming entry");
  Expect(
      incoming.find(fix.calleeUri) == std::string::npos,
      "the callee's own file must contribute no caller of itself: hubProc is "
      "not called in callee.bi");

  session.stop();
}
} // namespace

namespace fbtest {

void RunCallHierarchyTests() { RUN_TEST(TestCallHierarchyOutgoingAndIncoming); }

} // namespace fbtest
