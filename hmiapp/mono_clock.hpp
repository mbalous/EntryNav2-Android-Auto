// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Time since the head unit started, for delays: the car's date can be set at any moment (GPS), this clock cannot.
#ifndef ENTRY_AAD_HMIAPP_MONO_CLOCK_HPP
#define ENTRY_AAD_HMIAPP_MONO_CLOCK_HPP

#include <stdint.h>
#include <time.h>

namespace evo {
namespace app {

// 64 bits: long is 32 bits on the car, and its milliseconds went past 2^31 after 24.8 days of this clock.
inline int64_t monoMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}
inline long monoSeconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long>(ts.tv_sec);
}

}  // namespace app
}  // namespace evo

#endif
