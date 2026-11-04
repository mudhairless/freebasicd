# FreeBASIC LSP Server

Implement a cross-platform LSP 3.17 server for FreeBASIC in C++, over stdio.
The project root is a git repository. The default branch is `main`.

This file is the short, always-on working agreement. Detailed rules live in
linked documents. Read the linked document before starting a change in its
area; the summary here does not replace it.

## Read when relevant

- **Language facts, lexer, or parser:** read [`FreeBASIC.md`](FreeBASIC.md).
  It is the single source of truth for the language.
- **Dependencies, submodules, dependency CMake, or the LspCpp fork:** read
  [`docs/development/dependencies.md`](docs/development/dependencies.md).
- **Session boundary, index, roots, settings, quick fixes, code lens, pull
  diagnostics, message catalogs, or concurrency model:** read
  [`docs/development/architecture.md`](docs/development/architecture.md).
- **Session tests, test fixtures, hangs, ThreadSanitizer, or snapshot
  lifetimes:** read
  [`docs/development/testing.md`](docs/development/testing.md).
- **Roadmap and open gaps:** read [`PLAN.md`](PLAN.md). Finished work
  belongs in [`CHANGELOG.md`](CHANGELOG.md), not the plan.
- **clang-tidy conventions and suppressions:** read
  [`TIDY.md`](TIDY.md) with [`.clang-tidy`](.clang-tidy).

## Language rules

- Only the `fb` dialect is implemented. A `#LANG` directive or `$lang`
  metacommand records the selected dialect in `ParseResult.lang`. A non-`fb`
  file receives a best-effort `fb` parse and one `lang-mode` Information
  diagnostic.
- Do not re-derive language data already encoded in `src/language.cpp`.
  Known divergences from real `fbc` belong in `FreeBASIC.md` section 12.
- Identifiers are case-insensitive. Build lookup keys with
  `canonicalKey(name, lang)` from `src/language.h`, never with
  `toLowerChars` alone. In `fb` mode the canonical key drops a trailing type
  suffix. The preprocessor define table is the one deliberate exception: it
  keeps the suffix on a `#define` name. See `FreeBASIC.md` section 12.1.
- `///` and `''` comment lines directly above a declaration are its hover
  documentation. See `FreeBASIC.md` section 5.

## Core architecture

- `lexer`, `parser`, and `language` are independent of LSP and work in byte
  offsets. `session` is the only system boundary and converts positions at
  that boundary. All LSP positions are UTF-16 code units.
- Parse and validate external input in `session`. Report a contradiction at
  the boundary instead of silently resolving it. A malformed value must not
  degrade the whole session.
- The workspace index is in memory only. An open buffer is never overwritten
  by a disk entry. Enforce that rule in `WorkspaceIndex::upsert`, where the
  replacement is committed, not only in callers.
- The server owns one index per workspace root. Handlers snapshot a
  `shared_ptr<WorkspaceIndex>` and hold it while raw pointers into its
  entries are in use. A result type that returns a pointer into a snapshot
  must also carry the pin that keeps the snapshot alive.
- LspCpp handlers can run concurrently. Guard shared state, take a lock only
  to snapshot what a handler needs, and build the response without holding
  the lock.
- `freebasicd.toml` is the configuration truth.
  `workspace/didChangeConfiguration` ignores its payload and rereads each
  root's file. Feature handlers honor the settings for their document and
  return the appropriate empty result when a feature is disabled.
- Advertise only implemented capabilities. Advertise
  `positionEncoding: "utf-16"`.

## Verification

A full build is part of every deliverable:

```sh
cmake --build build --clean-first --parallel
ctest --test-dir build --output-on-failure
```

Building a test target alone does not relink `freebasicd`, the binary an
editor launches.

When adding a `session_integration` feature file, register it in both
places: the target in `CMakeLists.txt` and its `Run<Feature>Tests()` call in
`tests/session_integration.cpp`. Missing the second registration fails
silently. Confirm the new tests appear in the `[ RUN ]` output.

Hosted CI covers Linux, macOS, and Windows differences that a local run
cannot. A green local run is not a substitute for those legs.

ThreadSanitizer is on demand, not part of the normal gate. Run it after
changing `src/index.h`, `src/index.cpp`, `src/session.cpp`, or code that
returns pointers into a snapshot. Zero warnings is the bar. Commands and
concurrency rules are in
[`docs/development/testing.md`](docs/development/testing.md).

## Formatting and clang-tidy

Changed C++ and header files must pass the formatting gate:

```sh
git ls-files '*.cpp' '*.h' | grep -v '^third_party/' \
  | xargs clang-format --dry-run --Werror
```

Use the system-installed `clang-format`. If it disagrees with the version
pinned in CI, tell the user. A version sync is a standalone commit with no
`CHANGELOG.md` entry.

clang-tidy is on demand, not part of the milestone gate. Recreate the
throwaway compile database rather than trusting a stale one:

```sh
cmake -S . -B build-tidy -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_CXX_COMPILER=clang++
clang-tidy -p build-tidy --quiet <changed src/*.cpp files>
```

Bug-class checks are warnings as errors. Keep `src/` zero-diagnostic under
the baseline when clang-tidy is run. Do not widen suppressions to hide a new
bug-class finding. Suppressions record deliberate conventions, each with a
reason in `.clang-tidy` and `TIDY.md`. After `clang-tidy --fix`, run
`clang-format -i` over the touched files and review the diff.

## Versioning

The version follows semantic versioning. Its only source is
`project(freebasicd ... VERSION <x.y.z>)` in `CMakeLists.txt`.

- Bump it only when a release ships. Feature, fix, refactor, and document
  commits do not change it.
- A release retitles `CHANGELOG.md`'s `## [Unreleased]` section with the
  version and date, then opens a new empty `## [Unreleased]` section.
- Before 1.0, MINOR is the feature wave and PATCH is fixes and documents. A
  breaking protocol or configuration change takes MINOR.
- Stay below 1.0 until the LSP surface, `freebasicd.toml` keys, and install
  layout are stable.

## Git workflow

- Commit automatically when a milestone meets its acceptance criteria: its
  tests pass, the full build passes, changed C++ files are format-clean, and
  the deliverable is complete. Never commit half-finished or failing work.
- Close a finished milestone in the same wave:
  1. Add a dated entry at the top of `CHANGELOG.md`'s `## [Unreleased]`
     section. Record deviations from the sketch and defects or lessons that
     cannot be reconstructed from the diff.
  2. Delete the milestone section and table row from `PLAN.md`.
  3. Refresh stale references in `PLAN.md` and, when advertised behavior
     changed, the capability table in `README.md`.
- Do not leave a finished milestone marked `next`. Do not commit one without
  its changelog entry.
- Review `git status` and `git diff` before committing. Stage only intended
  files. Use a short conventional `scope:` style commit message.
- Push only when asked.
