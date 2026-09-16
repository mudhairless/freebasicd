# Suppressed clang-tidy checks

These checks are intentionally disabled in `.clang-tidy` by convention, not
because they were unfixable.  Counts are approximate and come from the last
full run over the eight `src/*.cpp` files (2026-09).  Re-evaluate each on a
quarterly basis or when the codebase shrinks / restructures.

---

## portability-avoid-pragma-once

**Looks for:** Use of `#pragma once` instead of traditional include guards
(`#ifndef` / `#define`).  The check flags a file's *first* line.

**Why suppressed:** Every header in this repo uses `#pragma once`.  Both are
well-supported by every active compiler; `#pragma once` avoids guard-name
collisions without contributor bookkeeping.  The cost of guarding it is nil.

**Hit count:** ~8 (every project header, always).

---

## readability-identifier-length

**Looks for:** Identifiers shorter than a configurable minimum (default 3)
outside an allowlist that covers loop indices (`i`, `j`, `k`).  Classifies
one-letter and two-letter locals as "hard to search for" or "hard to
remember".

**Why suppressed:** The codebase intentionally uses terse locals near the
metal: `char c`, `size_t i`, `auto it`, `string_view t`, `bool ok`, `int n`.
These show up in tight loops and token scanners where longer names add clutter
without helping the reader.  They are easy to search for *within* the
function scope they live in.

**Hit count:** ~195 (mostly loop counters, iterators, and peek/advance
scratch vars).

---

## readability-function-cognitive-complexity

**Looks for:** Functions exceeding a cyclomatic-complexity threshold
(default 25) by counting nested branches, `switch`/`case`, and
short-circuit operators.

**Why suppressed:** Informational only; the flagged functions (`lexNext`,
the parser's top-level `switch` dispatchers) are inherently branching state
machines where splitting into smaller functions would obscure the sequential
token model.  Complexity *at the structural level* is the nature of a lexer /
recursive-descent parser.

**Hit count:** ~9 (Lexer::lexNext, Parser::run, and a couple of
`handle*` methods).

---

## misc-include-cleaner

**Looks for:** Includes that are not directly used in the file, or symbols
that are used but not included by a direct `#include` (i.e. only available
transitively through an umbrella header).

**Why suppressed:** The project intentionally uses umbrella headers —
`src/symbols.h` includes all symbol declarations, `session.h` includes the
index and LspCpp bindings.  Strict include-what-you-use would force dozens of
per-type includes per file that do not match the current architecture.  This
is the single largest source of noise.

**Hit count:** ~135 (hits in every `.cpp` file; mostly `#include "symbols.h"`
and `#include <optional>`).

---

## misc-no-recursion

**Looks for:** Any function that calls itself, directly or indirectly,
flagging it as a potential stack-overflow risk.

**Why suppressed:** The parser is a recursive-descent over nested block
structure (the `blocks_` / `containers_` stacks).  The recursion depth is
bounded by the source text's nesting depth, which is the natural model.  An
explicit parse stack would add significant complexity with no safety gain.

**Hit count:** ~11 (`parseDocument` and a handful of `handle*` methods).

---

## misc-non-private-member-variables-in-classes

**Looks for:** Public or protected data members in a `class`, encouraging
them to be private with accessor functions.

**Why suppressed:** The flagged types (`Token`, `Symbol`, `ParseResult`,
`Block`, `BlockRow`, `DocsPage`) are plain data-holder structs where direct
field access *is* the design.  Accessor boilerplate would add nothing.

**Hit count:** ~7 (the POD structs listed above).

---

## bugprone-easily-swappable-parameters

**Looks for:** Functions whose signature has two or more parameters of the
same (or implicitly convertible) type, where the caller could silently swap
their order.

**Why suppressed:** The two hits involve the same type on consecutive
parameters (two `std::string_view` or two sizes) whose meaning is obvious
from their names and immediate call context.  Wrapping them in a strong-typed
struct would be overkill for this codebase's size.  The checker's built-in
allowlist already covers `it`/`begin`/`end` pairs.

**Hit count:** ~2 (two functions with two same-typed params).

---

## clang-analyzer-optin.performance.Padding

**Looks for:** Structs where the field layout wastes memory due to alignment
padding, suggesting reordering to close the gaps.

**Why suppressed:** The flagged structs are small (one instance), not
heap-allocated in hot loops, and their field order reflects logical grouping
rather than ABI layout.  Reordering to shave a few bytes would harm
readability for negligible runtime gain.

**Hit count:** 1.

---

## performance-enum-size

**Looks for:** Enums that do not specify an underlying integer type,
implying the compiler picks `int` which may be larger than needed.

**Why suppressed:** The codebase enums are used as switch keys, not stored
in serialized structures; the default `int` size has no measurable ABI or
memory cost.  `enum class` scoping (which this checker does not address) is
the real safety guard.

**Hit count:** ~5 (SymbolKind, TokenKind, BlockKind, LangMode, plus a local
enum in the lexer).