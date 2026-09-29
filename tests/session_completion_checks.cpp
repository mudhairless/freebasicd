/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// textDocument/completion and textDocument/signatureHelp.
//
// The two requests that propose text: keyword and symbol completions, member
// completions filtered by the access context, the signature-help active index,
// and the intrinsic-function catalog.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {
char const *kCompletionFrame =
    R"FB({"jsonrpc":"2.0","id":"comp","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/resolve.bas"},"position":{"line":1,"character":8}}})FB";

// Member completion context (PLAN.md M19): `p.` must complete only the UDT's
// accessible members — never keywords, globals, or intrinsics. The example
// type has a private member, so a module-level `p.` offers just x/y; inside
// the type's own member procedure the private member is offered too
// (FreeBASIC.md §7 Access sections, fbc's error-202 gate). The variable name
// is `p`, distinct
// from the type `Position`: FreeBASIC keys identifiers case-insensitively, so
// `dim position as Position` would collide the variable with its own type
// (fbc separates the namespaces; today's resolver does not — see §12).
char const kDidOpenMemberCompletionModuleFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/memcomp.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"type Position\n)FB"
    R"FB(    x as integer\n)FB"
    R"FB(    y as integer\n)FB"
    R"FB(    private:\n)FB"
    R"FB(    hidden as integer\n)FB"
    R"FB(end type\n)FB"
    R"FB(dim p as Position\n)FB"
    R"FB(p.\n"}}})FB";

// Cursor at the end of line 7 (`p.`), right after the dot.
char const *kMemberCompletionModuleFrame =
    R"FB({"jsonrpc":"2.0","id":"mc1","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/memcomp.bas"},"position":{"line":7,"character":2}}})FB";

char const kDidOpenMemberCompletionProcFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/memcomp2.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"type Position\n)FB"
    R"FB(    x as integer\n)FB"
    R"FB(    y as integer\n)FB"
    R"FB(    private:\n)FB"
    R"FB(    hidden as integer\n)FB"
    R"FB(end type\n)FB"
    R"FB(sub Position.set()\n)FB"
    R"FB(    dim obj as Position\n)FB"
    R"FB(    obj.\n"}}})FB";

// Cursor at the end of line 8 (`    obj.`), right after the dot inside the
// member procedure implementation.
char const *kMemberCompletionProcFrame =
    R"FB({"jsonrpc":"2.0","id":"mc2","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/memcomp2.bas"},"position":{"line":8,"character":8}}})FB";

char const *kSignatureHelpFrame =
    R"FB({"jsonrpc":"2.0","id":"sig","method":"textDocument/signatureHelp","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/calls.bas"},"position":{"line":5,"character":11}}})FB";

char const *kIntrinsicExprLeFrame =
    R"FB({"jsonrpc":"2.0","id":"ile","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/intr.bas"},"position":{"line":1,"character":6}}})FB";

char const *kIntrinsicExprPrFrame =
    R"FB({"jsonrpc":"2.0","id":"iep","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/intr.bas"},"position":{"line":2,"character":6}}})FB";

char const *kIntrinsicStmtPrFrame =
    R"FB({"jsonrpc":"2.0","id":"isp","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/intr.bas"},"position":{"line":3,"character":2}}})FB";

char const *kIntrinsicSignatureFrame =
    R"FB({"jsonrpc":"2.0","id":"isg","method":"textDocument/signatureHelp","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/intr.bas"},"position":{"line":4,"character":20}}})FB";

void TestCompletionOffersKeywordsAndSymbols() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenResolveFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kCompletionFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"comp\"");

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
  Expect(response.find("https://www.freebasic.net/wiki/KeyPgIf") !=
             std::string::npos,
         "keyword documentation must link to the FreeBASIC wiki");

  session.stop();
}

