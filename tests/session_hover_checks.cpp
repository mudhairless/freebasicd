/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// textDocument/hover.
//
// Every hover shape the server answers: a declaration's signature and doc
// comment, a usage resolved back to its declaration, member access, enum
// members, a keyword's wiki link, a member named like a keyword, an
// unresolvable type falling back to its owning variable, and an intrinsic's
// signature.
//
// A new test goes here and in the runner at the bottom of this file;
// everything it needs is either in session_support.h (the harness) or
// declared just above it (a wire frame only this file sends).

#include "session_support.h"

using namespace fbtest;

namespace {
// Line 4 is the `function clamp(...)` header; the cursor sits on that line.
char const *kHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"hov","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hello.bas"},"position":{"line":4,"character":1}}})FB";

char const *kHoverOnBodyFrame =
    R"FB({"jsonrpc":"2.0","id":"hov2","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hello.bas"},"position":{"line":5,"character":4}}})FB";

// A WITH + FOR + IF document mirroring drd/temp/src/engine.bas: a block-local
// `v1` used inside a `type(...)` initializer. Hovering the *usage* must show
// v1's declaration, not the enclosing block.
char const kDidOpenHoverUsageFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hovuse.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"sub ProcessSectorPhysics(map as map_struct, secIndex as integer)\n)FB"
    R"FB(    with map\n)FB"
    R"FB(        for i as integer = 0 to 3\n)FB"
    R"FB(            dim as Vector2 v1 = .vertices(i)\n)FB"
    R"FB(            if v1.x <> 0 then\n)FB"
    R"FB(                dim as Vector3 b = type(v1.x, .sectors(secIndex).floorHeight, v1.y)\n)FB"
    R"FB(            end if\n)FB"
    R"FB(        next i\n)FB"
    R"FB(    end with\n)FB"
    R"FB(end sub\n"}}})FB";

// Hover the `v1` usage inside the `type(...)` initializer (line 5, char 40).
char const *kHoverUsageFrame =
    R"FB({"jsonrpc":"2.0","id":"hvu","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovuse.bas"},"position":{"line":5,"character":40}}})FB";

// Hover the `v1` declaration itself (line 3, char 27).
char const *kHoverDeclFrame =
    R"FB({"jsonrpc":"2.0","id":"hvd","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovuse.bas"},"position":{"line":3,"character":27}}})FB";

// Hover the loop counter `i` in the for header (line 2, char 12).
char const *kHoverCounterFrame =
    R"FB({"jsonrpc":"2.0","id":"hvc","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovuse.bas"},"position":{"line":2,"character":12}}})FB";

// Hover a module-level Dim usage (resolve.bas line 1, char 0 = `counter`).
char const *kModuleDimHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"hvm","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/resolve.bas"},"position":{"line":1,"character":0}}})FB";

// Member-access hover (the reported regression): a WITH + inline UDT
// document mirroring drd/temp's world.bi/engine.bas shape. Hovering
// `.walls`, `.sectors(i).floorHeight` or `w.v1` must show the *field*
// declaration and its owning variable/type — never the enclosing sub.
char const kDidOpenMemberHoverFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hovmem.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"type Wall\n)FB"
    R"FB(    as integer v1, v2\n)FB"
    R"FB(end type\n)FB"
    R"FB(type Vector2\n)FB"
    R"FB(    as single x, y\n)FB"
    R"FB(end type\n)FB"
    R"FB(type Sector\n)FB"
    R"FB(    as single floorHeight\n)FB"
    R"FB(end type\n)FB"
    R"FB(type Map\n)FB"
    R"FB(    as Vector2 vertices(10)\n)FB"
    R"FB(    as Wall walls(10)\n)FB"
    R"FB(    as Sector sectors(10)\n)FB"
    R"FB(end type\n)FB"
    R"FB(sub run(map as Map, secIndex as integer)\n)FB"
    R"FB(    with map\n)FB"
    R"FB(        dim as Wall w = .walls(secIndex)\n)FB"
    R"FB(        dim as single f = .sectors(secIndex).floorHeight\n)FB"
    R"FB(        dim as single g = w.v1\n)FB"
    R"FB(    end with\n)FB"
    R"FB(end sub\n"}}})FB";

