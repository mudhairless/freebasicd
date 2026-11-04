# Testing and concurrency

Read this file before adding a session feature suite, debugging a hang or
silent test failure, changing test fixtures, or changing code that crosses a
thread or snapshot boundary.

## Test layout

Unit drivers live under `tests/` and run through CTest. The system `fbc`
compiler is available for checking ambiguous FreeBASIC constructs against
the real implementation.

`session_integration` drives `LanguageSession` with in-memory streams. It is
one executable made from one file per feature:

- Shared support lives in `tests/session_support.h` and
  `tests/session_support.cpp`, in namespace `fbtest`.
- Each feature file uses `namespace fbtest` and exposes one runner named
  `Run<Feature>Tests()`.
- `tests/session_integration.cpp` contains `main()` and calls each runner in
  a fixed order.

Keep wire frames and helpers used by only one feature in that feature file,
immediately above its tests. Adding a test should touch one file: add the
test function and one `RUN_TEST` line to that file's runner.

### Registering a new feature file

Register it in both places:

1. Add the source to the `session_integration` target in `CMakeLists.txt`.
2. Declare and call `Run<Feature>Tests()` in
   `tests/session_integration.cpp`.

Only the first omission fails loudly. If the runner is missing from the
driver, the file can compile and link while its tests never run. The suite
can then print zero failures after testing less than it did before.

After adding a file, search the driver for the runner name and confirm that
the new tests appear in the `[ RUN ]` output. A green run alone is not proof
that the new file is wired in.

Keep `session_integration` as one CTest suite. Its `[ RUN ]` and `[ DONE ]`
markers and final completion line describe one process. CTest timeout output
also captures one stream for that process. Splitting it into many
executables would make a crash log harder to use and create per-file timeout
budgets that do not help diagnose the failure. One file per feature is a
source layout choice. One executable is a diagnostics choice.

## Full builds and CI

A full build is part of every deliverable. The unit drivers,
`session_integration`, and `freebasicd` are separate targets. Building tests
alone does not relink the server binary an editor launches.

Before accepting or committing a change, build the whole tree:

```sh
cmake --build build --clean-first --parallel
ctest --test-dir build --output-on-failure
```

Use a clean build when stale objects could be reused. At minimum, build the
`freebasicd` target when a change affects the server binary.

Hosted CI builds and tests on Linux with GCC and Clang, macOS with
AppleClang, and Windows with MSVC. It also installs into a scratch prefix and
checks that tree with `tools/check_install_tree.cmake`. A local green run
does not cover the macOS or Windows differences.

## ThreadSanitizer

ThreadSanitizer is on demand, not part of the normal gate. CTest cannot see
the main data-race risk in this server: handler threads read an index
snapshot while the background scan replaces entries in the same index.

Run the integration driver under ThreadSanitizer after changing:

- `src/index.h` or `src/index.cpp`
- `src/session.cpp`
- Any code that returns a raw pointer into an index snapshot

```sh
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS="-fsanitize=thread -g" \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan --target session_integration --parallel
./build-tsan/session_integration 2>&1 | grep -c 'WARNING: ThreadSanitizer'
```

Zero warnings is the bar.

### Snapshot lifetime rules

A result type that returns a pointer into a snapshot must carry the object
that pins that snapshot. Current examples are `CrossDecl::file`,
`MemberAccess::file`, `MemberCompletion::owner.file`, and
`fblang::IncomingCall::caller`. The per-file hit list in `onWorkspaceSymbol`
holds `shared_ptr<IndexedFile const>` for the same reason.

Holding the `WorkspaceIndex` is not the pin. A scan can replace entries
inside the index while another thread uses a result obtained from it.

A pointer that must outlive a handler follows the same rule. LspCpp's
`SelectionRange::parent` is a non-owning `optional<SelectionRange*>`.
`src/selection_lsp.h` and `src/selection_lsp.cpp` therefore place every
ancestor in a `thread_local std::deque` that is cleared once per request.