void TestCompletionFiltersMembersByAccessContext() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  // Module level: `p.` offers only the public members.
  input->append(MakeLspFrame(kDidOpenMemberCompletionModuleFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kMemberCompletionModuleFrame));
  std::string const moduleResponse =
      WaitForOutputContaining(output, "\"id\":\"mc1\"");

  Expect(moduleResponse.find("\"label\":\"x\"") != std::string::npos &&
             moduleResponse.find("\"label\":\"y\"") != std::string::npos,
         "`p.` must offer the public members x and y");
  Expect(moduleResponse.find("\"label\":\"hidden\"") == std::string::npos,
         "the private member must not complete at module level");
  Expect(moduleResponse.find("\"label\":\"dim\"") == std::string::npos,
         "member access must not fall back to keywords");
  Expect(moduleResponse.find("\"label\":\"pow\"") == std::string::npos,
         "member access must not offer intrinsic catalog entries");

  // Inside `sub Position.set()`: the same type's private member completes.
  input->append(MakeLspFrame(kDidOpenMemberCompletionProcFrame));
  Expect(WaitForPublishedUri(output, 2).empty() == false,
         "second didOpen must publish diagnostics");

  input->append(MakeLspFrame(kMemberCompletionProcFrame));
  std::string const procResponse =
      WaitForOutputContaining(output, "\"id\":\"mc2\"");

  Expect(
      procResponse.find("\"label\":\"hidden\"") != std::string::npos,
      "the private member must complete inside the owner's member procedure");
  Expect(procResponse.find("\"label\":\"x\"") != std::string::npos,
         "public members still complete inside the member procedure");

  session.stop();
}

void TestSignatureHelpShowsParamsAndActiveIndex() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenCallsFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kSignatureHelpFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"sig\"");

  Expect(response.find("\"id\":\"sig\"") != std::string::npos,
         "signatureHelp request must receive a response");
  Expect(response.find("function add(a as integer, b as integer) as integer") !=
             std::string::npos,
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

void TestCompletionOffersIntrinsicCatalogItems() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenIntrinsicFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "intrinsic document didOpen must publish diagnostics");

  input->append(MakeLspFrame(kIntrinsicExprLeFrame));
  std::string const le = WaitForOutputContaining(output, "\"id\":\"ile\"");
  Expect(le.find("\"label\":\"Left$\"") != std::string::npos,
         "completion must offer the Left$ intrinsic under the `le` prefix");
  Expect(le.find(
             "\"detail\":\"Left$( str As String, n As Integer ) As String\"") !=
             std::string::npos,
         "the intrinsic item must carry its canonical signature");
  Expect(le.find("https://www.freebasic.net/wiki/KeyPgLeft") !=
             std::string::npos,
         "the intrinsic item must link to its wiki page");
  Expect(le.find("\"label\":\"left\"") == std::string::npos,
         "the bare keyword entry must be replaced by the catalog item, not "
         "duplicated");

  // Statement rows stay out of expression position...
  input->append(MakeLspFrame(kIntrinsicExprPrFrame));
  std::string const exprPr = WaitForOutputContaining(output, "\"id\":\"iep\"");
  Expect(exprPr.find("\"label\":\"Procptr\"") != std::string::npos,
         "function intrinsics must still complete in expression position");
  Expect(exprPr.find("\"label\":\"Print\"") == std::string::npos,
         "statement intrinsics must not complete in an expression");

  // ...and are offered where a statement may start.
  input->append(MakeLspFrame(kIntrinsicStmtPrFrame));
  std::string const stmtPr = WaitForOutputContaining(output, "\"id\":\"isp\"");
  Expect(stmtPr.find("\"label\":\"Print\"") != std::string::npos,
         "statement intrinsics must complete at statement position");
  Expect(stmtPr.find("\"detail\":\"Print [ #filenum, ]") != std::string::npos,
         "the statement item must carry its usage signature");

  session.stop();
}

void TestSignatureHelpResolvesIntrinsic() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenIntrinsicFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "intrinsic document didOpen must publish diagnostics");

  input->append(MakeLspFrame(kIntrinsicSignatureFrame));
  std::string const sig = WaitForOutputContaining(output, "\"id\":\"isg\"");
  Expect(sig.find("Mid$( str As String, start As Integer ) As String") !=
             std::string::npos,
         "signature help must resolve the keyword-lexed Mid$ intrinsic");
  Expect(sig.find("\"label\":\"str\"") != std::string::npos &&
             sig.find("\"label\":\"start\"") != std::string::npos,
         "intrinsic signature help must list the catalog parameters");
  Expect(sig.find("\"activeParameter\":1") != std::string::npos,
         "the comma must advance the active parameter");

  session.stop();
}
} // namespace

namespace fbtest {

void RunCompletionTests() {
  RUN_TEST(TestCompletionOffersKeywordsAndSymbols);
  RUN_TEST(TestCompletionFiltersMembersByAccessContext);
  RUN_TEST(TestSignatureHelpShowsParamsAndActiveIndex);
  RUN_TEST(TestCompletionOffersIntrinsicCatalogItems);
  RUN_TEST(TestSignatureHelpResolvesIntrinsic);
}

} // namespace fbtest