// Hover the `walls` member of the with-target (line 16, char 26).
char const *kMemberHoverWallFrame =
    R"FB({"jsonrpc":"2.0","id":"hmm","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovmem.bas"},"position":{"line":16,"character":26}}})FB";

// Hover `floorHeight` through the indexed chain (line 17, char 50).
char const *kMemberHoverFloorFrame =
    R"FB({"jsonrpc":"2.0","id":"hmf","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovmem.bas"},"position":{"line":17,"character":50}}})FB";

// Hover `v1` of the plain local variable (line 18, char 29).
char const *kMemberHoverLocalFrame =
    R"FB({"jsonrpc":"2.0","id":"hml","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovmem.bas"},"position":{"line":18,"character":29}}})FB";

// Enum conformance hover: an `Explicit` enum's branded value
// (`MyEnum.value_1`), a plain enum's qualified member with a *reserved-word*
// enum name (`color.green` — `color` is the graphics intrinsic), and a bare
// plain-enum member usage (`z = green`) must all resolve to the member
// declaration — the empty/orphan hover regression.
char const kDidOpenEnumHoverFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/enumhov.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"enum MyEnum explicit\n)FB"
    R"FB(    value_1 = 1\n)FB"
    R"FB(    value_2 = 2\n)FB"
    R"FB(end enum\n)FB"
    R"FB(enum color\n)FB"
    R"FB(    red = 1\n)FB"
    R"FB(    green = 2\n)FB"
    R"FB(end enum\n)FB"
    R"FB(dim x = MyEnum.value_1\n)FB"
    R"FB(dim y = color.green\n)FB"
    R"FB(dim z = green\n"}}})FB";

// Hover the explicit enum's branded member (line 8, char 15 = `value_1`).
char const *kEnumHoverBrandedFrame =
    R"FB({"jsonrpc":"2.0","id":"he1","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/enumhov.bas"},"position":{"line":8,"character":15}}})FB";

// Hover the reserved-word enum name's branded member (line 9, char 14 =
// `green` of `color.green`).
char const *kEnumHoverKeywordNameFrame =
    R"FB({"jsonrpc":"2.0","id":"he2","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/enumhov.bas"},"position":{"line":9,"character":14}}})FB";

// Hover a bare plain-enum member usage (line 10, char 8 = `green`).
char const *kEnumHoverBareFrame =
    R"FB({"jsonrpc":"2.0","id":"he3","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/enumhov.bas"},"position":{"line":10,"character":8}}})FB";

char const *kKeywordHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"khh","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/calls.bas"},"position":{"line":4,"character":0}}})FB";

// A procedure whose body is a dense run of reserved keywords: hovering any of
// them must show the keyword wiki link, never the enclosing sub signature (the
// reported regression). `sub` at its own header still shows the signature.
char const kDidOpenKeywordBodyFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/keybody.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"sub run(m as Map, secIndex as integer)\n)FB"
    R"FB(    dim as integer walls\n)FB"
    R"FB(    with map\n)FB"
    R"FB(        dim as integer w = .walls(secIndex)\n)FB"
    R"FB(    end with\n"}}})FB";

// Hover the `dim` inside the body (line 1, char 4).
char const *kKeywordBodyDimFrame =
    R"FB({"jsonrpc":"2.0","id":"hkd","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/keybody.bas"},"position":{"line":1,"character":4}}})FB";

// Hover the `with` inside the body (line 2, char 4).
char const *kKeywordBodyWithFrame =
    R"FB({"jsonrpc":"2.0","id":"hkw","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/keybody.bas"},"position":{"line":2,"character":4}}})FB";

// Hover the `end` closer (line 4, char 4).
char const *kKeywordBodyEndFrame =
    R"FB({"jsonrpc":"2.0","id":"hke","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/keybody.bas"},"position":{"line":4,"character":4}}})FB";

// Hover the `sub` word of its own header (line 0, char 0): the keyword opens
// the declaration, so the signature — not a generic wiki link — is shown.
char const *kKeywordBodySubHeadFrame =
    R"FB({"jsonrpc":"2.0","id":"hks","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/keybody.bas"},"position":{"line":0,"character":0}}})FB";

