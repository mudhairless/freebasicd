/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fbc_version.h"

#include "symbols.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <system_error>
#include <thread>
#include <tuple>

namespace fblang {

namespace {

bool parseNumber(std::string_view text, std::size_t *pos, int *out) {
  std::size_t const begin = *pos;
  long long value = 0;
  while (*pos < text.size() && text[*pos] >= '0' && text[*pos] <= '9') {
    value = (value * 10) + (text[*pos] - '0');
    if (value > std::numeric_limits<int>::max()) {
      return false;
    }
    ++*pos;
  }
  if (*pos == begin) {
    return false;
  }
  *out = static_cast<int>(value);
  return true;
}

// Quote one argument for the platform shell `std::system` uses. The executable
// path is user-supplied (fbc.path), so a space or quote must not split it.
std::string shellQuote(std::string const &s) {
#ifdef _WIN32
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') {
      out += "\"\"";
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
#else
  std::string out = "'";
  for (char const c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += '\'';
  return out;
#endif
}

// Assemble the version-probe command line. On Windows, `std::system` hands
// the string to `cmd.exe /c`, which strips the first and last quote characters
// on the line when the line holds more than one quoted string: here the
// executable's opening quote and the capture path's closing quote are the pair
// removed, which leaves the command malformed ("The filename, directory name,
// or volume label syntax is incorrect"). One extra pair around the whole
// command is the pair cmd strips instead, leaving both quoted paths intact.
// Keeping the wrap here lets the caller hold the final command as `const` on
// every platform.
std::string probeCommand(std::string const &executable,
                         std::string const &capture) {
  std::string command =
      shellQuote(executable) + " -version > " + shellQuote(capture) + " 2>&1";
#ifdef _WIN32
  command = "\"" + command + "\"";
#endif
  return command;
}

} // namespace

std::string FbcVersion::toString() const {
  return std::to_string(major) + "." + std::to_string(minor) + "." +
         std::to_string(patch);
}

bool operator==(FbcVersion const &a, FbcVersion const &b) {
  return std::tie(a.major, a.minor, a.patch) ==
         std::tie(b.major, b.minor, b.patch);
}

bool operator!=(FbcVersion const &a, FbcVersion const &b) { return !(a == b); }

bool operator<(FbcVersion const &a, FbcVersion const &b) {
  return std::tie(a.major, a.minor, a.patch) <
         std::tie(b.major, b.minor, b.patch);
}

bool operator<=(FbcVersion const &a, FbcVersion const &b) { return !(b < a); }

bool operator>(FbcVersion const &a, FbcVersion const &b) { return b < a; }

bool operator>=(FbcVersion const &a, FbcVersion const &b) { return !(a < b); }

std::optional<FbcVersion> parseFbcVersion(std::string_view output) {
  for (std::size_t i = 0; i < output.size(); ++i) {
    if (output[i] < '0' || output[i] > '9') {
      continue;
    }
    // A dotted component must start at a token boundary, so a number in the
    // middle of `1.2.3.4` (or after another dot) is not accepted as a
    // version.
    if (i > 0 && ((output[i - 1] >= '0' && output[i - 1] <= '9') ||
                  output[i - 1] == '.')) {
      continue;
    }
    std::size_t pos = i;
    int major = 0;
    int minor = 0;
    int patch = 0;
    if (!parseNumber(output, &pos, &major) || pos >= output.size() ||
        output[pos] != '.') {
      continue;
    }
    ++pos;
    if (!parseNumber(output, &pos, &minor) || pos >= output.size() ||
        output[pos] != '.') {
      continue;
    }
    ++pos;
    if (!parseNumber(output, &pos, &patch)) {
      continue;
    }
    // Reject a fourth dotted component and any trailing digits: `1.2.3.4`
    // and `1.2.34` are not the shape fbc reports.
    if (pos < output.size() &&
        ((output[pos] >= '0' && output[pos] <= '9') || output[pos] == '.')) {
      continue;
    }
    return FbcVersion{major, minor, patch};
  }
  return std::nullopt;
}

std::optional<FbcVersion>
probeFbcVersion(std::filesystem::path const &executable,
                std::chrono::milliseconds timeout) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(executable, ec)) {
    return std::nullopt;
  }
  std::filesystem::path const tempDir =
      std::filesystem::temp_directory_path(ec);
  if (ec) {
    return std::nullopt;
  }
  static std::atomic<unsigned long long> counter{0};
  auto const stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path const capture =
      tempDir / ("freebasicd-fbc-" + std::to_string(stamp) + "-" +
                 std::to_string(counter.fetch_add(1)) + ".txt");
  std::string const command =
      probeCommand(executable.string(), capture.string());

  // `std::system` has no timeout, so the child runs on its own thread and the
  // caller waits only for the bound. A timeout detaches the worker instead of
  // joining it: a hung compiler must not stall startup, and a future's
  // destructor would block on the same hang.
  auto const promise = std::make_shared<std::promise<int>>();
  std::future<int> done = promise->get_future();
  std::thread worker([command, promise] {
    // The command is the user's own configured compiler, shell-quoted in
    // shellQuote; see TIDY.md, bugprone-command-processor.
    // NOLINTNEXTLINE(bugprone-command-processor, cert-env33-c)
    int const status = std::system(command.c_str());
    promise->set_value(status);
  });
  if (done.wait_for(timeout) != std::future_status::ready) {
    worker.detach();
    return std::nullopt;
  }
  worker.join();
  (void)done.get();

  std::ifstream in(capture, std::ios::binary);
  if (!in) {
    std::filesystem::remove(capture, ec);
    return std::nullopt;
  }
  std::string const text{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
  in.close();
  std::filesystem::remove(capture, ec);
  return parseFbcVersion(text);
}

bool versionAllows(std::optional<FbcVersion> const &version,
                   Diagnostic const &finding) {
  if (!version) {
    return true;
  }
  if (finding.since && *version < *finding.since) {
    return false;
  }
  if (finding.until && *version > *finding.until) {
    return false;
  }
  return true;
}

} // namespace fblang
