/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// FreeBASIC language model shared by the lexer, parser, index, and session.
// Everything here is LSP-agnostic and works in byte offsets; the session
// converts to/from LSP positions only at the protocol boundary.

namespace fblang {

struct SourceRange {
  uint32_t beg = 0;
  uint32_t end = 0;
};

// A `#include [once] "target"` directive as written in the source. Byte-offset
// ranges; `target` spans the filename literal (quotes excluded). Resolution to
// an absolute path happens at the index boundary (resolveIncludeTarget).
struct IncludeDirective {
  SourceRange line;    // whole `#include ...` line
  SourceRange target;  // filename literal range (quotes excluded)
  std::string literal; // filename as written, case preserved
  bool once = false;   // `#include once`; `#pragma once` is recorded at the
                       // document/file level, not on the edge
};

// One usage of a symbol. `moduleScope` = true when the usage sits at module
// level (inside no block), i.e. the site is exposed to the #include closure
// for cross-file resolution.
struct Occurrence {
  SourceRange range;
  bool moduleScope = true;
};

enum class SymbolKind {
  Sub,
  Function,
  Property,
  Constructor,
  Destructor,
  Operator,
  Type,
  Union,
  Enum,
  Namespace,
  Scope,
  Const,
  Dim,
  Label,
  Parameter,
  Variable
};

// Member visibility as gated by an access section inside a TYPE body
// (`Private:`, `Public:`, `Protected:` — FreeBASIC.md §7 Access sections).
// Members default to Public; a section gates every member declaration after
// it until the next section. Only a TYPE body ever *changes* it — Union
// bodies reject access sections (fbc: syntax error) and a plain enum has
// none. An anonymous block does not start a new one either: it inherits the
// section in force where it is written, so its fields carry the enclosing
// `Private:` (FreeBASIC.md §7).
enum class Access { Public, Private, Protected };

// Per-document symbol. Ranges are byte offsets into the source buffer.
struct Symbol {
  // Display name (original case + suffix char). A declaration block written
  // without a name gets `<anonymous <keyword>>` here — display only.
  std::string name;
  // Canonical lookup key = lowercase name incl. suffix. Empty for a
  // nameless block (and for a Scope): that emptiness is the parser's
  // unnamed-declaration signal, so `name`'s placeholder never fills it in.
  std::string key;
  SymbolKind kind = SymbolKind::Variable;
  SourceRange range; // whole construct (SUB ... END SUB); decls: the statement
  SourceRange selection; // name token, or the opener keyword when the
                         // declaration has no name (its selection must still
                         // fall inside `range` — LSP requires it)
  std::string signature; // readable declaration header (for hover/details)
  std::string doc;       // /// or '' doc-comment block directly above
  std::vector<Symbol> children;

  // M5 occurrence projection. `moduleScope` (true for file roots) marks
  // declarations whose key is cross-file reachable through the #include
  // closure; `occurrences` holds every usage site that resolved to this
  // symbol at analyze time, sorted by `range.beg` with the name token
  // itself excluded. Open buffers populate both; the disk cache stores them.
  bool moduleScope = false;
  std::vector<Occurrence> occurrences;

  // M7 storage tagging (FreeBASIC.md §8/§12.2): true only for a module-level
  // `Dim`-kind declaration carrying the `Shared` modifier (`Dim Shared`,
  // `Redim Shared`, `Common Shared`, `[Static] Var Shared`). Module-scope
  // plain `Dim`/`Common` stay false. Inside any block, module-scope Dim-kind
  // candidates require `shared`; at module level everything is visible.
  // Procedure/type/enum/const roots are storage-less and never gated.
  bool shared = false;

  // True for a `for <name> as <type> = ...` loop counter: a Dim local to the
  // loop, declared in the header. fbc ground truth (FreeBASIC.md §8): the
  // counter is invisible after `next`, and a header *without* `as` reuses an
  // existing declaration (undeclared is error 42), so only the `as` form
  // declares. Distinguishes the iterator from a plain Dim in the loop body.
  bool loopVar = false;

  // True on an Enum root declared `enum <name> explicit` (FreeBASIC.md §8,
  // KeyPgEnum): its members are reachable only through qualified access
  // (`Name.member`), never as bare module names. A plain `enum <name>`
  // publishes each member as a module-scope constant.
  bool explicitEnum = false;

  // Member visibility (Access enum above). Stamped by the parser when a
  // TYPE-body member is captured: the container's current access section.
  // Non-public members are offered by completion only from inside a member
  // procedure of the same type, and `resolveMemberAccess` still resolves them
  // (hover/references do not gate) — fbc reports error 202 on outside access.
  Access access = Access::Public;