// Cross-file member hover: the field's type lives in a header the requesting
// file's include closure cannot reach (the `#include` names a missing file),
// so even then a member whose name collides with a local variable must resolve
// to the *member* through the workspace byKey fallback, not to the variable
// and not to the enclosing sub. Types are only in the second buffer.
char const kDidOpenCrossTypeMainFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hovx.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"#include once \"zzz_unresolved.bi\"\n)FB"
    R"FB(sub run(m as Map)\n)FB"
    R"FB(    dim as integer walls\n)FB"
    R"FB(    dim as integer w = m.walls(1)\n)FB"
    R"FB(end sub\n"}}})FB";

char const kDidOpenCrossTypeWorldFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/world.bi","languageId":"basic","version":1,)FB"
    R"FB("text":"type Wall\n)FB"
    R"FB(    as integer v1, v2\n)FB"
    R"FB(end type\n)FB"
    R"FB(type Map\n)FB"
    R"FB(    as Wall walls(10)\n)FB"
    R"FB(end type\n"}}})FB";

// Hover the `walls` member in `m.walls(1)` (line 3, char 26).
char const *kCrossTypeMemberFrame =
    R"FB({"jsonrpc":"2.0","id":"hxm","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovx.bas"},"position":{"line":3,"character":26}}})FB";

// FreeBASIC lets reserved words name type members (`as string name`), so
// hovering `t.name` must show the member/type info — never the intrinsic/
// keyword page that would otherwise attach to the reserved word (fbc-verified,
// FreeBASIC.md §2).
char const kDidOpenKeywordMemberFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hovkw.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"type Mytype\n)FB"
    R"FB(    as string name\n)FB"
    R"FB(    as integer other\n)FB"
    R"FB(end type\n)FB"
    R"FB(dim t as Mytype\n)FB"
    R"FB(t.name = \"A Name\"\n)FB"
    R"FB(sub s\n)FB"
    R"FB(    dim v as Mytype\n)FB"
    R"FB(    v.name = \"x\"\n)FB"
    R"FB(end sub\n"}}})FB";

// Hover `name` *inside* the word in `t.name` at module level (line 5, char
// 3) — a cursor on the first char after `.` lands on the `.` token, which is
// a separate pre-existing wart, so poke the middle of the word.
char const *kKeywordMemberHoverModuleFrame =
    R"FB({"jsonrpc":"2.0","id":"hkm","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovkw.bas"},"position":{"line":5,"character":3}}})FB";

// Hover the member's own declaration `name` in `as string name` (line 1,
// char 15): must behave like any identifier field, not the keyword page.
char const *kKeywordMemberHoverDeclFrame =
    R"FB({"jsonrpc":"2.0","id":"hkd","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovkw.bas"},"position":{"line":1,"character":15}}})FB";

// Hover `name` in `v.name` inside a sub body (line 8, char 7).
char const *kKeywordMemberHoverSubFrame =
    R"FB({"jsonrpc":"2.0","id":"hks","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovkw.bas"},"position":{"line":8,"character":7}}})FB";

// Unknown declared type: `map` is `as Shape`, but no `Shape` type exists
// anywhere (not in this file, not the workspace). Hovering `.walls` inside the
// `with` block must still say "Member of `map`." and must not fall back to the
// colliding local `walls` or to the enclosing sub's signature.
char const kDidOpenHovUnknownTypeFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hovunk.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"sub run(map as Shape, secIndex as integer)\n)FB"
    R"FB(    dim as integer walls\n)FB"
    R"FB(    with map\n)FB"
    R"FB(        dim as integer a = .walls(secIndex)\n)FB"
    R"FB(    end with\n)FB"
    R"FB(end sub\n"}}})FB";

// Hover the `walls` member of `.walls(secIndex)` (line 3, char 28).
char const *kHoverUnknownTypeMemberFrame =
    R"FB({"jsonrpc":"2.0","id":"hxu","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovunk.bas"},"position":{"line":3,"character":28}}})FB";

// Known type, missing member: `Map` exists with only a `walls` field, so
// `m2.missing` cannot resolve to a field — the hover must still name the
// owning variable and its (known) type.
char const kDidOpenHovMissingMemberFrame[] =
    R"FB({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":)FB"
    R"FB({"uri":"file://{{tmp}}/hovmiss.bas","languageId":"basic","version":1,)FB"
    R"FB("text":"type Map\n)FB"
    R"FB(    as integer walls(10)\n)FB"
    R"FB(end type\n)FB"
    R"FB(sub run(m2 as Map)\n)FB"
    R"FB(    dim as integer f = m2.missing\n)FB"
    R"FB(end sub\n"}}})FB";

