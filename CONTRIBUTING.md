# Contributing

The server is early and the API is not stable. That cuts both ways: small
pull requests are easy to review and land quickly, and there is a lot of ground
still uncovered.

Read [`AGENTS.md`](AGENTS.md) before you start. It is the working agreement
for this repository, and most of the rules below are summaries of what it
already says.

## Contributions come with a disclosure

This project is a testbed for agentic coding. Most of the code was written by
an AI coding agent working under the human maintainer's account.

If an AI agent wrote or co-authored any part of your change, say so in the pull
request description, and name the agent or model if you know it. Nothing else
about a contribution changes: no special review standard, no extra paperwork,
and no legal significance either way. The disclosure exists so a reader knows
how much human judgment went into the diff.

A human reviews every change before it lands, including the ones an agent
produced. The agent runs the tools; the maintainer owns the commits.

## No CLA, no sign-off

Contributions land under the same terms as the rest of the project:
GPL-3.0-or-later, as in [`LICENSE.md`](LICENSE.md). There is no contributor
license agreement and no `Signed-off-by` requirement. By opening a pull
request you agree to those terms for your contribution.

## Getting set up

```sh
git clone --recursive https://github.com/mudhairless/freebasicd
cd freebasicd
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Requirements: CMake 3.16+, a C++17 compiler, and GNU gettext for the message
catalogs. Without gettext the build still succeeds and skips the catalogs.
`fbc` 1.10.2 is not needed to build, but it is the ground truth for anything
about FreeBASIC semantics.

The `--recursive` matters. `LspCpp` and `tomlplusplus` are submodules, and a
shallow clone without them will not configure.

## Before you open a pull request

Every one of these is a gate the project holds itself to, and CI holds it too:

```sh
# 1. all test suites green (15 as of this writing)
ctest --test-dir build --output-on-failure

# 2. formatting clean, no diff
clang-format --dry-run --Werror src/*.cpp src/*.h tests/*.cpp tools/*.cpp

# 3. regenerate anything generated that your change touched
cmake --build build --target grammar      # editors/freebasic.* from the catalog
cmake --build build --target po-template  # po/freebasicd.pot from the sources
```

The two generated trees have freshness tests. If you edit the keyword catalog in
`src/language.cpp` without regenerating the grammars, `grammar_checks` fails. If
you add a translatable literal without re-extracting the template,
`i18n_checks` fails.

Formatting is a hard gate, not a suggestion. If you run `clang-tidy --fix` and
then `clang-format -i` over the same lines, review the result: the fix output
is not formatter-clean on its own.

## Where things live

| Path | What belongs there |
|---|---|
| `src/lexer.cpp`, `src/parser.cpp` | Language surface. Nothing LSP-specific. |
| `src/language.cpp` | Keyword catalog, block closers, intrinsics, all as data |
| `src/resolve.cpp` | Name and member resolution, same-file and cross-file |
| `src/index.cpp` | The per-workspace in-memory index and include resolution |
| `src/session.cpp` | Every LSP handler, capability, and diagnostic |
| `src/code_actions.cpp` | Quick fixes, one function plus one table row |
| `src/settings.cpp` | `freebasicd.toml` |
| `src/i18n.cpp` | gettext plumbing |
| `tests/` | One ctest suite per subsystem, no test framework |
| `tools/` | Generators, not runtime code |
| `third_party/` | Vendored. Do not edit. |
| `editors/` | Generated. Do not edit; regenerate. |

`FreeBASIC.md` is the source of truth for language facts. If your change
touches how FreeBASIC behaves, update that file in the same pull request, and
keep its §12 list of known divergences from real `fbc` current. `PLAN.md` holds
the roadmap: check §4 for the open gaps before you start, so you do not
duplicate work. It is for work still to do — a change that closes a gap deletes
that gap and adds a dated entry to `CHANGELOG.md` instead, in the same wave.

## Adding a quick fix

One function plus one row in the registry table. A provider is a pure function
of a diagnostic and a context object, and it must never guess: offer a candidate
only when the context's own seams accept it. `AGENTS.md` §Quick fixes has the
contract. Fixes ship as LSP `CodeAction`s carrying an `edit`, never as
`Command`s.

## Patching LspCpp

Do not patch the vendored submodule in place. LspCpp upstream is
[kuafuwang/LspCpp](https://github.com/kuafuwang/LspCpp); our fork is
[mudhairless/LspCpp](https://github.com/mudhairless/LspCpp) on the branch
`lsp-3.17-completions`, which carries the missing LSP 3.17 types and
serialization fixes we need. A change there means a commit on that branch and a
pin bump in this repository, and it needs write access to the fork.

If you need something from LspCpp that is not there, open an issue describing
the missing type or the wrong wire shape. That is genuinely useful and does not
need fork access.

## Translations

The message template is `po/freebasicd.pot`; the 29 `po/<lang>.po` files carry
the translations. English is the msgid language, so there is no `en.po`.

```sh
cmake --build build --target po-template   # re-extract the template
cmake --build build --target update-po     # merge it into the po files
```

Then fill in the `msgstr` lines in your language's file. The build compiles
every catalog with `msgfmt --check`, so a format-string or placeholder mismatch
is a build error rather than a crash at runtime.

One rule is enforced by a test and is not negotiable: FreeBASIC keywords and
the word "FreeBASIC" itself are never translated. A user reads those in the
source.

## Version numbers

Do not touch the version in `CMakeLists.txt`. It follows semantic versioning
and moves only when a release ships. See `AGENTS.md` §Versioning.

## Reporting bugs

There are four issue templates, and each asks for the evidence that kind of
report needs:

- **Bug report** — the server gets something wrong. Asks which feature, the
  minimal `.bas` file, the dialect, the exact diagnostic, the startup line
  that carries the version, and your client. A missing feature is often a
  client that never sent the request, so the client matters.
- **fbc divergence** — the server and the real `fbc` disagree about FreeBASIC
  semantics. Asks for the compiler's own output with its error number, your
  `fbc` version, the construct in FreeBASIC's words, and whether
  `FreeBASIC.md` §12 already documents it.
- **Build or CI failure** — it does not configure, compile, or pass its tests.
  Asks for the first error, the toolchain, whether gettext is present, and
  `git submodule status`, which is the usual cause.
- **Feature request** — something it should do. Asks what you do instead
  today, which is usually what decides whether it gets built.

FreeBASIC semantics questions are the most useful kind of issue, but a
question about the language itself belongs on the
[FreeBASIC wiki](https://www.freebasic.net/wiki/DocToc). If the server and the
wiki disagree, that is a divergence report.

Security vulnerabilities do not go in issues. See [`SECURITY.md`](SECURITY.md).
