/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The integration driver's main().
//
// The suite is one executable, not one per feature file: the harness in
// session_support.{h,cpp} defines RUN_TEST and the reporters, so the files are
// translation units, and main is what calls each one's runner. Keeping a single
// process is deliberate, because the suite's diagnostics are a property of the
// process (see the [ RUN ] / [ DONE ] notes in session_support.h) and because
// `ctest --timeout` prints one stream that names the test it died in.
//
// A new feature file adds a `void Run<Feature>Tests()` that calls RUN_TEST for
// each of its tests, a declaration below, and a call in the same order. The
// order matters only for readability of a `--filter` run, but keeping it fixed
// means a filtered run reproduces the unfiltered sequence.
//
// Naming the groups is also how a failure is triaged: the [ DONE ] marker
// carries the test's name, and that name's file is the one to open.
#include "session_support.h"

#include <cstdio>
#include <exception>

// One declaration per feature file, in the order main calls them below.
namespace fbtest {
void RunCoreTests();
void RunHoverTests();
void RunNavigationTests();
void RunCompletionTests();
void RunCodeActionsTests();
void RunWorkspaceTests();
void RunIndexesTests();
void RunCrossfileTests();
void RunRenameTests();
void RunSemanticTokensTests();
void RunSettingsTests();
void RunAnalysisTests();
void RunCallHierarchyTests();
void RunCodeLensTests();
void RunPullDiagnosticsTests();
} // namespace fbtest

int main(int argc, char **argv) {
  test::InitTestFilter(argc, argv);
  // Covers what escapes the tests: a destructor, static teardown, a noexcept
  // violation, a joinable std::thread. It reports whether an exception was even
  // active, because a bare terminate is itself the diagnosis.
  std::set_terminate(fbtest::ReportUncaught);

  fbtest::RunCoreTests();
  fbtest::RunHoverTests();
  fbtest::RunNavigationTests();
  fbtest::RunCompletionTests();
  fbtest::RunCodeActionsTests();
  fbtest::RunWorkspaceTests();
  fbtest::RunIndexesTests();
  fbtest::RunCrossfileTests();
  fbtest::RunRenameTests();
  fbtest::RunSemanticTokensTests();
  fbtest::RunSettingsTests();
  fbtest::RunAnalysisTests();
  fbtest::RunCallHierarchyTests();
  fbtest::RunCodeLensTests();
  fbtest::RunPullDiagnosticsTests();

  // The last line of a healthy run. If it is missing, main never got here, and
  // the last [ DONE ] names the test the process died in; if it is present, the
  // death was after the suite — during teardown, static destruction, or a
  // thread that outlived it. Those are different bugs with different fixes, and
  // a log that cannot tell them apart costs a CI run per guess.
  std::printf("[ DONE  ] all tests ran: %d failure(s), %d skipped\n",
              test::Failures(), test::SkippedTests());
  std::fflush(stdout);
  return test::Failures() == 0 ? 0 : 1;
}