// Hover the `missing` member of `m2.missing` (line 4, char 26).
char const *kHoverMissingMemberFrame =
    R"FB({"jsonrpc":"2.0","id":"hxq","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/hovmiss.bas"},"position":{"line":4,"character":26}}})FB";

char const *kIntrinsicHoverFrame =
    R"FB({"jsonrpc":"2.0","id":"ihv","method":"textDocument/hover","params":)FB"
    R"FB({"textDocument":{"uri":"file://{{tmp}}/intr.bas"},"position":{"line":5,"character":4}}})FB";

// Enum members resolve on hover: an `Explicit` enum's branded value
// (`MyEnum.value_1`), a plain enum's qualified member with a reserved-word
// enum name (`color.green`), and a bare plain-enum member usage (`z = green`)
// all land on the member declaration and label it as an enum member.
void TestHoverShowsEnumMembers() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenEnumHoverFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  auto hover = [&](char const *frame, char const *id, char const *signature) {
    input->append(MakeLspFrame(frame));
    std::string const h = WaitForOutputContaining(output, id);
    Expect(h.find(id) != std::string::npos,
           "the enum hover request must receive a response");
    Expect(h.find(signature) != std::string::npos,
           "the enum hover must show the member's declaration line");
    return h;
  };

  // Explicit enum, branded member: `MyEnum.value_1`.
  std::string const branded =
      hover(kEnumHoverBrandedFrame, "\"id\":\"he1\"", "value_1 = 1");
  Expect(branded.find("Enum member of `MyEnum`.") != std::string::npos,
         "an explicit enum's branded member names its enum");

  // Plain enum whose name is a reserved word: `color.green` still resolves.
  std::string const kwNamed =
      hover(kEnumHoverKeywordNameFrame, "\"id\":\"he2\"", "green = 2");
  Expect(kwNamed.find("Enum member of `color`.") != std::string::npos,
         "a reserved-word enum name still names the member's enum");

  // Bare usage of a plain enum's member resolves to the member declaration.
  std::string const bare =
      hover(kEnumHoverBareFrame, "\"id\":\"he3\"", "green = 2");
  Expect(bare.find("Enum member of `color`.") != std::string::npos,
         "a bare plain-enum member usage names the member's enum");

  session.stop();
}

void TestHoverShowsSignatureAndDoc() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenHierFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kHoverFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"hov\"");

  Expect(response.find("\"id\":\"hov\"") != std::string::npos,
         "hover request must receive a response");
  Expect(response.find("\"kind\":\"markdown\"") != std::string::npos,
         "hover contents must be markdown");
  Expect(response.find("function clamp(v as integer, lo as integer, hi as "
                       "integer) as integer") != std::string::npos,
         "hover over the function header must show its signature");
  Expect(response.find("\"range\"") != std::string::npos,
         "hover must carry the selection range of the hovered symbol");

  input->append(MakeLspFrame(kHoverOnBodyFrame));
  std::string const bodyHover =
      WaitForOutputContaining(output, "\"id\":\"hov2\"");
  Expect(bodyHover.find("function clamp(v as integer") != std::string::npos,
         "hover anywhere inside a function must resolve to that function");

  session.stop();
}

