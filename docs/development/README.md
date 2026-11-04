# Development documents

`AGENTS.md` contains the short, always-on working agreement. These documents
hold the details an agent or contributor needs only for a particular kind of
change.

- **[Dependencies](dependencies.md)**: read before changing a submodule, a
  dependency pin, dependency CMake, or the LspCpp fork.
- **[Architecture](architecture.md)**: read before changing the session
  boundary, workspace index, configuration handling, concurrency model, or
  an LSP feature module.
- **[Testing and concurrency](testing.md)**: read before adding a session
  feature suite, debugging a hang or silent test failure, changing test
  fixtures, or changing code that crosses a thread or snapshot boundary.

The language reference remains [`FreeBASIC.md`](../../FreeBASIC.md). The
roadmap remains [`PLAN.md`](../../PLAN.md), and landed work is recorded in
[`CHANGELOG.md`](../../CHANGELOG.md).