That design depends on the current send path. `sendSessionMessage`
serializes the reply inline on the handler thread under the send mutex, and
`ResponseOrError(T&&)` moves the response. The batch therefore has one
thread, one arena, and no extra lock. Build all chains for a
`selectionRanges` call together. Separate calls for each position would
clear the arena and leave earlier chains pointing into reused storage.

The selection-range integration test checks the nested `parent` chain in
the actual wire reply. Keep that assertion when changing this area. It tests
the lifetime claim instead of assuming it.

### Other concurrency rules

- An offset is meaningful only with the content it was measured against.
  The object that returns an offset must also supply the matching line
  table. This is one reason a scan must not reparse an open buffer. A
  fixture can create the same mismatch by writing bytes the client never
  sent: text-mode output can turn LF into CRLF on MSVC while the `didOpen`
  text carries LF bytes.
- Change a condition variable's predicate state while holding the mutex the
  waiter uses. An atomic flag does not provide the required lock discipline
  and can lose the notification. A shutdown wait also needs a timeout or
  the lock discipline that guarantees progress.
- Only one thread owns a `std::thread` object. Take `scannerMu_` for every
  `join()` and every assignment to `scanner_`.

## Diagnosing hangs and silent failures

A hanging suite should identify the test that was running.
`session_integration` prints a flushed `[ RUN ] <test>` line for each test.
CI passes a CTest timeout, and the captured output for a timed-out suite
shows the last test reached.

Silence is not a pass. On MSVC, every exit path from `main` can be quiet. A
bare CTest failure with no reason can mean the process never returned.

The harness provides these markers:

- `RUN_TEST` prints `[ DONE ]` around each test.
- An escaping exception is caught so the failing test is named and counted.
- `main` prints a final line after all tests run.
- `std::set_terminate` reports whether an exception was active when the
  process terminated.

If the final line is missing, the failure is inside a test. If it is
present, look at teardown or static destruction.

Keep portable markers in the shared harness. A Windows unhandled-exception
filter was removed because it printed an address that could not be resolved
in a Release build and pulled `<Windows.h>` into a cross-platform test file.
Add a platform reporter only when a crash is localized to a test and the
existing markers are demonstrably insufficient.

A platform-specific trigger still needs a local reproduction when one can
be built. For example, `TestHomeFolderGuardAsksTheFilesystem` sets `HOME` to
a symlink and then to a `..` spelling. It proves that the home folder's own
project stays out of the index and can fail on Linux before the defect
reaches a Windows runner.

## Fixtures and wire frames

### Build real file URIs

Do not build a URI as `"file://" + path.string()`. Use LspCpp's
`make_file_scheme_uri` with the result of `fblang::normalizePath`. That is
the same pair of calls the server uses when it returns a URI, so the test and
server produce the same bytes on every platform.

A backslash in a JSON string is an escape. A literal Windows path inside a
frame can therefore change the path before the server sees it. Use the
`{{tmp}}` placeholder that `MakeLspFrame` expands for the platform instead
of a hard-coded path such as `file:///tmp/x.bas`.

A fixture that constructs wire data through string surgery can fail because
the input is malformed, not because the server returned the wrong answer.
Read the actual frame before investigating the server.

### Raw strings can end earlier than they look

This literal is a trap:

```cpp
R"(","value":"")"
```

The `)"` sequence is the raw string terminator. The resulting literal ends
after `:"`, and the remaining quote starts another empty string. The frame
never parses and the handler never runs. A poll then reports a timeout.

When a poll times out and the newest reply is empty, print and inspect the
frame before reading server code.

### Scope a poll to the intended reply

`PollRequest` searches the cumulative output stream. A `didOpen` already
publishes diagnostics, so a document name or other common text can occur in
the stream before the request being tested is answered.

Match against the part of the reply the caller will inspect, usually its
tail. When a poll exhausts its budget, it should report that result. To the
caller, a poll that gave up and a wrong answer are both failed assertions.
