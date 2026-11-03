/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace fblang {

struct Diagnostic;

// The dotted version a resolved `fbc` reports, for example `1.10.2`. Only the
// dotted number is part of the value: `fbc -version` also embeds a build date
// and a platform triple, and both are deliberately ignored. The name avoids
// confusion with `FBLANG_VERSION`, which is this server's own version.
struct FbcVersion {
  int major = 0;
  int minor = 0;
  int patch = 0;

  std::string toString() const;
};

bool operator==(FbcVersion const &a, FbcVersion const &b);
bool operator!=(FbcVersion const &a, FbcVersion const &b);
bool operator<(FbcVersion const &a, FbcVersion const &b);
bool operator<=(FbcVersion const &a, FbcVersion const &b);
bool operator>(FbcVersion const &a, FbcVersion const &b);
bool operator>=(FbcVersion const &a, FbcVersion const &b);

// Parse the dotted version out of `fbc -version` output. The first complete
// `N.N.N` token wins; surrounding prose, a build date, and the platform triple
// are irrelevant. Returns nullopt for empty output, an unparsable one, or a
// number that is not exactly three dotted components.
std::optional<FbcVersion> parseFbcVersion(std::string_view output);

// The default bound on a version probe: long enough for a cold `fbc
// -version` on a slow disk, short enough that a hung one cannot stall
// workspace adoption.
inline constexpr std::chrono::milliseconds kFbcProbeTimeout{2000};

// Run `executable -version` and parse its output. The call is bounded by
// `timeout`: a compiler that hangs, is missing, or writes nothing usable leaves
// the result unknown rather than stalling startup. The capture uses only
// standard C++ and a shell redirection to a temporary file, so the same code
// serves Linux, macOS, and Windows/MSVC.
std::optional<FbcVersion> probeFbcVersion(
    std::filesystem::path const &executable,
    std::chrono::milliseconds timeout = kFbcProbeTimeout);

// The delivery gate for version-aware findings. A finding with no bounds
// applies to every version; an unknown probe version never filters anything
// (the server then behaves exactly as it did before version awareness). The
// `until` bound is inclusive.
bool versionAllows(std::optional<FbcVersion> const &version,
                   Diagnostic const &finding);

} // namespace fblang
