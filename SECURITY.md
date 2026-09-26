# Security policy

## Reporting a vulnerability

Report it privately through GitHub's security advisories:

**`github.com/mudhairless/freebasicd/security/advisories/new`**

That opens a private thread between you and the maintainer. It stays private
until a fix ships. Please do not file a public issue for a vulnerability, and
please do not open a pull request containing one: a public issue tells every
reader about the bug before there is a patched version to upgrade to.

The `Security` tab is only active once the repository is public, so if the link
404s, email `ebben.feagan@gmail.com` instead.

Include what a reporter can usually give us:

- The FreeBASIC source, or the smallest input that reproduces it. For a
  language server, the triggering `.bas` file matters more than the editor.
- What you expected and what happened, including the exact diagnostic or
  crash.
- The server version (the startup line on stderr prints it), the platform, the
  compiler, and the client you were using.
- Whether the project has a `freebasicd.toml`, and its contents if so.

## What counts as a vulnerability

This server reads files the editor tells it to read and runs no external
compiler, so the interesting classes are narrow. In scope:

- Arbitrary file read or write, including any path that escapes the workspace
  root or follows a symlink out of it. Include resolution is the place to look.
- Code execution, from any input, including a crafted source file, a crafted
  `freebasicd.toml`, or a malicious message on the LSP stream.
- A crash, hang, or unbounded memory growth reachable from opening or editing
  a file. A stack overflow or an infinite loop in the parser or resolver
  qualifies; a slow parse of a genuinely huge file does not, unless it never
  finishes.
- Anything that leaks memory from a long session, if it is reproducible.
- Incorrect handling of the LSP stream itself, such as a request that makes the
  server desynchronize from the client and mis-attribute later diagnostics.

Out of scope, and not treated as security bugs:

- Wrong diagnostics, wrong completions, or wrong hover text. They are bugs,
  and you can file them as issues.
- Crashes triggered only by a client sending a malformed message that no
  conformant client would send.
- Denial of service through resources the user chose, such as indexing a very
  large tree on a small machine.
- Vulnerabilities in the vendored dependencies themselves. Report those
  upstream; we will bump the pin.

## Supported versions

| Version | Supported |
|---------|-----------|
| 0.7.x (unreleased, the current `main`) | Yes, fixes land on `main` |
| Anything older | No such version exists yet |

There is no tagged release at the time of writing. Until 0.7.0 ships, treat
`main` as the only supported thing and assume it changes under you.

## What to expect

- An acknowledgement within a few days. This is a volunteer project with one
  maintainer and no paid support, so there is no response-time guarantee and no
  service level agreement.
- A fix and a release, or an explanation of why the report is not a security
  issue. Either way you will hear back.
- Credit in the release notes, unless you ask not to be named.

## Disclosure

Give the maintainer a reasonable window to ship a fix before disclosing
publicly. If a fix is going to take longer than you can wait, say so in the
report and the window can be shortened. The maintainer will not ask you to keep
a disclosure quiet once a fix is out.
