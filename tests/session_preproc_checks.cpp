/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// textDocument/hover and textDocument/completion for preprocessor symbols.
//
// The preprocessor surface as a user sees it: a `#define`/`#macro` usage
// resolves like any declaration (hover shows the definition line, completion
// offers the name), a macro's hover shows the opener with its parameters but
// never the body, and a define completed inside a procedure appears with the
// Constant kind. The engine.bas repro — a define above a function, used inside
// it, hovering the usage showed the enclosing function — is the first test.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {
// The user's reported shape: a define at the top of the module, a function
// below it, and a usage inside the function body.
char const kDidOpenDefineUsageFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/ppdef.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"#define MAX_CACHED_TEXTURES 19\n)FB"
    R"FB(\n)FB"
    R"FB(function TotalCachedTextures() as integer\n)FB"
    R"FB(    return MAX_CACHED_TEXTURES\n)FB"
    R"FB(end function\n"}}})FB";

// Hover the `MAX_CACHED_TEXTURES` usage inside the function (line 3, char 15).
char const *kDefineUsageHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"pph1","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/ppdef.bas"},"position":{"line":3,"character":15}}})FB";

// A `#macro` with a body: hovering the invocation must show the opener line
// (parameters included) and never the body text.
char const kDidOpenMacroFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/ppmac.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"#macro say_out(w)\n)FB"
    R"FB(    print w\n)FB"
    R"FB(#endmacro\n)FB"
    R"FB(\n)FB"
    R"FB(say_out(\"hello\")\n"}}})FB";

// Hover the `say_out` invocation (line 4, char 2).
char const *kMacroHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"pph2","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/ppmac.bas"},"position":{"line":4,"character":2}}})FB";

// Defines and macros both, with a sub below them: completion from inside the
// sub (or at module level) must offer both names with the Constant kind.
char const kDidOpenCompletionFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/ppcomp.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"#define MAX_CACHED_TEXTURES 19\n)FB"
    R"FB(#macro say_out(w)\n)FB"
    R"FB(    print w\n)FB"
    R"FB(#endmacro\n)FB"
    R"FB(\n)FB"
    R"FB(sub build()\n)FB"
    R"FB(    dim as integer n = 1\n"}}})FB";

// Completion at the end of the sub body (line 6, char 23).
char const *kPreprocCompletionFrame =
    R"FB({"jsonrpc":"2.0","id":"ppc1","method":"textDocument/completion","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/ppcomp.bas"},"position":{"line":6,"character":23}}})FB";

void TestPreprocHoverShowsDefineOnUsage() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenDefineUsageFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kDefineUsageHoverFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"pph1\"");

  Expect(response.find("\"id\":\"pph1\"") != std::string::npos,
         "hover request must receive a response");
  Expect(response.find("#define MAX_CACHED_TEXTURES 19") != std::string::npos,
         "hover on a define usage must show the directive's first line");
  Expect(response.find("Preprocessor define.") != std::string::npos,
         "hover on a define usage must label it a preprocessor define");
  Expect(response.find("TotalCachedTextures") == std::string::npos,
         "hover must not fall back to the enclosing function");

  session.stop();
}

void TestPreprocHoverMacroOpenerNotBody() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenMacroFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kMacroHoverFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"pph2\"");

  Expect(response.find("\"id\":\"pph2\"") != std::string::npos,
         "hover request must receive a response");
  Expect(response.find("#macro say_out(w)") != std::string::npos,
         "hover on a macro invocation must show the opener with its "
         "parameters");
  Expect(response.find("Preprocessor macro.") != std::string::npos,
         "hover on a macro invocation must label it a macro");
  Expect(response.find("print w") == std::string::npos,
         "hover must never show the macro body");

  session.stop();
}

void TestPreprocCompletionOffersDefinesAndMacros() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenCompletionFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kPreprocCompletionFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"ppc1\"");

  Expect(response.find("\"id\":\"ppc1\"") != std::string::npos,
         "completion request must receive a response");
  Expect(response.find("\"label\":\"MAX_CACHED_TEXTURES\"") !=
             std::string::npos,
         "completion must offer the define's name");
  Expect(response.find("\"label\":\"say_out\"") != std::string::npos,
         "completion must offer the macro's name");
  Expect(response.find("\"detail\":\"#define MAX_CACHED_TEXTURES 19\"") !=
             std::string::npos,
         "the define item must carry the directive as its detail");
  Expect(response.find("\"kind\":21") != std::string::npos,
         "define and macro items must have the Constant completion kind");

  session.stop();
}
} // namespace

namespace fbtest {

void RunPreprocTests() {
  RUN_TEST(TestPreprocHoverShowsDefineOnUsage);
  RUN_TEST(TestPreprocHoverMacroOpenerNotBody);
  RUN_TEST(TestPreprocCompletionOffersDefinesAndMacros);
}

} // namespace fbtest