void TestHoverResolvesUsageToDeclaration() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenHoverUsageFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kHoverUsageFrame));
  std::string const usageHover =
      WaitForOutputContaining(output, "\"id\":\"hvu\"");
  Expect(usageHover.find("\"id\":\"hvu\"") != std::string::npos,
         "usage hover request must receive a response");
  Expect(usageHover.find("dim as Vector2 v1 = .vertices(i)") !=
             std::string::npos,
         "hover on a usage must show the declaring Dim with its type and "
         "initializer");
  Expect(usageHover.find("Local variable in Sub `ProcessSectorPhysics`, "
                         "inside the `for` block.") != std::string::npos,
         "hover must describe the variable's kind and the block it lives in");
  Expect(usageHover.find("`if`") == std::string::npos,
         "hover must never surface a scope-block node as the symbol");

  input->append(MakeLspFrame(kHoverDeclFrame));
  std::string const declHover =
      WaitForOutputContaining(output, "\"id\":\"hvd\"");
  Expect(declHover.find("dim as Vector2 v1 = .vertices(i)") !=
             std::string::npos,
         "hover on the declaration itself must show the same symbol info");

  input->append(MakeLspFrame(kHoverCounterFrame));
  std::string const counterHover =
      WaitForOutputContaining(output, "\"id\":\"hvc\"");
  Expect(counterHover.find("for i as integer = 0 to 3") != std::string::npos,
         "hover on a for-loop counter must show the header with its type");
  Expect(counterHover.find("Loop counter in Sub `ProcessSectorPhysics`.") !=
             std::string::npos,
         "hover must label the counter as a loop counter in its procedure");

  input->append(MakeLspFrame(kDidOpenResolveFrame));
  Expect(WaitForPublishedUri(output, 2).empty() == false,
         "second didOpen must publish diagnostics");
  input->append(MakeLspFrame(kModuleDimHoverFrame));
  std::string const moduleHover =
      WaitForOutputContaining(output, "\"id\":\"hvm\"");
  Expect(moduleHover.find("Module-level variable") != std::string::npos,
         "hover on a module-level usage must label it module-level");

  session.stop();
}

void TestHoverShowsMemberAccess() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenMemberHoverFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  // `.walls` — the reported regression: the member of the with-target, not
  // the enclosing sub's signature.
  input->append(MakeLspFrame(kMemberHoverWallFrame));
  std::string const wallHover =
      WaitForOutputContaining(output, "\"id\":\"hmm\"");
  Expect(wallHover.find("\"id\":\"hmm\"") != std::string::npos,
         "member hover request must receive a response");
  Expect(wallHover.find("as Wall walls(10)") != std::string::npos,
         "member hover shows the field's declaration line (type + name)");
  Expect(wallHover.find("Member of `map` (`Map`).") != std::string::npos,
         "a with-implicit member names the with-target variable and its type");
  Expect(wallHover.find("sub run(") == std::string::npos,
         "member hover must never fall back to the enclosing sub signature");

  // `.sectors(secIndex).floorHeight` — indexed chain lands on the element
  // type, so the owning type is Sector, not Map.
  input->append(MakeLspFrame(kMemberHoverFloorFrame));
  std::string const floorHover =
      WaitForOutputContaining(output, "\"id\":\"hmf\"");
  Expect(floorHover.find("as single floorHeight") != std::string::npos,
         "chained member hover shows the leaf field's declaration");
  Expect(floorHover.find("Member of `Sector`.") != std::string::npos,
         "a deep member names its owning type (indexed element type)");

  // `w.v1` — plain local variable base names the variable and its type.
  input->append(MakeLspFrame(kMemberHoverLocalFrame));
  std::string const localHover =
      WaitForOutputContaining(output, "\"id\":\"hml\"");
  Expect(localHover.find("as integer v1, v2") != std::string::npos,
         "member hover shows the whole field list of the declaration line");
  Expect(localHover.find("Member of `w` (`Wall`).") != std::string::npos,
         "a variable member names the base variable and its type");

  session.stop();
}

void TestKeywordHoverInsideProcedureShowsWikiLink() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenKeywordBodyFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  auto hover = [&](char const *frame, char const *id) {
    input->append(MakeLspFrame(frame));
    return WaitForOutputContaining(output, id);
  };

  // The reported regression: hovering a keyword inside a body used to show the
  // enclosing procedure's signature. It must show the keyword's wiki link.
  std::string const dimHover = hover(kKeywordBodyDimFrame, "\"id\":\"hkd\"");
  Expect(dimHover.find("\"id\":\"hkd\"") != std::string::npos,
         "keyword hover request must receive a response");
  Expect(dimHover.find("KeyPgDim") != std::string::npos,
         "the `dim` keyword inside a body shows its wiki link");
  Expect(dimHover.find("sub run(m as Map") == std::string::npos,
         "the `dim` keyword inside a body must not show the enclosing sub");

  std::string const withHover = hover(kKeywordBodyWithFrame, "\"id\":\"hkw\"");
  Expect(withHover.find("KeyPgWith") != std::string::npos,
         "the `with` keyword inside a body shows its wiki link");
  Expect(withHover.find("sub run(m as Map") == std::string::npos,
         "the `with` keyword inside a body must not show the enclosing sub");

  std::string const endHover = hover(kKeywordBodyEndFrame, "\"id\":\"hke\"");
  Expect(endHover.find("KeyPgEnd") != std::string::npos,
         "the `end` keyword inside a body shows its wiki link");
  Expect(endHover.find("sub run(m as Map") == std::string::npos,
         "the `end` keyword inside a body must not show the enclosing sub");

  // The opener of a declaration is not a generic keyword: hovering the `sub`
  // word of its own header must keep showing the procedure signature.
  std::string const subHead = hover(kKeywordBodySubHeadFrame, "\"id\":\"hks\"");
  Expect(subHead.find("sub run(m as Map") != std::string::npos,
         "the `sub` word of its own header still shows the signature");

  session.stop();
}