  // M15 type graph. Two edges, both of which the parser had no room for, and
  // both of which a type hierarchy and a go-to-implementation need.

  // The type this one extends: `type derived extends base` records base's
  // lookup key here. `Extends` is FreeBASIC's *only* inheritance form — there
  // is no `Type : base` (fbc rejects it even under `-lang qb`) and no
  // `interface` keyword, so a type has at most one base and there are no
  // interfaces to model. `union u extends a` is the same edge. Empty for a
  // type that extends nothing, for every other kind, and for a malformed
  // opener line. fbc has no forward base references, so a base that does not
  // resolve is a real defect rather than a form to answer for.
  std::string extendsKey;

  // The type whose member procedure this declaration implements:
  // `sub t.go()` records owner `t` and takes `go` as its own name and key.
  // FreeBASIC declares a member procedure inside the type and *defines* it at
  // module level, qualified by the type name — fbc error 17 rejects a
  // definition inside the type body. Empty for a module-level declaration
  // that is not dot-qualified, and for the `declare` side, which is a plain
  // child of the type. The *member's* key, not the type's, is this symbol's
  // `key`, so `sub t.go()` no longer claims the key `t` and collides with
  // `type t`. Constructor/Destructor are deliberately excluded: their
  // implementation is spelled `constructor t()` with no dot, which a parser
  // cannot tell from a module constructor (FreeBASIC.md §12).
  std::string ownerKey;

  // The owner's spelling as written — `Sub T.proc()` records `T`, where
  // `ownerKey` records `t`: the same name/key split every other declaration
  // has. `ownerKey` is what resolution looks the type up by; `ownerName` is
  // what an *outline* entry shows, so the implementation reads `T.proc`
  // instead of an unqualified `proc` at file level with nothing to say whose
  // it is. Empty exactly when `ownerKey` is.
  std::string ownerName;
};

enum class Severity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct Diagnostic {
  SourceRange range;
  Severity severity = Severity::Error;

  // The parser's own routing key for this diagnostic, stable per *kind* of
  // problem (`unterminated-block`, `stray-closer`, ...). Quick fixes and the
  // corpus goldens key on it, and it is deliberately coarse: splitting a
  // family onto fbc's numbers changes `fbcError` below, never this string, so
  // a fix registered once keeps covering every number the family can report.
  // When `fbcError` is 0 this is also the code the client sees.
  std::string code;

  // fbc's own message number for this diagnostic (its `error N`), or 0 for a
  // diagnostic fbc has no number for (our conventions, and every preprocessor
  // shape: fbc's catalog covers neither). When set, the session reports the
  // code as `fbcCode(Error, fbcError)` ("fbc error: 42") and links
  // `codeDescription.href` to the wiki page that documents the whole catalog —
  // `code` above stays the routing key, so the two are not the same field by
  // design. The number is keyed off the block kind at each report site, so it
  // names the *specific* closer fbc would name (125 EXPECTEDENDSUB, 13
  // EXPECTEDNEXT, ...) rather than the coarse family.
  int fbcError = 0;

  std::string message;

  // Where a *missing closer* belongs, when the parse knows: the parser records
  // the block's logical end — the statement its grammar could not accept, or
  // the closer that did not match — and the `unterminated-block` fix writes its
  // `END TYPE` there instead of at end-of-buffer (`range` deliberately stays on
  // the opener: the squiggle means "you forgot to close this", and the fix
  // needs the opener to name the closer). Unset means there was no evidence,
  // and the end of the buffer is the best available guess — a procedure body
  // accepts every statement, so EOF really is its answer.
  std::optional<std::uint32_t> closerAt;
};

// Block kinds used for block matching and block-based folding.
enum class BlockKind {
  Sub,
  Function,
  Property,
  Operator,
  Constructor,
  Destructor,
  Type,
  Union,
  Enum,
  Namespace,
  Scope,
  If,
  Select,
  For,
  While,
  Do,
  With,
  Extern,
  Asm,
  PreprocIf,
  PreprocMacro
};

struct ParseResult {
  std::vector<Symbol> roots;
  std::vector<Diagnostic> diagnostics;
  std::vector<SourceRange>
      blockRanges; // every nested block, useful for folding

  // Dialect the file declares via #LANG; "fb" when none or unsupported. Only
  // "fb" is implemented; other modes are parsed best-effort for now.
  std::string lang = "fb";
};

inline std::string toLowerChars(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    out.push_back(c);
  }
  return out;
}

} // namespace fblang
