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

## readability-magic-numbers

**Looks for:** Numeric literals used directly in code, on the theory that
an unexplained number hides a meaning a named constant would state.

**Why configured, not suppressed:** The check stays enabled, because the
theory is right for values that carry a meaning of their own (the fbc
probe's timeout became a named constant the day the check flagged it).
What it may not flag is set once, in `.clang-tidy`'s `CheckOptions`, so no
call site needs a comment: small counts and the decimal base (`0;1;2;5;10`),
the nanosecond/microsecond/millisecond unit ratios of the timing code
(`1000;100000;1000000`, and the `10.0` display threshold that pairs with
them), and powers of two (buffer sizes, bit widths). A value with a real
meaning still reports, and the remedy is a named constant, never a new
entry on the ignore list and never a per-site suppression.

The fbc message numbers are no longer literals at report sites: a
diagnostic carries an `FbcDiagId`, and the number is resolved through a
version-range table selected by the probed compiler version. The resultId
fold salts in `session.cpp` are likewise named constants beside
`diagnosticsResultId`. What remains flagged is a small set of
self-describing literals — the radix bases of the literal parsers, the
integer saturation bound, the `typeof` needle length, the shift bound, and
the reserve slack — where the number is the clearest statement of the
operation. They stay flagged by decision; the rule above still holds for
every other value.

**Hit count:** 24 before configuration, across the four files of the
2026-11-03 run; the 2026-11-05 pass named 54 of the 68 remaining sites
(fbc identities plus resultId salts), leaving only the self-describing
literals above.

---

## readability-trailing-comma

**Looks for:** Multi-line initializer lists whose last element has no
trailing comma.

**Why suppressed:** Initializer layout in this repo belongs to
clang-format, whose output does not add trailing commas to the two-field
braced initializers this codebase is full of. The check would flag new
code as fast as it is written, and the comma carries no meaning the layout
does not already show.

**Hit count:** 12 in the 2026-11-03 run.

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

## misc-include-cleaner

**Looks for:** Symbols used in a file whose providing header is not
directly included there, and includes no symbol in the file needs.

**Why suppressed:** This codebase's headers are self-contained by design:
a file includes the headers of the modules it talks to, and those headers
provide what they name. The check instead demands a direct include for
every standard-library symbol a header already brings in (`std::string`
via the project's own header was its first hit), which churns every file
whenever a header's internals move and fights the convention the rest of
the tree follows. Include discipline here is reviewed by eye, not by this
check.

**Hit count:** 127 in the 2026-11-03 run, the large majority of all
findings that day.

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

## bugprone-command-processor

**Looks for:** Calls to `std::system` and friends, which run a command
through the platform shell.

**Why suppressed at one site only:** `probeFbcVersion`
(`src/fbc_version.cpp`) runs the user's own configured compiler with
`-version` to learn its version at workspace adoption. The executable path
comes from the user's own `freebasicd.toml` (or their `PATH`) and is
shell-quoted by `shellQuote` before the command is built; nothing a
document or a client sends reaches the command line. There is no portable
standard-library way to spawn a process with a timeout across Linux,
macOS, and MSVC, and the probe already carries the timeout thread for the
same reason. The check stays enabled project-wide; the suppression is a
`NOLINTNEXTLINE` on that one call, so a second `std::system` anywhere else
still fails the run.

**Hit count:** 1 (the fbc version probe).

---

## bugprone-throwing-static-initialization / cert-err58-cpp

**Looks for:** Storage-duration (static / `thread_local`) initialization that
may throw, which cannot be caught.

**Why suppressed at one site only:** `arena` (`src/selection_lsp.cpp`) is
deliberately `thread_local`: one thread, one arena, no lock, and the nodes
are consumed by a single synchronous write on the handler's own thread before
anything can reuse them (the comment above the declaration states the
lifetime model). A default-constructed `std::deque` allocates nothing, so
the throw-during-init risk the check guards against cannot bite here. The
checks stay enabled project-wide; the suppression is a `NOLINTNEXTLINE` on
that one declaration, with a `TODO` to reevaluate it if the arena's thread
model changes.

**Hit count:** 1 (the selection arena).

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