void TestMemberHoverResolvesCrossFileTypeOutsideClosure() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  // The include names a file that does not exist, so the type can only be
  // reached through the workspace byKey fallback, not the include closure.
  input->append(MakeLspFrame(kDidOpenCrossTypeMainFrame));
  input->append(MakeLspFrame(kDidOpenCrossTypeWorldFrame));
  Expect(WaitForPublishedUri(output, 2).empty() == false,
         "both didOpens must publish diagnostics");

  input->append(MakeLspFrame(kCrossTypeMemberFrame));
  std::string const hover = WaitForOutputContaining(output, "\"id\":\"hxm\"");
  Expect(hover.find("\"id\":\"hxm\"") != std::string::npos,
         "member hover request must receive a response");
  Expect(hover.find("as Wall walls(10)") != std::string::npos,
         "a member whose type is outside the include closure still resolves to "
         "the field declaration");
  Expect(hover.find("Member of `m` (`Map`).") != std::string::npos,
         "the outside-closure member names the base variable and its type");
  Expect(hover.find("dim as integer walls") == std::string::npos,
         "the member must not fall back to the colliding local variable");
  Expect(hover.find("sub run(") == std::string::npos,
         "the member must not fall back to the enclosing sub signature");

  session.stop();
}

void TestMemberHoverKeywordNamedMember() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  input->append(MakeLspFrame(kDidOpenKeywordMemberFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  // `t.name` at module level: the reserved word is a member, so hover shows
  // the field — not the `Name(...)` intrinsic page the lexer kind would
  // otherwise produce.
  input->append(MakeLspFrame(kKeywordMemberHoverModuleFrame));
  std::string const moduleHover =
      WaitForOutputContaining(output, "\"id\":\"hkm\"");
  Expect(moduleHover.find("\"id\":\"hkm\"") != std::string::npos,
         "keyword-member hover request must receive a response");
  Expect(moduleHover.find("as string name") != std::string::npos,
         "hovering a reserved-word member shows its declaration line");
  Expect(moduleHover.find("Member of `t` (`Mytype`).") != std::string::npos,
         "the keyword member names the base variable and its type");
  Expect(moduleHover.find("FreeBASIC intrinsic") == std::string::npos,
         "a reserved-word member must not show the intrinsic page");
  Expect(moduleHover.find("www.freebasic.net") == std::string::npos,
         "a reserved-word member must not show the keyword wiki link");

  // Hovering the member's own declaration behaves like an identifier field.
  input->append(MakeLspFrame(kKeywordMemberHoverDeclFrame));
  std::string const declHover =
      WaitForOutputContaining(output, "\"id\":\"hkd\"");
  Expect(declHover.find("as string name") != std::string::npos,
         "hovering the declaration shows the same field info");
  Expect(declHover.find("Field of type `Mytype`.") != std::string::npos,
         "the reserved-word declaration is a field of its type");
  Expect(declHover.find("FreeBASIC intrinsic") == std::string::npos,
         "the declaration must not show the intrinsic page");

  // Inside a sub body, `v.name` still names the member — never the enclosing
  // procedure's signature.
  input->append(MakeLspFrame(kKeywordMemberHoverSubFrame));
  std::string const subHover =
      WaitForOutputContaining(output, "\"id\":\"hks\"");
  Expect(subHover.find("as string name") != std::string::npos,
         "the in-sub keyword member shows its declaration line");
  Expect(subHover.find("Member of `v` (`Mytype`).") != std::string::npos,
         "the in-sub keyword member names the base variable and its type");
  Expect(subHover.find("sub s(") == std::string::npos,
         "the keyword member must not fall back to the enclosing sub");

  session.stop();
}

void TestMemberHoverFallsBackToOwningVariable() {
  lsp::NullLog log;
  lsp::LanguageSession session(log);
  auto input = std::make_shared<FeedableIStream>();
  auto output = std::make_shared<StringOStream>();

  FreeBasicServer server(session);
  server.registerHandlers();
  session.start(input, output);

  // When the declared type of the chain root is unknown anywhere, the hover can
  // still say the access is a member of the owning variable — never a colliding
  // local or the enclosing routine.
  input->append(MakeLspFrame(kDidOpenHovUnknownTypeFrame));
  Expect(WaitForPublishedUri(output, 1).empty() == false,
         "didOpen must publish diagnostics");

  input->append(MakeLspFrame(kHoverUnknownTypeMemberFrame));
  std::string const hover = WaitForOutputContaining(output, "\"id\":\"hxu\"");
  Expect(hover.find("\"id\":\"hxu\"") != std::string::npos,
         "member hover request must receive a response");
  Expect(hover.find("Member of `map`.") != std::string::npos,
         "an unknown-type with-implicit member names the with-target");
  Expect(hover.find("Local variable") == std::string::npos,
         "the unknown-type member must not fall back to the colliding local");
  Expect(hover.find("sub run(") == std::string::npos,
         "the unknown-type member must not fall back to the sub signature");

  // A known type with a missing field still names the owning variable and the
  // type, instead of guessing wrong.
  input->append(MakeLspFrame(kDidOpenHovMissingMemberFrame));
  Expect(WaitForPublishedUri(output, 2).empty() == false,
         "second didOpen must publish diagnostics");

  input->append(MakeLspFrame(kHoverMissingMemberFrame));
  std::string const hover2 = WaitForOutputContaining(output, "\"id\":\"hxq\"");
  Expect(hover2.find("Member of `m2` (`Map`).") != std::string::npos,
         "a missing member still names the owning variable and its known type");
  Expect(TailAfter(hover2, "\"id\":\"hxq\"").find("Local variable") ==
             std::string::npos,
         "the missing member must not fall back to a variable");

  session.stop();
}

void TestHoverLinksKeywordDocs() {
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

  input->append(MakeLspFrame(kKeywordHoverFrame));
  std::string const response =
      WaitForOutputContaining(output, "\"id\":\"khh\"");

  Expect(response.find("\"id\":\"khh\"") != std::string::npos,
         "keyword hover request must receive a response");
  Expect(response.find("dim") != std::string::npos,
         "keyword hover must name the keyword");
  Expect(response.find("https://www.freebasic.net/wiki/KeyPgDim") !=
             std::string::npos,
         "keyword hover must link to the FreeBASIC wiki page");

  session.stop();
}

void TestHoverShowsIntrinsicSignature() {
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

  input->append(MakeLspFrame(kIntrinsicHoverFrame));
  std::string const hov = WaitForOutputContaining(output, "\"id\":\"ihv\"");
  Expect(hov.find("Left$( str As String, n As Integer ) As String") !=
             std::string::npos,
         "hover on Left$ must show the catalog signature");
  Expect(hov.find("https://www.freebasic.net/wiki/KeyPgLeft") !=
             std::string::npos,
         "intrinsic hover must link to the FreeBASIC wiki page");

  session.stop();
}
} // namespace

namespace fbtest {

void RunHoverTests() {
  RUN_TEST(TestHoverShowsSignatureAndDoc);
  RUN_TEST(TestHoverResolvesUsageToDeclaration);
  RUN_TEST(TestHoverShowsMemberAccess);
  RUN_TEST(TestHoverShowsEnumMembers);
  RUN_TEST(TestKeywordHoverInsideProcedureShowsWikiLink);
  RUN_TEST(TestMemberHoverResolvesCrossFileTypeOutsideClosure);
  RUN_TEST(TestMemberHoverKeywordNamedMember);
  RUN_TEST(TestMemberHoverFallsBackToOwningVariable);
  RUN_TEST(TestHoverLinksKeywordDocs);
  RUN_TEST(TestHoverShowsIntrinsicSignature);
}

} // namespace fbtest
