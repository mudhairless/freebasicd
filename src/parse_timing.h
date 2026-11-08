/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <chrono>
#include <cstdio>
#include <string>

namespace fblang {

// Report how long parsing one file took. The display uses two decimals
// below ten units and none at or above it.
inline void logParseDuration(std::string const &path,
                             std::chrono::steady_clock::duration dt) {
  constexpr double kTwoDecimalThreshold = 10.0;
  auto const ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(dt).count();
  auto const ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();
  if (ms > 10) {
    (void)std::fprintf(stderr, "[freebasicd] %s parsed in %lld ms\n",
                       path.c_str(), static_cast<long long>(ms));
  } else if (ns == 0) {
    (void)std::fprintf(stderr, "[freebasicd] %s parsed in <1 us\n",
                       path.c_str());
  } else if (dt < std::chrono::microseconds(1)) {
    (void)std::fprintf(stderr, "[freebasicd] %s parsed in %lld ns\n",
                       path.c_str(), static_cast<long long>(ns));
  } else if (dt < std::chrono::milliseconds(1)) {
    double const us =
        std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(
            dt)
            .count();
    if (us < kTwoDecimalThreshold) {
      (void)std::fprintf(stderr, "[freebasicd] %s parsed in %.2f us\n",
                         path.c_str(), us);
    } else {
      (void)std::fprintf(stderr, "[freebasicd] %s parsed in %.0f us\n",
                         path.c_str(), us);
    }
  } else {
    double const msecs =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
            dt)
            .count();
    if (msecs >= kTwoDecimalThreshold) {
      (void)std::fprintf(stderr, "[freebasicd] %s parsed in %lld ms\n",
                         path.c_str(), static_cast<long long>(ms));
    } else {
      (void)std::fprintf(stderr, "[freebasicd] %s parsed in %.2f ms\n",
                         path.c_str(), msecs);
    }
  }
}

} // namespace fblang
